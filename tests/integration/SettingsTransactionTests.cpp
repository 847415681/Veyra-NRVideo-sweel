#include "veyra/engine/VideoPresenter.h"
#include "veyra/pipeline/EnhanceGraph.h"
#include "veyra/gfx/D3D12DeviceContext.h"
#include "veyra/gfx/CommandSlotRing.h"
#include "veyra/Log.h"
#include "veyra/sink/ImageExportSink.h"
#include "veyra/RuntimePaths.h"
#include <d3d12sdklayers.h>
#include <filesystem>
#include <iostream>
#include <vector>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}
#include "HdrCopyPresentationFixture.h"

// A real external COM reference reproduces the one-chain-per-HWND ownership
// hazard without injecting anything or changing a third-party overlay.
int wmain(int argc,wchar_t** argv){
    using namespace veyra;if(argc!=2)return 2;
    const std::filesystem::path dir=argv[1];std::filesystem::create_directories(dir);Logger::instance().openFile((dir/L"engine.log").wstring());
    gfx::D3D12DeviceContext ctx;gfx::CommandSlotRing ring;Status status;gfx::DeviceContextDesc device;device.enableDebugLayer=true;
    if(!ctx.initialize(device,status)||!ring.initialize(ctx.device(),ctx.directQueue(),ctx.fence(),ctx.fenceEvent(),4,status))return 2;
    gfx::ComPtr<ID3D12InfoQueue> messages;ctx.device()->QueryInterface(IID_PPV_ARGS(&messages));if(!messages)return 2;messages->ClearStoredMessages();
    HWND window=CreateWindowExW(WS_EX_NOACTIVATE,L"STATIC",L"Veyra retained swapchain",WS_POPUP,30,30,320,180,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);if(!window)return 2;
    engine::VideoPresenter presenter;gfx::ComPtr<IDXGISwapChain3> held;bool ok=true;
    AVFrame* frame=av_frame_alloc();frame->format=AV_PIX_FMT_RGBA;frame->width=320;frame->height=180;if(av_frame_get_buffer(frame,32)<0)return 2;
    for(unsigned round=0;round<8&&ok;++round){
        if(!presenter.prepareGraphChange()){ok=false;break;}
        pipeline::EnhanceGraph graph(ctx,ring);pipeline::EnhanceGraphDesc desc;
        desc.sourceWidth=320;desc.sourceHeight=180;desc.workWidth=round%2?640:320;desc.workHeight=round%2?360:180;
        desc.nrWidth=desc.workWidth;desc.nrHeight=desc.workHeight;desc.rgbInput=true;desc.enableNr=desc.enableFg=desc.enableSr=false;desc.noFeatures=desc.noNgx=true;
        ok=graph.initialize(desc)&&graph.createViews()&&presenter.open(ctx,window,graph);
        if(!held)held=presenter.swapChainForTest();
        const bool same=held&&held.Get()==presenter.swapChainForTest().Get();ok=ok&&same;
        for(int y=0;y<frame->height;++y)for(int x=0;x<frame->width;++x){auto* p=frame->data[0]+size_t(y)*frame->linesize[0]+x*4;p[0]=uint8_t(25+round*20);p[1]=70;p[2]=120;p[3]=255;}
        pipeline::EnhanceGraph::FrameOutputs out;
        ok=ok&&graph.process(frame,round*33.333333,true,out,round+1)&&presenter.present(ctx,ring,graph,out.videoSlot,false,true,0,false,.5f,out.batch.identity);
        sink::RgbaImage image;
        ok=ok&&presenter.readPresentedFrameForTest(ctx,ring,image);
        if(ok){const size_t at=(size_t(image.height/2)*image.width+image.width/2)*4;ok=std::abs(int(image.pixels[at])-int(25+round*20))<=2;}
        std::cout<<"SETTINGS_REUSE round="<<round<<" retainedExternalReference="<<same<<" generation="<<presenter.generation()<<" passed="<<ok<<std::endl;
        ok=presenter.prepareGraphChange()&&ring.drainQueue()&&ok;out={};graph.shutdown();
    }
    held.Reset();presenter.close();av_frame_free(&frame);
    // The field failure was NR on/off while HDR10 and DLSS 2X remained on.
    // Keep an external chain reference during that exact graph transition,
    // then compare the actually presented 10-bit pixels with the graph output.
    frame=av_frame_alloc();frame->format=AV_PIX_FMT_P010LE;frame->width=640;frame->height=360;
    frame->color_range=AVCOL_RANGE_MPEG;frame->colorspace=AVCOL_SPC_BT2020_NCL;
    frame->color_primaries=AVCOL_PRI_BT2020;frame->color_trc=AVCOL_TRC_SMPTE2084;
    ok=av_frame_get_buffer(frame,32)>=0&&ok;
    unsigned hdrChecks=0;int maxError=0;
    for(unsigned round=0;round<8&&ok;++round){
        ok=presenter.prepareGraphChange();
        pipeline::EnhanceGraph graph(ctx,ring);pipeline::EnhanceGraphDesc desc;
        desc.sourceWidth=desc.workWidth=desc.nrWidth=640;desc.sourceHeight=desc.workHeight=desc.nrHeight=360;
        desc.hdrInput=desc.hdrOutput=true;desc.captureBitDepth=10;
        desc.enableNr=round%2!=0;desc.enableFg=true;desc.enableSr=false;desc.fgMultiplier=2;
        desc.enableNvofStandalone=desc.enableNr;desc.nrRuntime=engine::NrRuntime::NvidiaOriginal;
        desc.runtimeAbsPath=runtime::localRuntimeDirectory().wstring();
        ok=ok&&graph.initialize(desc)&&graph.createViews()&&presenter.open(ctx,window,graph);
        engine::PresentationSettings presentation;
        presentation.display=engine::DisplaySync::Tearing;
        presentation.hdrFgCompatibility=round%4>=2;
        std::wstring notice;
        const auto effective=presenter.configurePresentation(ctx,presentation,true,notice);
        ok=ok&&effective.hdrFgCompatibility==presentation.hdrFgCompatibility;
        if(!held)held=presenter.swapChainForTest();
        const bool same=held&&held.Get()==presenter.swapChainForTest().Get();ok=ok&&same;
        for(int y=0;y<360;++y)for(int x=0;x<640;++x)reinterpret_cast<uint16_t*>(frame->data[0]+size_t(y)*frame->linesize[0])[x]=uint16_t((240+round*32)<<6);
        for(int y=0;y<180;++y)for(int x=0;x<640;++x)reinterpret_cast<uint16_t*>(frame->data[1]+size_t(y)*frame->linesize[1])[x]=512<<6;
        pipeline::EnhanceGraph::FrameOutputs out;
        for(unsigned i=0;i<3&&ok;++i){
            out={};ok=graph.process(frame,(round*3+i)*33.333333,i==0,out,round*3+i+1)&&ring.waitIdle();
            ok=ok&&presenter.present(ctx,ring,graph,out.videoSlot,false,false,0,false,.5f,out.batch.identity);
        }
        auto shown=presenter.presentedResourceForTest();std::vector<uint32_t> actual,expected;unsigned w=0,h=0,ew=0,eh=0;
        ok=ok&&readHdrWords(ctx,ring,shown.Get(),actual,w,h)&&readHdrWords(ctx,ring,graph.videoFrameResource(out.videoSlot),expected,ew,eh);
        if(ok){
            const auto a=actual[size_t(h/2)*w+w/2],b=expected[size_t(eh/2)*ew+ew/2];
            for(unsigned c=0;c<3;++c)maxError=std::max(maxError,std::abs(int((a>>(10*c))&1023)-int((b>>(10*c))&1023)));
            ok=maxError<=1;++hdrChecks;
        }
        shown.Reset();std::cout<<"SETTINGS_HDR round="<<round<<" nr="<<desc.enableNr<<" copyCompatibility="<<effective.hdrFgCompatibility<<" retainedExternalReference="<<same<<" checks="<<hdrChecks<<" max10BitError="<<maxError<<" passed="<<ok<<std::endl;
        ok=presenter.prepareGraphChange()&&ring.drainQueue()&&ok;out={};graph.shutdown();
    }
    held.Reset();presenter.close();av_frame_free(&frame);
    unsigned errors=0;for(UINT64 i=0;i<messages->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T size=0;messages->GetMessage(i,nullptr,&size);std::vector<char> buffer(size);auto* m=reinterpret_cast<D3D12_MESSAGE*>(buffer.data());messages->GetMessage(i,m,&size);if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++errors;std::cerr<<m->pDescription<<std::endl;}}
    ok=ok&&errors==0&&SUCCEEDED(ctx.device()->GetDeviceRemovedReason());messages.Reset();ring.shutdown();ctx.shutdown();DestroyWindow(window);
    std::cout<<"SETTINGS_RESULT passed="<<ok<<" debugErrors="<<errors<<std::endl;return ok?0:1;
}
