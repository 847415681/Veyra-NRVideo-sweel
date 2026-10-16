#include "veyra/pipeline/GpuPassUtils.h"
#include "veyra/gfx/D3D12DeviceContext.h"
#include "veyra/gfx/CommandSlotRing.h"
#include "veyra/ngx/NvOfSession.h"
#include <d3d12sdklayers.h>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
using namespace veyra;using namespace veyra::pipeline;
namespace {
struct Cleanup{std::function<void()> fn;~Cleanup(){fn();}};
float texture(int x,int y){
    // Deterministic, non-periodic broad-band field, translated without resampling.
    uint32_t v=uint32_t(x/4)*747796405u+uint32_t(y/4)*2891336453u+12345u;
    v=((v>>((v>>28)+4))^v)*277803737u;v=(v>>22)^v;
    return float(40+(v%176));
}
double median(std::vector<double> x){std::sort(x.begin(),x.end());return x[x.size()/2];}
int run(unsigned w,unsigned h,bool both,bool occlusion){
    gfx::D3D12DeviceContext ctx;gfx::CommandSlotRing ring;Status st;gfx::DeviceContextDesc dd;dd.enableDebugLayer=true;
    if(!ctx.initialize(dd,st)||!ring.initialize(ctx.device(),ctx.directQueue(),ctx.fence(),ctx.fenceEvent(),4,st))return 2;
    const unsigned gw=(w+3)/4,gh=(h+3)/4;
    std::array<ComPtr<ID3D12Resource>,6> tex;
    for(unsigned i=0;i<6;++i)tex[i]=makeTexture(ctx.device(),i<2?w:gw,i<2?h:gh,i<2?DXGI_FORMAT_B8G8R8A8_UNORM:i%2==0?DXGI_FORMAT_R16G16_SINT:DXGI_FORMAT_R8_UINT,false);
    for(auto& t:tex)if(!t)return 2;
    ComPtr<ID3D12Fence> ready;if(FAILED(ctx.device()->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&ready))))return 2;
    ngx::NvOfSession session;
    bool cleaned=false,cleanupOk=true;
    Cleanup cleanup{[&]{if(cleaned)return;cleaned=true;cleanupOk=ring.drainQueue();if(session.initialized())cleanupOk=session.unregisterAll(st)&&cleanupOk;for(auto& t:tex)t.Reset();session.shutdown();}};
    ngx::NvOfSession::Desc desc;desc.width=w;desc.height=h;desc.inFence=ctx.fence();desc.outFence=ready.Get();
    if(both){desc.reverseFlow=tex[4].Get();desc.reverseCost=tex[5].Get();}
    if(both){
        auto invalid=desc;invalid.reverseFlow=tex[5].Get();
        if(session.initialize(ctx.device(),tex[0].Get(),tex[1].Get(),tex[2].Get(),tex[3].Get(),invalid,st)||session.initialized()||st!=Status::InvalidArgument)return 2;
        session.shutdown();invalid=desc;invalid.reverseFlow=tex[2].Get();
        if(session.initialize(ctx.device(),tex[0].Get(),tex[1].Get(),tex[2].Get(),tex[3].Get(),invalid,st)||session.initialized()||st!=Status::InvalidArgument)return 2;
        session.shutdown();
    }
    if(!session.initialize(ctx.device(),tex[0].Get(),tex[1].Get(),tex[2].Get(),tex[3].Get(),desc,st))return 2;
    if(session.nextOutValue()!=1||ready->GetCompletedValue()!=0)return 2;
    // A malformed paired call must preserve the initialized session.
    auto malformed=desc;malformed.reverseFlow=tex[4].Get();malformed.reverseCost=nullptr;
    if(session.initialize(ctx.device(),tex[0].Get(),tex[1].Get(),tex[2].Get(),tex[3].Get(),malformed,st)||!session.initialized())return 2;
    std::vector<ComPtr<ID3D12Resource>> uploads;StateTracker states;uint32_t slot=0;auto* list=ring.acquireNext(slot,st);if(!list)return 2;
    const unsigned pitch=(w*4+255)&~255u;
    for(unsigned i=0;i<2;++i){
        auto up=makeUploadBuffer(ctx.device(),size_t(pitch)*h);void* ptr=nullptr;if(!up||FAILED(up->Map(0,nullptr,&ptr)))return 2;
        for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
            auto* p=static_cast<uint8_t*>(ptr)+size_t(y)*pitch+x*4;
            const bool novel=occlusion&&i==1&&x>=w/3&&x<w*2/3&&y>=h/3&&y<h*2/3;
            uint8_t v=uint8_t(novel?128:texture(int(x)+(i?8:0),int(y)));
            p[0]=p[1]=p[2]=v;p[3]=255;
        }
        up->Unmap(0,nullptr);states.transition(list,tex[i].Get(),D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION a{},b{};a.pResource=tex[i].Get();a.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;b.pResource=up.Get();b.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;b.PlacedFootprint.Footprint={DXGI_FORMAT_B8G8R8A8_UNORM,w,h,1,pitch};list->CopyTextureRegion(&a,0,0,0,&b,nullptr);states.transition(list,tex[i].Get(),D3D12_RESOURCE_STATE_COMMON);uploads.push_back(up);
    }
    if(!ring.submitAndSignal(slot)||!ring.waitIdle())return 2;
    auto batch=[&](unsigned n){for(unsigned i=0;i<n;++i)if(!session.execute(ring.lastSignaledValue(),st))return false;
        if(FAILED(ctx.directQueue()->Wait(ready.Get(),session.nextOutValue()-1)))return false;
        return ring.drainQueue();};
    if(!batch(4))return 2;
    const auto start=std::chrono::steady_clock::now();if(!batch(60))return 2;
    const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()/60;
    const unsigned fp=(gw*4+255)&~255u,cp=(gw+255)&~255u;
    std::array<size_t,4> offsets{};size_t bytes=0;
    for(unsigned i=0;i<4;++i){
        offsets[i]=(bytes+D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT-1)&~size_t(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT-1);
        bytes=offsets[i]+size_t(i%2?cp:fp)*gh;
    }
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=bytes;bd.Height=1;bd.DepthOrArraySize=bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> rb;if(FAILED(ctx.device()->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&rb))))return 2;
    list=ring.acquireNext(slot,st);if(!list)return 2;
    for(unsigned i=0;i<(both?4u:2u);++i){states.transition(list,tex[i+2].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION a{},b{};a.pResource=tex[i+2].Get();a.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;b.pResource=rb.Get();b.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;b.PlacedFootprint.Offset=offsets[i];b.PlacedFootprint.Footprint={tex[i+2]->GetDesc().Format,gw,gh,1,i%2?cp:fp};list->CopyTextureRegion(&b,0,0,0,&a,nullptr);states.transition(list,tex[i+2].Get(),D3D12_RESOURCE_STATE_COMMON);}
    if(!ring.submitAndSignal(slot)||!ring.waitIdle())return 2;
    void* ptr=nullptr;D3D12_RANGE range{0,bytes};if(FAILED(rb->Map(0,&range,&ptr)))return 2;
    auto vectorAt=[&](unsigned which,unsigned x,unsigned y){auto* p=reinterpret_cast<int16_t*>(static_cast<uint8_t*>(ptr)+offsets[which]+y*fp+x*4);return std::array<float,2>{p[0]/32.f,p[1]/32.f};};
    std::vector<double> errors,reverseErrors;
    unsigned novelTotal=0,novelBase=0,novelConsistent=0,goodTotal=0,goodBase=0,goodConsistent=0;
    for(unsigned y=8;y+8<gh;++y)for(unsigned x=8;x+8<gw;++x){
        const float px=float(x*4),py=float(y*4);auto v=vectorAt(0,x,y);float qx=px+v[0],qy=py+v[1];
        const bool novel=occlusion&&px>=w/3+8&&px<w*2/3-8&&py>=h/3+8&&py<h*2/3-8;
        const bool clean=!occlusion||px<w/3-16||px>=w*2/3+16||py<h/3-16||py>=h*2/3+16;
        if(clean){errors.push_back(std::hypot(v[0]-8,v[1]));if(both){auto r=vectorAt(2,x,y);reverseErrors.push_back(std::hypot(r[0]+8,r[1]));}}
        if(!novel&&!clean)continue;
        bool base=static_cast<uint8_t*>(ptr)[offsets[1]+y*cp+x]<32&&qx>=0&&qx<w-1&&qy>=0&&qy<h-1;
        // Same encoded-luma endpoint test as product's rejection boundary.
        if(base){int ix=int(qx),iy=int(qy);float tx=qx-ix,ty=qy-iy;float a=texture(ix,iy)*(1-tx)+texture(ix+1,iy)*tx,b=texture(ix,iy+1)*(1-tx)+texture(ix+1,iy+1)*tx;
            float current=novel?128:texture(int(px)+8,int(py));base=std::abs(a*(1-ty)+b*ty-current)/255<.15f;}
        bool consistent=base;
        if(both&&base){unsigned rx=std::min(unsigned(qx/4),gw-1),ry=std::min(unsigned(qy/4),gh-1);auto r=vectorAt(2,rx,ry);consistent=static_cast<uint8_t*>(ptr)[offsets[3]+ry*cp+rx]<32&&std::hypot(v[0]+r[0],v[1]+r[1])<1.5f;}
        if(novel){++novelTotal;novelBase+=base;novelConsistent+=consistent;}else{++goodTotal;goodBase+=base;goodConsistent+=consistent;}
    }
    D3D12_RANGE written{0,0};rb->Unmap(0,&written);
    bool ok=!errors.empty()&&median(errors)<=1&&(!both||median(reverseErrors)<=1);
    UINT64 reverseBytes=0;for(unsigned i=4;i<6;++i){auto d=tex[i]->GetDesc();reverseBytes+=ctx.device()->GetResourceAllocationInfo(0,1,&d).SizeInBytes;}
    cleanup.fn();ok=ok&&cleanupOk;
    ComPtr<ID3D12InfoQueue> iq;if(FAILED(ctx.device()->QueryInterface(IID_PPV_ARGS(&iq))))return 2;unsigned debugErrors=0;
    for(UINT64 i=0;i<iq->GetNumStoredMessages();++i){SIZE_T n=0;if(FAILED(iq->GetMessage(i,nullptr,&n)))return 2;std::vector<uint8_t>b(n);auto* m=reinterpret_cast<D3D12_MESSAGE*>(b.data());if(FAILED(iq->GetMessage(i,m,&n)))return 2;if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++debugErrors;std::cerr<<m->pDescription<<std::endl;}}
    std::cout<<"BIDIRECTIONAL width="<<w<<" height="<<h<<" both="<<both<<" occlusion="<<occlusion<<" meanBatchMs="<<ms<<" reverseAllocationBytes="<<reverseBytes<<" medianEpe="<<median(errors)<<" reverseMedianEpe="<<(both?median(reverseErrors):-1)<<" novel="<<novelTotal<<" novelBaseAccepted="<<novelBase<<" novelConsistent="<<novelConsistent<<" good="<<goodTotal<<" goodBaseAccepted="<<goodBase<<" goodConsistent="<<goodConsistent<<" debugErrors="<<debugErrors<<" pass="<<(ok&&!debugErrors)<<std::endl;
    return ok&&!debugErrors?0:1;
}
int registrationLifecycle(bool both,unsigned rejectAt){
    gfx::D3D12DeviceContext ctx;gfx::CommandSlotRing ring;Status st;gfx::DeviceContextDesc dd;dd.enableDebugLayer=true;
    if(!ctx.initialize(dd,st)||!ring.initialize(ctx.device(),ctx.directQueue(),ctx.fence(),ctx.fenceEvent(),4,st))return 2;
    std::array<ComPtr<ID3D12Resource>,6> tex;
    for(unsigned i=0;i<6;++i)tex[i]=makeTexture(ctx.device(),i<2?640:160,i<2?360:90,
        i<2?DXGI_FORMAT_B8G8R8A8_UNORM:i%2==0?DXGI_FORMAT_R16G16_SINT:DXGI_FORMAT_R8_UINT,false);
    for(auto& t:tex)if(!t)return 2;
    ComPtr<ID3D12Fence> ready;if(FAILED(ctx.device()->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&ready))))return 2;
    ngx::NvOfSession session;bool cleaned=false,cleanupOk=true;
    Cleanup cleanup{[&]{if(cleaned)return;cleaned=true;cleanupOk=ring.drainQueue();
        if(session.initialized())cleanupOk=session.unregisterAll(st)&&cleanupOk;
        for(auto& t:tex)t.Reset();session.shutdown();}};
    ngx::NvOfSession::Desc desc;desc.width=640;desc.height=360;desc.inFence=ctx.fence();desc.outFence=ready.Get();
    if(both){desc.reverseFlow=tex[4].Get();desc.reverseCost=tex[5].Get();}
    const auto reject=std::to_wstring(rejectAt);
    SetEnvironmentVariableW(L"VEYRA_TEST_NVOF_REGISTER_FAILURE",rejectAt?reject.c_str():nullptr);
    const bool initialized=session.initialize(ctx.device(),tex[0].Get(),tex[1].Get(),tex[2].Get(),tex[3].Get(),desc,st);
    SetEnvironmentVariableW(L"VEYRA_TEST_NVOF_REGISTER_FAILURE",nullptr);
    // Registration-only setup/rollback must not advertise completed flow.
    bool ok=ready->GetCompletedValue()==0&&session.nextOutValue()==1&&session.executeCount()==0;
    ok=ok&&(rejectAt?(!initialized&&!session.initialized()&&st==Status::DeviceFailure):(initialized&&session.initialized()));
    cleanup.fn();ok=ok&&cleanupOk;
    ComPtr<ID3D12InfoQueue> iq;if(FAILED(ctx.device()->QueryInterface(IID_PPV_ARGS(&iq))))return 2;unsigned errors=0;
    for(UINT64 i=0;i<iq->GetNumStoredMessages();++i){SIZE_T n=0;if(FAILED(iq->GetMessage(i,nullptr,&n)))return 2;
        std::vector<uint8_t>b(n);auto* m=reinterpret_cast<D3D12_MESSAGE*>(b.data());if(FAILED(iq->GetMessage(i,m,&n)))return 2;
        if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++errors;std::cerr<<m->pDescription<<std::endl;}}
    std::cout<<"REGISTRATION_LIFECYCLE both="<<both<<" rejectAt="<<rejectAt<<" debugErrors="<<errors
        <<" noFlowPublished="<<(ready->GetCompletedValue()==0)<<" pass="<<(ok&&!errors)<<std::endl;
    return ok&&!errors?0:1;
}
}
int main(int argc,char** argv){unsigned w=argc>1?unsigned(std::stoi(argv[1])):640,h=argc>2?unsigned(std::stoi(argv[2])):360;if(w<128||h<128||w>3840||h>2160)return 2;
    for(bool both:{false,true})for(unsigned rejected:{0u,2u,4u}){int r=registrationLifecycle(both,rejected);if(r)return r;}
    if(int r=registrationLifecycle(true,6))return r;
    for(bool occlusion:{false,true})for(bool both:{false,true}){int r=run(w,h,both,occlusion);if(r)return r;}return 0;}
