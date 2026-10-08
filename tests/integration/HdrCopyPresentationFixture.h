#pragma once
#include <cstring>

// Lossless readback belongs to this bounded integration test only. Playback
// uses neither this helper nor the full-image CPU wait below.
static bool readHdrWords(veyra::gfx::D3D12DeviceContext& ctx,veyra::gfx::CommandSlotRing& ring,
                        ID3D12Resource* texture,std::vector<uint32_t>& words,unsigned& w,unsigned& h){
    if(!texture||texture->GetDesc().Format!=DXGI_FORMAT_R10G10B10A2_UNORM)return false;
    const auto desc=texture->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 total=0;
    ctx.device()->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&total);
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=total;
    buffer.Height=1;buffer.DepthOrArraySize=buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    if(FAILED(ctx.device()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback))))return false;
    veyra::Status status=veyra::Status::Ok;unsigned slot=0;auto* list=ring.acquireNext(slot,status);if(!list)return false;
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={texture,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE};list->ResourceBarrier(1,&barrier);
    D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=texture;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=footprint;
    list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);list->ResourceBarrier(1,&barrier);
    if(!ring.submitAndSignal(slot)||!ring.waitIdle())return false;
    void* mapped=nullptr;D3D12_RANGE range{0,size_t(total)};if(FAILED(readback->Map(0,&range,&mapped)))return false;
    w=unsigned(desc.Width);h=desc.Height;words.resize(size_t(w)*h);
    for(unsigned y=0;y<h;++y)std::memcpy(words.data()+size_t(y)*w,static_cast<uint8_t*>(mapped)+footprint.Offset+size_t(y)*footprint.Footprint.RowPitch,size_t(w)*4);
    D3D12_RANGE empty{0,0};readback->Unmap(0,&empty);return true;
}

static bool hdrCopyPresentation(veyra::gfx::D3D12DeviceContext& ctx,veyra::gfx::CommandSlotRing& ring,
                                HWND window,const std::filesystem::path& runtime){
    using namespace veyra;
    pipeline::EnhanceGraph graph(ctx,ring);engine::VideoPresenter presenter;
    AVFrame* frame=av_frame_alloc();if(!frame)return false;
    frame->format=AV_PIX_FMT_P010LE;frame->width=640;frame->height=360;
    frame->color_range=AVCOL_RANGE_MPEG;frame->colorspace=AVCOL_SPC_BT2020_NCL;
    frame->color_primaries=AVCOL_PRI_BT2020;frame->color_trc=AVCOL_TRC_SMPTE2084;
    bool ok=av_frame_get_buffer(frame,32)>=0;
    MONITORINFO monitor{sizeof(monitor)};ok=ok&&GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor);
    unsigned real=0,generated=0,pixelChecks=0;int maxError=0;
    const bool buffers4k=GetEnvironmentVariableW(L"VEYRA_TEST_HDR_4K_WINDOW",nullptr,0)>0;
    for(unsigned multiplier:{1u,2u,4u,2u})for(bool full:{false,true}){
        if(!ok)break;
        SetWindowPos(window,nullptr,0,0,full?(buffers4k?3840:monitor.rcMonitor.right-monitor.rcMonitor.left):640,full?(buffers4k?2160:monitor.rcMonitor.bottom-monitor.rcMonitor.top):360,SWP_NOZORDER|SWP_NOACTIVATE);
        ShowWindow(window,SW_SHOWNOACTIVATE);
        pipeline::EnhanceGraphDesc desc;desc.sourceWidth=desc.workWidth=640;desc.sourceHeight=desc.workHeight=360;
        if(GetEnvironmentVariableW(L"VEYRA_TEST_HDR_VFG",nullptr,0)>0){desc.frameGenerationBackend=engine::FrameGenerationBackend::Vfg;desc.vfgAsyncSubmission=true;desc.vfgQuality=1;}
        desc.enableNr=desc.enableSr=false;desc.enableFg=multiplier>1;desc.fgMultiplier=std::max(2u,multiplier);
        desc.hdrInput=desc.hdrOutput=true;desc.captureBitDepth=10;desc.runtimeAbsPath=runtime.wstring();
        ok=graph.initialize(desc)&&graph.createViews()&&presenter.open(ctx,window,graph);
        if(!ok)break;
        presenter.recordGpuTimings();
        std::vector<double> blitTimes;
        const auto collect=[&]{for(const auto& sample:presenter.takeGpuTimings(ctx.fence())){
            const auto& blit=sample.gpu[size_t(diagnostics::GpuStage::Blit)];
            if(blit.state==diagnostics::SampleState::Measured&&blit.milliseconds)blitTimes.push_back(*blit.milliseconds);
        }};
        engine::PresentationSettings requested;requested.fullscreen=full;std::wstring text;presenter.configurePresentation(ctx,requested,desc.enableFg,text);
        for(unsigned i=0;ok&&i<64;++i){
            if(!full&&(i==24||i==40)){
                SetWindowPos(window,nullptr,0,0,i==24?800:640,i==24?450:360,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);Sleep(110);
            }
            for(unsigned y=0;y<360;++y){auto* row=reinterpret_cast<uint16_t*>(frame->data[0]+size_t(y)*frame->linesize[0]);
                for(unsigned x=0;x<640;++x)row[x]=uint16_t((400+i+(x/80)%2*60)<<6);}
            for(unsigned y=0;y<180;++y){auto* row=reinterpret_cast<uint16_t*>(frame->data[1]+size_t(y)*frame->linesize[1]);for(unsigned x=0;x<640;++x)row[x]=uint16_t(512<<6);}
            const auto start=std::chrono::steady_clock::now();
            while(!graph.nextFrameSlotAvailable()&&std::chrono::steady_clock::now()-start<std::chrono::seconds(2))Sleep(1);
            pipeline::EnhanceGraph::FrameOutputs out;
            ok=graph.nextFrameSlotAvailable()&&graph.process(frame,i*16.6827,i==0||i==32,out,i+1)&&ring.waitIdle()&&graph.resolveGeneration(out);
            for(unsigned j=0;ok&&j<out.batch.count;++j){
                const auto& item=out.batch.frames[j];const bool gen=item.kind==pipeline::FrameKind::Generated;
                if(gen&&item.validity!=pipeline::GenerationValidity::Valid)continue;
                ok=presenter.present(ctx,ring,graph,item.lease->slot,gen,false,0,false,.5f,item.identity,{},item.pts100ns);
                collect();
                if(gen)++generated;else ++real;
                if(ok&&!full&&i<8){
                    auto shown=presenter.presentedResourceForTest();std::vector<uint32_t> actual,expected;unsigned w=0,h=0,ew=0,eh=0;
                    ok=readHdrWords(ctx,ring,shown.Get(),actual,w,h)&&readHdrWords(ctx,ring,item.lease->texture.Get(),expected,ew,eh)&&w==ew&&h==eh;
                    if(ok){
                        for(size_t p=0;p<actual.size();++p)for(unsigned c=0;c<3;++c)
                            maxError=std::max(maxError,std::abs(int((actual[p]>>(10*c))&1023)-int((expected[p]>>(10*c))&1023)));
                        ok=maxError<=1;++pixelChecks;
                    }
                }
            }
        }
        // Exercise both teardown orders. HDR/no-FG uses the shared queue.
        ok=ring.drainQueue()&&ok;
        auto last=presenter.presentedResourceForTest();last.Reset();collect();
        std::sort(blitTimes.begin(),blitTimes.end());
        if(!blitTimes.empty())std::cout<<"HDR_COPY_COST multiplier="<<multiplier<<" full="<<full<<" buffers4k="<<buffers4k
            <<" samples="<<blitTimes.size()<<" medianGpuMs="<<blitTimes[blitTimes.size()/2]<<" p95GpuMs="<<blitTimes[std::min(blitTimes.size()-1,blitTimes.size()*95/100)]<<std::endl;
        if(multiplier==4){graph.shutdown();presenter.close();}else{presenter.close();graph.shutdown();}
        std::cout<<"HDR_COPY multiplier="<<multiplier<<" full="<<full<<" checks="<<pixelChecks<<" max10BitError="<<maxError<<" pass="<<ok<<std::endl;
    }
    ring.drainQueue();presenter.close();graph.shutdown();av_frame_free(&frame);
    std::cout<<"HDR_COPY_SUMMARY real="<<real<<" generated="<<generated<<" checks="<<pixelChecks<<" max10BitError="<<maxError<<" pass="<<ok<<std::endl;
    return ok&&real>0&&generated>0&&pixelChecks>0;
}
