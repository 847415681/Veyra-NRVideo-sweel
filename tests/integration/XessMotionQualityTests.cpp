// Headless motion-quality check for the XeSS present path (2026-10-12, follow-up to
// FgXessBoundaryTests). The same deterministic scenes (side-running character over a
// panning background, the static-character orbit and the dense rails) run through the
// product EnhanceGraph with motion source "optical flow"; the motion the XeSS presenter
// would receive is read back and scored against the exact scene motion. There is no window,
// provider or screen capture, so it can run beside other work; what XeSS makes of the
// motion stays the job of FgXessBoundaryTests.
// Each run scores, on the same frames, the previous path (VEYRA_TEST_FLOW_REFINE=0), the
// photometric-only refinement (VEYRA_TEST_FLOW_CONSISTENCY=0) and the default refinement. Regions (current frame, crop X0..X0+CW as the capture test):
//   bgBand    background within Band px of a part edge, not uncovered
//   uncovered background whose previous position was covered by the character
//   fgBand    character within Band px of a part edge
//   far       background beyond Band px
// usage: veyra_xess_motion_quality_tests <outDir> <run|orbit|bars> <speedPx> [tag]
//        veyra_xess_motion_quality_tests <outDir> perf <frames> [tag]
// perf: product extents (3840x2160 source/work, 1920x1080 flow, the run scene upscaled), GPU
// time of the optical-flow stage (NVOF wait, densify, refinement, adaptation) and of the whole
// graph per path; median and P95 after 8 warm-up frames.
// Knobs (environment) as FgXessBoundaryTests: VEYRA_TEST_FLOW_FULL=1, VEYRA_TEST_FLOW_QUALITY,
// VEYRA_TEST_FLOW_VALIDATE=0, VEYRA_TEST_NVOF_GRID (read by EnhanceGraph).
#include "veyra/pipeline/EnhanceGraph.h"
#include "veyra/pipeline/GpuPassUtils.h"
#include "veyra/gfx/D3D12DeviceContext.h"
#include "veyra/gfx/CommandSlotRing.h"
#include "veyra/sink/ImageExportSink.h"
#include "veyra/RuntimePaths.h"
#include "veyra/Log.h"
#include <DirectXPackedVector.h>
#include <d3d12sdklayers.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <iostream>
#include <numbers>
#include <string>
#include <thread>
#include <map>
#include <vector>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}
using namespace veyra;
using Microsoft::WRL::ComPtr;
namespace {
constexpr int W=2560,H=1440,FlowW=1280,FlowH=720,N=16,Warmup=2,Band=40,X0=480,CW=1600;

float hash01(int x,int y,uint32_t seed){
    uint32_t v=uint32_t(x)*0x9e3779b9u^uint32_t(y)*0x85ebca6bu^seed*0xc2b2ae35u;
    v^=v>>16;v*=0x7feb352du;v^=v>>15;v*=0x846ca68bu;v^=v>>16;return float(v&0xffffu)/65535.f;
}
float noise(float x,float y,float cell,uint32_t seed){
    const float gx=x/cell,gy=y/cell;const int ix=int(std::floor(gx)),iy=int(std::floor(gy));
    float tx=gx-ix,ty=gy-iy;tx=tx*tx*(3-2*tx);ty=ty*ty*(3-2*ty);
    const float a=hash01(ix,iy,seed),b=hash01(ix+1,iy,seed),c=hash01(ix,iy+1,seed),d=hash01(ix+1,iy+1,seed);
    return (a*(1-tx)+b*tx)*(1-ty)+(c*(1-tx)+d*tx)*ty;
}
template<class F> void parallelRows(int rows,F&& f){
    const int n=std::max(1u,std::thread::hardware_concurrency());std::vector<std::jthread> pool;
    for(int t=0;t<n;++t)pool.emplace_back([&,t]{for(int y=t;y<rows;y+=n)f(y);});
}
// Scene identical to FgXessBoundaryTests (parts back to front, limbs swing about a pivot).
struct Part{float px,py,len,r;int kind;float swing;bool limb;};
constexpr Part kParts[]={
    {1280,560,300,45,3,+.8f,true},   // back arm
    {1280,900,400,58,2,-1.f,true},   // back leg
    {1280,520,340,150,1,0,false},    // torso: capsule from (1280,520) downwards
    {1280,380,0,80,0,0,false},       // head
    {1280,900,400,58,2,+1.f,true},   // front leg
    {1280,560,300,45,3,-.8f,true}};  // front arm
constexpr std::array<std::array<float,3>,4> kBase{{{205,160,130},{48,66,118},{46,46,52},{52,70,124}}};
struct Scene {
    bool run=true;int speed=0;std::vector<std::array<uint8_t,3>> background;int bw=0;
    float angle(const Part& p,double t)const{
        if(!p.limb||!run)return 0;
        return float(p.swing*35.0*std::numbers::pi/180.0*std::sin(2*std::numbers::pi*t/8.0));
    }
    int bgOffset(double t)const{return int(std::lround(t*speed));}
    void local(const Part& p,double t,float x,float y,float& u,float& v)const{
        const float a=angle(p,t),dx=std::sin(a),dy=std::cos(a);const float rx=x-p.px,ry=y-p.py;
        u=rx*dx+ry*dy;v=rx*dy-ry*dx;
    }
    float sdf(const Part& p,double t,float x,float y)const{
        float u,v;local(p,t,x,y,u,v);
        if(u<0)return std::hypot(u,v)-p.r;if(u>p.len)return std::hypot(u-p.len,v)-p.r;return std::abs(v)-p.r;
    }
    std::array<float,3> partColor(const Part& p,double t,float x,float y,int index)const{
        float u,v;if(p.limb)local(p,t,x,y,u,v);else{u=y;v=x;}
        const float n=.7f+.6f*noise(u+300.f*index,v+500.f*index,7,21+index);
        const float stripe=(int(std::floor(u/18))&1)?16.f:0.f;
        const auto& c=kBase[size_t(p.kind)];
        return {c[0]*n+stripe,c[1]*n+stripe,c[2]*n+stripe};
    }
    std::array<uint8_t,3> pixel(int x,int y,double t,int* top=nullptr,float* edge=nullptr)const{
        const auto& b=background[size_t(y)*bw+x+bgOffset(t)];
        float c[3]={float(b[0]),float(b[1]),float(b[2])};int id=-1;float nearest=1e9f;
        for(int i=0;i<6;++i){
            const auto& p=kParts[i];const float d=sdf(p,t,x+.5f,y+.5f);nearest=std::min(nearest,std::abs(d));
            const float a=std::clamp(.5f-d,0.f,1.f);if(a<=0)continue;
            const auto pc=partColor(p,t,x+.5f,y+.5f,i);
            for(int k=0;k<3;++k)c[k]=c[k]*(1-a)+std::clamp(pc[k],0.f,255.f)*a;
            if(a>=.5f)id=i;
        }
        if(top)*top=id;if(edge)*edge=nearest;
        return {uint8_t(std::lround(c[0])),uint8_t(std::lround(c[1])),uint8_t(std::lround(c[2]))};
    }
    std::array<float,2> ideal(int x,int y,int frame,int top)const{
        if(top<0)return {float(speed),0.f};
        const auto& p=kParts[top];if(!p.limb||!run)return {0.f,0.f};
        float u,v;local(p,frame,x+.5f,y+.5f,u,v);
        const float a=angle(p,frame-1),dx=std::sin(a),dy=std::cos(a);
        const float px=p.px+u*dx+v*dy,py=p.py+u*dy-v*dx;
        return {px-(x+.5f),py-(y+.5f)};
    }
};
Scene makeScene(bool run,int speed,bool bars){
    Scene s;s.run=run;s.speed=speed;s.bw=W+(N+2)*speed;s.background.resize(size_t(s.bw)*H);
    parallelRows(H,[&](int y){
        for(int x=0;x<s.bw;++x){
            const float n=.5f*noise(float(x),float(y),80,1)+.3f*noise(float(x),float(y),20,2)+.2f*noise(float(x),float(y),5,3);
            const float t=float(y)/H;float r=110*(1-t)+86*t,g=138*(1-t)+112*t,b=168*(1-t)+70*t;
            const float m=.55f+.9f*n;r*=m;g*=m;b*=m;
            if(x%211<13){r*=.35f;g*=.32f;b*=.3f;}
            if((y/120)%3==1&&x%67<4){r=std::min(255.f,r+60);g=std::min(255.f,g+55);b=std::min(255.f,b+40);}
            if(bars&&y<H*3/4){const float rail=x%22<4?.18f:1.f;r=(150.f+20.f*n)*rail;g=(160.f+18.f*n)*rail;b=(170.f+16.f*n)*rail;}
            s.background[size_t(y)*s.bw+x]={uint8_t(std::clamp(r,0.f,255.f)),uint8_t(std::clamp(g,0.f,255.f)),uint8_t(std::clamp(b,0.f,255.f))};
        }
    });
    return s;
}
std::vector<uint8_t> readTexture(gfx::D3D12DeviceContext& ctx,gfx::CommandSlotRing& ring,
    pipeline::EnhanceGraph& graph,ID3D12Resource* texture) {
    if(!texture)return {};
    const auto desc=texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT rows=0;UINT64 rowBytes=0,total=0;
    ctx.device()->GetCopyableFootprints(&desc,0,1,0,&footprint,&rows,&rowBytes,&total);
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width=total;buffer.Height=1;buffer.DepthOrArraySize=buffer.MipLevels=1;
    buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    if(FAILED(ctx.device()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,
        D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback))))return {};
    Status status=Status::Ok;unsigned slot=0;auto* list=ring.acquireNext(slot,status);if(!list)return {};
    const auto state=graph.diagnosticResourceState(texture);
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={texture,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,state,D3D12_RESOURCE_STATE_COPY_SOURCE};
    if(state!=D3D12_RESOURCE_STATE_COPY_SOURCE)list->ResourceBarrier(1,&barrier);
    D3D12_TEXTURE_COPY_LOCATION dst{},src{};
    dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=footprint;
    src.pResource=texture;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    if(state!=D3D12_RESOURCE_STATE_COPY_SOURCE){std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);list->ResourceBarrier(1,&barrier);}
    if(!ring.submitAndSignal(slot)||!ring.waitIdle())return {};
    void* data=nullptr;D3D12_RANGE range{0,SIZE_T(total)};if(FAILED(readback->Map(0,&range,&data)))return {};
    std::vector<uint8_t> result(size_t(rowBytes)*rows);
    for(UINT y=0;y<rows;++y)std::memcpy(result.data()+size_t(y)*rowBytes,
        static_cast<uint8_t*>(data)+footprint.Offset+size_t(y)*footprint.Footprint.RowPitch,size_t(rowBytes));
    D3D12_RANGE written{0,0};readback->Unmap(0,&written);return result;
}
struct Region {
    double sum=0;uint64_t n=0,over1=0,over4=0;
    void add(float e){sum+=e;++n;over1+=e>1;over4+=e>4;}
    std::string json()const{return std::format("{{\"epe\":{:.4f},\"over1Pct\":{:.3f},\"over4Pct\":{:.3f},\"pixels\":{}}}",n?sum/n:-1.0,n?100.0*over1/n:-1.0,n?100.0*over4/n:-1.0,n);}
};
// Crop images for inspection: EPE x16 (white = 16 px wrong) and motion (128 + 2 px per level).
bool save(const std::filesystem::path& path,int size,int cx,int cy,auto&& colour){
    sink::RgbaImage out;out.width=out.height=uint32_t(size);out.pixels.resize(size_t(size)*size*4);
    const int x0=std::clamp(cx-size/2,X0,X0+CW-size),y0=std::clamp(cy-size/2,0,H-size);
    for(int y=0;y<size;++y)for(int x=0;x<size;++x){const auto c=colour(x0+x,y0+y);auto* q=&out.pixels[(size_t(y)*size+x)*4];q[0]=c[0];q[1]=c[1];q[2]=c[2];q[3]=255;}
    std::error_code ec;std::filesystem::remove(path,ec);return sink::saveImage(path.wstring(),out);
}
uint8_t level(float v){return uint8_t(std::clamp(128.f+v*2.f,0.f,255.f));}
std::string percentiles(std::vector<double> v){
    if(v.empty())return "null";std::sort(v.begin(),v.end());
    return std::format("{{\"median\":{:.4f},\"p95\":{:.4f},\"samples\":{}}}",v[v.size()/2],v[std::min(v.size()-1,size_t(v.size()*.95))],v.size());
}
int perf(const std::filesystem::path& dir,int frames,const std::string& label){
    constexpr unsigned PW=3840,PH=2160;
    const Scene scene=makeScene(true,40,false);
    std::vector<std::vector<std::array<uint8_t,3>>> source(N);
    for(int k=0;k<N;++k){source[k].resize(size_t(W)*H);parallelRows(H,[&](int y){for(int x=0;x<W;++x)source[k][size_t(y)*W+x]=scene.pixel(x,y,k);});}
    gfx::D3D12DeviceContext ctx;gfx::CommandSlotRing ring;Status st=Status::Ok;gfx::DeviceContextDesc dd;
    if(!ctx.initialize(dd,st)||!ring.initialize(ctx.device(),ctx.directQueue(),ctx.fence(),ctx.fenceEvent(),6,st))return 2;
    AVFrame* f=av_frame_alloc();if(!f)return 2;
    f->width=PW;f->height=PH;f->format=AV_PIX_FMT_RGBA;f->color_range=AVCOL_RANGE_JPEG;f->color_trc=AVCOL_TRC_IEC61966_2_1;f->colorspace=AVCOL_SPC_RGB;
    if(av_frame_get_buffer(f,32)<0)return 2;
    constexpr const char* kPaths[]={"previous","photometric","refined"};bool ok=true;
    for(int path=0;path<3&&ok;++path){
        SetEnvironmentVariableW(L"VEYRA_TEST_FLOW_REFINE",path?L"1":L"0");
        SetEnvironmentVariableW(L"VEYRA_TEST_FLOW_CONSISTENCY",path==1?L"0":nullptr);
        pipeline::EnhanceGraph graph(ctx,ring);pipeline::EnhanceGraphDesc gd;
        gd.sourceWidth=gd.workWidth=PW;gd.sourceHeight=gd.workHeight=PH;gd.flowWidth=PW/2;gd.flowHeight=PH/2;gd.rgbInput=true;
        gd.enableNr=false;gd.enableSr=false;gd.enableFg=true;gd.noNgx=true;gd.fgMultiplier=2;
        gd.frameGenerationBackend=engine::FrameGenerationBackend::XeSS;gd.fgMotion=engine::MotionSource::OpticalFlow;
        gd.runtimeAbsPath=runtime::localRuntimeDirectory().wstring();
        if(!graph.initialize(gd)||!graph.createViews()){std::cerr<<"graph open failed"<<std::endl;ok=false;break;}
        graph.recordGpuTimings();std::vector<double> flow,span;
        for(int k=0;k<frames&&ok;++k){
            const auto& s=source[size_t(k%N)];
            parallelRows(int(PH),[&](int y){auto* row=f->data[0]+size_t(y)*f->linesize[0];const int sy=y*H/int(PH);for(int x=0;x<int(PW);++x){const auto& c=s[size_t(sy)*W+x*W/int(PW)];row[x*4]=c[0];row[x*4+1]=c[1];row[x*4+2]=c[2];row[x*4+3]=255;}});
            pipeline::EnhanceGraph::FrameOutputs out;
            ok=graph.process(f,k*(1000.0/30),k==0,out,uint64_t(k+1))&&ring.waitIdle();
            for(const auto& timing:graph.takeGpuTimings()){
                if(timing.identity.sourceFrameId<=8)continue;
                if(const auto& v=timing.gpu[size_t(diagnostics::GpuStage::Flow)];v.milliseconds)flow.push_back(*v.milliseconds);
                if(const auto whole=diagnostics::graphExecutionSpanMs(timing))span.push_back(*whole);
            }
        }
        std::cout<<std::format("XESS_MOTION_PERF {{\"label\":\"{}\",\"path\":\"{}\",\"flow\":\"{}x{}\",\"nvof\":{},\"flowStageMs\":{},\"graphMs\":{}}}",
            label,kPaths[path],graph.flowWidth(),graph.flowHeight(),graph.metrics().nvofExecuteCount,percentiles(flow),percentiles(span))<<std::endl;
        ok=ring.drainQueue()&&ok;graph.shutdown();
    }
    av_frame_free(&f);ring.shutdown();ctx.shutdown();
    return ok?0:1;
}
} // namespace

int wmain(int argc,wchar_t** argv){
    if(argc!=4&&argc!=5)return 2;
    const std::filesystem::path dir=argv[1];const std::wstring sceneName=argv[2];const int speed=_wtoi(argv[3]);
    if(sceneName==L"perf"){std::filesystem::create_directories(dir);const std::string label=std::format("perf{}",argc==5?"-"+std::filesystem::path(argv[4]).string():std::string());
        Logger::instance().openFile((dir/(label+".log")).wstring());return speed>=16&&speed<=600?perf(dir,speed,label):2;}
    const bool run=sceneName==L"run",bars=sceneName==L"bars";if(!run&&!bars&&sceneName!=L"orbit")return 2;if(speed<1||speed>200)return 2;
    std::filesystem::create_directories(dir);CoInitializeEx(nullptr,COINIT_MULTITHREADED); // WIC image export
    const std::string label=std::format("motion-{}{}{}",bars?"bars":run?"run":"orbit",speed,argc==5?"-"+std::filesystem::path(argv[4]).string():std::string());
    Logger::instance().openFile((dir/(label+".log")).wstring());
    const Scene scene=makeScene(run,speed,bars);
    gfx::D3D12DeviceContext ctx;gfx::CommandSlotRing ring;Status st=Status::Ok;gfx::DeviceContextDesc dd;dd.enableDebugLayer=true;
    if(!ctx.initialize(dd,st)||!ring.initialize(ctx.device(),ctx.directQueue(),ctx.fence(),ctx.fenceEvent(),6,st))return 2;
    auto knob=[](const wchar_t* name)->std::wstring{wchar_t v[16]{};return GetEnvironmentVariableW(name,v,16)?std::wstring(v):std::wstring();};
    AVFrame* f=av_frame_alloc();if(!f)return 2;
    f->width=W;f->height=H;f->format=AV_PIX_FMT_RGBA;f->color_range=AVCOL_RANGE_JPEG;f->color_trc=AVCOL_TRC_IEC61966_2_1;f->colorspace=AVCOL_SPC_RGB;
    if(av_frame_get_buffer(f,32)<0)return 2;
    std::vector<int> tops(size_t(W)*H),previousTops(size_t(W)*H);std::vector<float> edges(size_t(W)*H);
    const int scoreH=bars?H*3/4:H,shown=N-2;bool ok=true;unsigned scored=0;
    std::vector<float> shownError[3];
    constexpr const char* kPaths[]={"previous","photometric","refined"};
    for(int path=0;path<3;++path){
        const bool refine=path>0;
        SetEnvironmentVariableW(L"VEYRA_TEST_FLOW_REFINE",refine?L"1":L"0");
        SetEnvironmentVariableW(L"VEYRA_TEST_FLOW_CONSISTENCY",path==1?L"0":nullptr);
        pipeline::EnhanceGraph graph(ctx,ring);pipeline::EnhanceGraphDesc gd;
        gd.sourceWidth=gd.workWidth=W;gd.sourceHeight=gd.workHeight=H;gd.flowWidth=FlowW;gd.flowHeight=FlowH;gd.rgbInput=true;
        gd.enableNr=false;gd.enableSr=false;gd.enableFg=true;gd.noNgx=true;gd.fgMultiplier=2;
        gd.frameGenerationBackend=engine::FrameGenerationBackend::XeSS;gd.fgMotion=engine::MotionSource::OpticalFlow;
        if(knob(L"VEYRA_TEST_FLOW_FULL")==L"1"){gd.flowWidth=W;gd.flowHeight=H;}
        if(const auto q=knob(L"VEYRA_TEST_FLOW_QUALITY");!q.empty())gd.flowQuality=engine::FlowQuality(std::clamp(_wtoi(q.c_str()),0,2));
        if(knob(L"VEYRA_TEST_FLOW_VALIDATE")==L"0")gd.validateMotion=false;
        gd.runtimeAbsPath=runtime::localRuntimeDirectory().wstring();
        if(!graph.initialize(gd)||!graph.createViews()){std::cerr<<"graph open failed"<<std::endl;ok=false;break;}
        Region bgBand,uncovered,fgBand,outside,all,parts[6];std::vector<uint16_t> motion(size_t(W)*H*2);
        std::map<std::string,std::array<std::vector<float>,3>> samples;
        for(int k=0;k<N&&ok;++k){
            std::swap(tops,previousTops);
            parallelRows(H,[&](int y){auto* row=f->data[0]+size_t(y)*f->linesize[0];for(int x=0;x<W;++x){int top=-1;float edge=0;const auto p=scene.pixel(x,y,k,&top,&edge);tops[size_t(y)*W+x]=top;edges[size_t(y)*W+x]=edge;row[x*4]=p[0];row[x*4+1]=p[1];row[x*4+2]=p[2];row[x*4+3]=255;}});
            pipeline::EnhanceGraph::FrameOutputs out;
            ok=graph.process(f,k*(1000.0/30),k==0,out,uint64_t(k+1))&&ring.waitIdle();
            if(!ok||k<Warmup)continue;
            if(!graph.presentMotionValid(out.videoSlot)){std::cerr<<"motion not valid at frame "<<k<<std::endl;ok=false;break;}
            const auto bytes=readTexture(ctx,ring,graph,graph.presentMotion(out.videoSlot));
            if(bytes.size()!=motion.size()*2){ok=false;break;}
            std::memcpy(motion.data(),bytes.data(),bytes.size());
            auto at=[&](int x,int y){const size_t i=(size_t(y)*W+x)*2;return std::array<float,2>{DirectX::PackedVector::XMConvertHalfToFloat(motion[i]),DirectX::PackedVector::XMConvertHalfToFloat(motion[i+1])};};
            for(int y=0;y<scoreH;++y)for(int x=X0;x<X0+CW;++x){
                const size_t i=size_t(y)*W+x;const int top=tops[i];const auto v=scene.ideal(x,y,k,top);const auto m=at(x,y);
                const float e=std::hypot(m[0]-v[0],m[1]-v[1]);all.add(e);
                if(k==shown)shownError[path].push_back(e);
                if(top>=0){if(edges[i]<Band){fgBand.add(e);parts[top].add(e);}continue;}
                const int px=std::clamp(int(std::lround(x+v[0])),0,W-1),py=std::clamp(int(std::lround(y+v[1])),0,H-1);
                if(previousTops[size_t(py)*W+px]>=0)uncovered.add(e);else if(edges[i]<Band)bgBand.add(e);else outside.add(e);
            }
            if(const auto refinedBytes=readTexture(ctx,ring,graph,graph.diagnosticRefinedFlow());!refinedBytes.empty()){
                const unsigned fw=graph.flowWidth(),fh=unsigned(refinedBytes.size()/(size_t(fw)*8));const auto* r=reinterpret_cast<const uint16_t*>(refinedBytes.data());
                for(unsigned fy=0;fy<fh;fy+=2)for(unsigned fx=0;fx<fw;fx+=2){
                    const int x=int((fx+.5)*W/fw),y=int((fy+.5)*H/fh);if(x<X0||x>=X0+CW||y>=scoreH)continue;
                    const size_t i=size_t(y)*W+x;const int top=tops[i];const auto v=scene.ideal(x,y,k,top);std::string region;
                    if(top>=0)region=edges[i]<Band?(kParts[top].limb?"limbBand":"bodyBand"):"inner";
                    else{const int px=std::clamp(int(std::lround(x+v[0])),0,W-1),py=std::clamp(int(std::lround(y+v[1])),0,H-1);region=previousTops[size_t(py)*W+px]>=0?"uncovered":edges[i]<Band?"bgBand":"far";}
                    auto half=[&](int c){return DirectX::PackedVector::XMConvertHalfToFloat(r[(size_t(fy)*fw+fx)*4+c]);};
                    auto& s=samples[region];s[0].push_back(half(2));s[1].push_back(half(3));s[2].push_back(half(3)/std::max(1.f,std::hypot(half(0),half(1))));
                }
            }
            ++scored;
            if(k==shown){
                const std::string stem=label+"-"+kPaths[path];
                auto epe=[&](int x,int y){const auto v=scene.ideal(x,y,k,tops[size_t(y)*W+x]);const auto m=at(x,y);const auto e=uint8_t(std::min(255.f,16.f*std::hypot(m[0]-v[0],m[1]-v[1])));return std::array<uint8_t,3>{e,e,e};};
                auto vec=[&](int x,int y){const auto m=at(x,y);return std::array<uint8_t,3>{level(m[0]),level(m[1]),128};};
                ok=save(dir/(stem+"-legs-epe.png"),448,1280,1250,epe)&&save(dir/(stem+"-legs-motion.png"),448,1280,1250,vec)&&
                   save(dir/(stem+"-arms-epe.png"),448,1280,650,epe)&&save(dir/(stem+"-arms-motion.png"),448,1280,650,vec);
                if(ok&&!refine){auto truth=[&](int x,int y){const auto v=scene.ideal(x,y,k,tops[size_t(y)*W+x]);return std::array<uint8_t,3>{level(v[0]),level(v[1]),128};};
                    ok=save(dir/(label+"-ideal-legs-motion.png"),448,1280,1250,truth)&&save(dir/(label+"-ideal-arms-motion.png"),448,1280,650,truth);}
            }
        }
        const auto m=graph.metrics();
        std::cout<<std::format("XESS_MOTION_QUALITY {{\"label\":\"{}\",\"path\":\"{}\",\"flow\":\"{}x{}\",\"nvof\":{},\"all\":{},\"bgBand\":{},\"uncovered\":{},\"fgBand\":{},\"far\":{},\"parts\":[{},{},{},{},{},{}]}}",
            label,kPaths[path],graph.flowWidth(),gd.flowHeight?gd.flowHeight:H,m.nvofExecuteCount,all.json(),bgBand.json(),uncovered.json(),fgBand.json(),outside.json(),parts[0].json(),parts[1].json(),parts[2].json(),parts[3].json(),parts[4].json(),parts[5].json())<<std::endl;
        for(auto& [region,s]:samples){std::string text;
            for(auto& values:s){std::sort(values.begin(),values.end());auto q=[&](double f){return values.empty()?-1.f:values[size_t(f*(values.size()-1))];};text+=std::format("[{:.3f},{:.3f},{:.3f},{:.3f}]",q(.1),q(.5),q(.9),q(.97));}
            std::cout<<std::format("XESS_REFINE_STATS {} {} n={} cost/mismatch/relative p10,p50,p90,p97 {}",kPaths[path],region,s[0].size(),text)<<std::endl;}
        ok=ring.drainQueue()&&ok;graph.shutdown();
    }
    if(ok&&shownError[1].size()==size_t(scoreH)*CW&&shownError[2].size()==shownError[1].size()){
        auto delta=[&](int x,int y)->std::array<uint8_t,3>{if(y>=scoreH)return {0,0,0};const size_t i=size_t(y)*CW+(x-X0);const float d=shownError[2][i]-shownError[1][i];
            const auto m=uint8_t(std::min(255.f,4.f*std::abs(d)));return std::abs(d)<1?std::array<uint8_t,3>{40,40,40}:d>0?std::array<uint8_t,3>{m,0,0}:std::array<uint8_t,3>{0,m,0};};
        ok=save(dir/(label+"-delta-legs.png"),448,1280,1250,delta)&&save(dir/(label+"-delta-arms.png"),448,1280,650,delta);
    }
    unsigned errors=0;ComPtr<ID3D12InfoQueue> iq;
    if(SUCCEEDED(ctx.device()->QueryInterface(IID_PPV_ARGS(&iq))))
        for(UINT64 i=0;i<iq->GetNumStoredMessages();++i){SIZE_T bytes=0;if(FAILED(iq->GetMessage(i,nullptr,&bytes)))continue;std::vector<uint8_t>b(bytes);auto* msg=reinterpret_cast<D3D12_MESSAGE*>(b.data());
            if(SUCCEEDED(iq->GetMessage(i,msg,&bytes))&&msg->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++errors;std::cerr<<msg->pDescription<<std::endl;}}
    const bool pass=ok&&!errors&&scored==3u*(N-Warmup);
    std::cout<<std::format("XESS_MOTION_QUALITY_DONE {{\"label\":\"{}\",\"scoredFrames\":{},\"errors\":{},\"pass\":{}}}",label,scored,errors,pass?"true":"false")<<std::endl;
    av_frame_free(&f);iq.Reset();ring.shutdown();ctx.shutdown();CoUninitialize();
    return pass?0:1;
}
