// Real GPU checks for the XeSS-only motion branch. Pixel readback and waits are
// confined to this test; the product keeps its asynchronous GPU path.
#include "veyra/pipeline/EnhanceGraph.h"
#include "veyra/pipeline/GpuPassUtils.h"
#include "veyra/gfx/D3D12DeviceContext.h"
#include "veyra/gfx/CommandSlotRing.h"
#include "veyra/Log.h"
#include <DirectXPackedVector.h>
#include <d3d12sdklayers.h>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <vector>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}
using namespace veyra;
using Microsoft::WRL::ComPtr;
namespace {
unsigned checks=0, failures=0;
void check(bool ok,const char* why) { ++checks;if(!ok){++failures;std::cerr<<"FAIL "<<why<<'\n';} }

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
bool zero(const std::vector<uint8_t>& bytes) {
    if(bytes.empty()||bytes.size()%2)return false;
    for(size_t i=0;i<bytes.size();i+=2){uint16_t h;std::memcpy(&h,bytes.data()+i,2);if((h&0x7fff)!=0)return false;}
    return true;
}
bool finite(const std::vector<uint8_t>& bytes) {
    if(bytes.empty()||bytes.size()%2)return false;
    for(size_t i=0;i<bytes.size();i+=2){uint16_t h;std::memcpy(&h,bytes.data()+i,2);if(!std::isfinite(DirectX::PackedVector::XMConvertHalfToFloat(h)))return false;}
    return true;
}
struct Frame {std::vector<uint8_t> motion,flow,video;};
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=2)return 2;
    const std::filesystem::path out=argv[1];std::filesystem::create_directories(out);
    Logger::instance().openFile((out/L"engine.log").wstring());
    gfx::D3D12DeviceContext ctx;gfx::CommandSlotRing ring;Status status=Status::Ok;
    gfx::DeviceContextDesc dd;dd.enableDebugLayer=true;
    if(!ctx.initialize(dd,status)||!ring.initialize(ctx.device(),ctx.directQueue(),ctx.fence(),ctx.fenceEvent(),6,status))return 2;
    ComPtr<ID3D12InfoQueue> iq;ctx.device()->QueryInterface(IID_PPV_ARGS(&iq));if(!iq)return 2;
    AVFrame* frame=av_frame_alloc();if(!frame)return 2;
    frame->format=AV_PIX_FMT_RGBA;frame->width=640;frame->height=360;
    frame->color_range=AVCOL_RANGE_JPEG;frame->colorspace=AVCOL_SPC_RGB;frame->color_trc=AVCOL_TRC_IEC61966_2_1;
    if(av_frame_get_buffer(frame,32)<0)return 2;
    std::vector<Frame> legacy;unsigned changed=0;
    for(unsigned mode=0;mode<3;++mode) {
        SetEnvironmentVariableW(L"VEYRA_TEST_FLOW_REFINE",mode==0?L"0":L"1");
        pipeline::EnhanceGraph graph(ctx,ring);pipeline::EnhanceGraphDesc desc;
        desc.sourceWidth=640;desc.sourceHeight=360;desc.workWidth=1280;desc.workHeight=720;
        desc.flowWidth=320;desc.flowHeight=180;desc.rgbInput=true;
        desc.enableNr=desc.enableSr=false;desc.enableFg=true;desc.noNgx=true;
        desc.enableNvofStandalone=true;desc.fgMultiplier=2;
        desc.frameGenerationBackend=engine::FrameGenerationBackend::XeSS;
        desc.fgMotion=mode==2?engine::MotionSource::Automatic:engine::MotionSource::OpticalFlow;
        const bool initialized=graph.initialize(desc)&&graph.createViews();check(initialized,"graph initializes");if(!initialized)break;
        for(unsigned k=0;k<8;++k) {
            for(int y=0;y<frame->height;++y)for(int x=0;x<frame->width;++x) {
                auto* p=frame->data[0]+size_t(y)*frame->linesize[0]+x*4;
                const bool body=x>245&&x<385&&y>70&&y<300;
                const int moving=x+int(k)*12;
                p[0]=uint8_t(body?180:35+(moving/7+y/5)%90);
                p[1]=uint8_t(body?60:50+(moving/11+y/9)%100);
                p[2]=uint8_t(body?45:30+(moving/17+y/7)%80);p[3]=255;
            }
            const bool reset=k==0||k==4;pipeline::EnhanceGraph::FrameOutputs output;
            const bool processed=graph.process(frame,k*(1000.0/30),reset,output,k+1)&&ring.waitIdle();
            check(processed,"frame processes");if(!processed)break;
            Frame got;
            got.motion=readTexture(ctx,ring,graph,graph.presentMotion(output.videoSlot));
            got.flow=readTexture(ctx,ring,graph,graph.flowResource());
            got.video=readTexture(ctx,ring,graph,graph.videoFrameResource(output.videoSlot));
            check(finite(got.motion),"XeSS motion is finite");
            check(!got.video.empty(),"real output exists");
            check(graph.presentMotionValid(output.videoSlot)==!reset,"history validity resets together");
            if(reset||mode==2)check(zero(got.motion),"first/reset/automatic frame receives zero motion");
            else check(!zero(got.motion),"optical-flow frame receives actual motion");
            if(mode==0)legacy.push_back(std::move(got));
            else if(k<legacy.size()) {
                check(got.video==legacy[k].video,"XeSS motion repair never changes source/real output");
                if(!reset)check(got.flow==legacy[k].flow,"original validated NR/SR guidance stays identical");
                if(mode==1&&!reset&&got.motion!=legacy[k].motion)++changed;
            }
        }
        check(graph.metrics().nvofExecuteCount==6,"NVOF executes six valid frame pairs around reset");
        check(ring.drainQueue(),"bounded teardown drains");graph.shutdown();
    }
    check(changed>0,"refinement changes actual XeSS motion");
    unsigned errors=0;
    for(UINT64 i=0;i<iq->GetNumStoredMessages();++i){SIZE_T n=0;iq->GetMessage(i,nullptr,&n);std::vector<uint8_t> bytes(n);auto* message=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
        if(SUCCEEDED(iq->GetMessage(i,message,&n))&&message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++errors;std::cerr<<message->pDescription<<'\n';}}
    check(errors==0,"D3D12 debug errors zero");check(SUCCEEDED(ctx.device()->GetDeviceRemovedReason()),"device remains healthy");
    av_frame_free(&frame);iq.Reset();ring.shutdown();ctx.shutdown();
    std::cout<<"XESS_MOTION_CONTRACT checks="<<checks<<" failures="<<failures<<" changedPairs="<<changed<<" debugErrors="<<errors<<std::endl;
    return failures?1:0;
}
