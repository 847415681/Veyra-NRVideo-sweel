#include "veyra/pipeline/EnhanceGraph.h"
#include "veyra/engine/GraphDescription.h"
#include "veyra/gfx/D3D12DeviceContext.h"
#include "veyra/gfx/CommandSlotRing.h"
#include "veyra/sink/ImageExportSink.h"
#include "veyra/source/MediaFileSource.h"
#include "veyra/RuntimePaths.h"
#include "veyra/Log.h"
#include <filesystem>
#include <iostream>
#include <array>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

// Matched deterministic frames, actual SDK evaluation and output pixels.
// Readbacks are restricted to this test; these are not playback timings.
int wmain(int argc,wchar_t** argv){
    using namespace veyra;
    if(argc<2)return 2;
    const HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    if(FAILED(com))return 2;
    const bool baseline=argc>2&&std::wstring_view(argv[2])==L"baseline";
    source::MediaFileSource media;const AVFrame* natural=nullptr;
    if(argc>3){
        source::SourceOpenDesc open;open.path=argv[3];open.preferHardwareDecode=false;
        if(!media.open(open)||!media.seek({12,1}))return 2;
        pipeline::FramePacket packet;
        if(media.read(packet,&natural)!=source::SourceReadStatus::Frame||!natural)return 2;
    }
    const std::filesystem::path directory=argv[1];std::filesystem::create_directories(directory);
    Logger::instance().openFile((directory/L"engine.log").wstring());
    gfx::D3D12DeviceContext ctx;gfx::CommandSlotRing ring;Status status;
    gfx::DeviceContextDesc device;
    if(!ctx.initialize(device,status)||!ring.initialize(ctx.device(),ctx.directQueue(),ctx.fence(),ctx.fenceEvent(),4,status))return 2;
    bool ok=true;
    for(unsigned quality:{0u,4u,5u})for(bool still:{false,true})for(bool exportJob:{false,true}){
        engine::EnhancementSettings settings;settings.nr=false;settings.sr=true;settings.multiplier=1;
        settings.videoSrQuality=quality;settings.srTarget=pipeline::SrTarget::Qhd;
        engine::StageRequest request;request.width=2560;request.height=1440;request.sr=true;request.stillImage=still;request.exportJob=exportJob;
        pipeline::EnhanceGraphDesc desc;engine::describeStages(request,settings,desc);
        const bool expected=!baseline&&quality==4&&!still;
        const bool passed=desc.enableSr==expected;ok=passed&&ok;
        std::cout<<"SR_POLICY quality="<<quality<<" stillImage="<<still<<" exportJob="<<exportJob<<" nativeActive="<<desc.enableSr<<" passed="<<passed<<std::endl;
    }
    auto render=[&](unsigned quality,bool native,bool color,bool enabled,sink::RgbaImage& image){
        engine::EnhancementSettings settings;settings.nr=false;settings.sr=enabled;settings.multiplier=1;
        settings.videoSrQuality=quality;settings.srTarget=natural?pipeline::SrTarget::Uhd4K:pipeline::SrTarget::Qhd;
        settings.color.enabled=color;settings.color.exposure=color?1.0f:0.0f;
        settings.srMotion=quality==engine::kVideoSrFsr?engine::MotionSource::OpticalFlow:engine::MotionSource::Zero;
        engine::StageRequest request;request.sr=enabled;
        request.width=natural?(native?unsigned(natural->width):unsigned(natural->width)/2):(native?2560:1280);
        request.height=natural?(native?unsigned(natural->height):unsigned(natural->height)/2):(native?1440:720);
        pipeline::EnhanceGraphDesc desc;engine::describeStages(request,settings,desc);
        desc.rgbInput=true;desc.runtimeAbsPath=runtime::localRuntimeDirectory().wstring();
        pipeline::EnhanceGraph graph(ctx,ring);
        if(!graph.initialize(desc)||!graph.createViews())return false;
        AVFrame* frame=av_frame_alloc();frame->format=AV_PIX_FMT_RGBA;frame->width=request.width;frame->height=request.height;
        if(av_frame_get_buffer(frame,32)<0){av_frame_free(&frame);return false;}
        if(natural){
            auto* scaler=sws_getContext(natural->width,natural->height,AVPixelFormat(natural->format),frame->width,frame->height,AV_PIX_FMT_RGBA,SWS_LANCZOS,nullptr,nullptr,nullptr);
            if(!scaler){av_frame_free(&frame);return false;}
            const int rows=sws_scale(scaler,natural->data,natural->linesize,0,natural->height,frame->data,frame->linesize);sws_freeContext(scaler);
            if(rows!=frame->height){av_frame_free(&frame);return false;}
            frame->color_range=AVCOL_RANGE_JPEG;frame->color_primaries=natural->color_primaries;frame->color_trc=natural->color_trc;frame->colorspace=AVCOL_SPC_RGB;
        }else for(unsigned y=0;y<request.height;++y)for(unsigned x=0;x<request.width;++x){
            auto* p=frame->data[0]+size_t(y)*frame->linesize[0]+x*4;
            const auto texture=((x/3+y/3)%2)*18;
            p[0]=uint8_t(25+(x*105/request.width)+texture);p[1]=uint8_t(25+(y*105/request.height)+texture);
            p[2]=uint8_t(45+((x/32+y/32)%3)*20+texture);p[3]=255;
        }
        pipeline::EnhanceGraph::FrameOutputs out;bool passed=true;uint64_t first=0;
        for(unsigned i=0;i<3&&passed;++i){
            out={};passed=graph.process(frame,i*33.333333,i==0,out,i+1)&&ring.waitIdle();
            if(i==0)first=graph.metrics().srEvaluateCount;
        }
        if(passed)passed=sink::readRgba8(ctx,ring,graph.videoFrameResource(out.videoSlot),image);
        const bool expectedActive=enabled&&(!native||(!baseline&&quality>=1&&quality<=4));
        const uint64_t expectedEvaluations=expectedActive?(quality==engine::kVideoSrFsr&&baseline?2:3):0;
        passed=passed&&graph.srEnabled()==expectedActive&&graph.metrics().srEvaluateCount==expectedEvaluations;
        if(enabled&&quality==engine::kVideoSrFsr)passed=passed&&first==(baseline?0u:1u);
        const auto name=std::to_string(quality)+(native?"-native":"-upscale")+(color?"-color":"")+(enabled?"-on":"-off");
        if(passed)passed=sink::saveImage((directory/(name+".png")).wstring(),image,false);
        std::cout<<"SR_CASE name="<<name<<" active="<<graph.srEnabled()<<" firstEvaluations="<<first<<" evaluations="<<graph.metrics().srEvaluateCount<<" width="<<image.width<<" height="<<image.height<<" passed="<<passed<<std::endl;
        out={};ring.drainQueue();graph.shutdown();av_frame_free(&frame);return passed;
    };
    auto difference=[](const sink::RgbaImage& a,const sink::RgbaImage& b){
        uint64_t changed=0,error=0;
        if(!a.width||!a.height||a.width!=b.width||a.height!=b.height||a.pixels.size()!=b.pixels.size())return std::pair<uint64_t,double>{0,-1};
        for(size_t i=0;i<a.pixels.size();++i)if(i%4!=3){const auto d=std::abs(int(a.pixels[i])-int(b.pixels[i]));changed+=d!=0;error+=d;}
        return std::pair<uint64_t,double>{changed,double(error)/(a.width*a.height*3)};
    };
    auto mean=[](const sink::RgbaImage& a){if(!a.width||!a.height)return -1.0;double v=0;for(size_t i=0;i<a.pixels.size();++i)if(i%4!=3)v+=a.pixels[i];return v/(a.width*a.height*3);};
    sink::RgbaImage nativeOff,nativeOn;
    ok=render(4,true,false,false,nativeOff)&&ok;ok=render(4,true,false,true,nativeOn)&&ok;
    auto [changed,mae]=difference(nativeOff,nativeOn);
    const bool nativePass=baseline?changed==0:changed>1000;
    std::cout<<"SR_NATIVE_CHANGED channels="<<changed<<" mae8="<<mae<<" passed="<<nativePass<<std::endl;ok=ok&&nativePass;
    sink::RgbaImage low,high,colored;
    ok=render(1,false,false,true,low)&&ok;ok=render(4,false,false,true,high)&&ok;ok=render(4,false,true,true,colored)&&ok;
    const bool colorPass=mean(colored)>mean(high)+10;
    auto [qualityChanged,qualityMae]=difference(low,high);
    std::cout<<"SR_PRECOLOR baseMean="<<mean(high)<<" coloredMean="<<mean(colored)<<" passed="<<colorPass<<std::endl;
    std::cout<<"SR_QUALITY_CHANGED channels="<<qualityChanged<<" mae8="<<qualityMae<<" passed="<<(qualityChanged>100)<<std::endl;
    ok=ok&&colorPass&&qualityChanged>100;
    sink::RgbaImage dlss,fsr;
    ok=render(0,false,false,true,dlss)&&ok;ok=render(5,false,false,true,fsr)&&ok;
    media.close();ring.shutdown();ctx.shutdown();CoUninitialize();std::cout<<"SR_RESULT baseline="<<baseline<<" natural="<<(natural!=nullptr)<<" passed="<<ok<<std::endl;return ok?0:1;
}
