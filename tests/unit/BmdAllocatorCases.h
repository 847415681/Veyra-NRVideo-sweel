#pragma once
#include <array>

// Uses real DirectShow allocators and samples, not fake success callbacks.
// OutputPin is the upstream direction fixture from CaptureColorContractTests.
template<class Check> void bmdAllocatorCases(Check check){
    using namespace veyra::source;
    VIDEOINFOHEADER2 vi{};vi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    vi.bmiHeader.biWidth=8;vi.bmiHeader.biHeight=4;vi.bmiHeader.biBitCount=16;
    vi.bmiHeader.biSizeImage=64;vi.AvgTimePerFrame=333333;
    AM_MEDIA_TYPE type{};type.majortype=MEDIATYPE_Video;type.subtype=MEDIASUBTYPE_UYVY;
    type.subtype.Data1=MAKEFOURCC('H','D','Y','C');type.formattype=FORMAT_VideoInfo2;
    type.cbFormat=sizeof(vi);type.pbFormat=reinterpret_cast<BYTE*>(&vi);
#ifndef VEYRA_TEST_BMD_LEGACY_FACTORY
    auto other=type;VIDEOINFOHEADER2 otherInfo=vi;other.pbFormat=reinterpret_cast<BYTE*>(&otherInfo);
    check(nativeCaptureContractsMatch(type,other),"BMD exact raw contract matches equivalent declaration");
    otherInfo.dwInterlaceFlags=AMINTERLACE_IsInterlaced|AMINTERLACE_Field1First;
    check(!nativeCaptureContractsMatch(type,other),"BMD connection rejects same-size/fps interlace mismatch");otherInfo=vi;
    other.subtype=MEDIASUBTYPE_UYVY;
    check(!nativeCaptureContractsMatch(type,other),"BMD connection rejects unrequested alias subtype");
#endif
    ComPtr<IBaseFilter> filter;ComPtr<IPin> input,output;ComPtr<IMemInputPin> memory;
    output.Attach(new OutputPin);unsigned delivered=0;std::array<BYTE,64> observed{};
    const auto callback=[&](IMediaSample* sample){
        BYTE* bytes=nullptr;if(FAILED(sample->GetPointer(&bytes))||!bytes)return E_FAIL;
        std::copy_n(bytes,observed.size(),observed.begin());++delivered;return S_OK;
    };
#ifdef VEYRA_TEST_BMD_LEGACY_FACTORY
    const auto create=createNativeCaptureSink(type,callback,filter,input);
#else
    const auto create=createNativeCaptureSink(type,callback,filter,input,CaptureAllocatorMode::Upstream);
#endif
    check(create==S_OK&&SUCCEEDED(filter.As(&memory)),"BMD real terminal created");
    if(!memory)return;
    ComPtr<IAMFilterMiscFlags> flags;const auto flagsHr=filter.As(&flags);
    check(flagsHr==S_OK&&flags->GetMiscFlags()==AM_FILTER_MISC_FLAGS_IS_RENDERER,"BMD terminal advertises renderer to graph manager");
    if(flags){
        ComPtr<IUnknown> a,b,c;filter.As(&a);flags.As(&b);input.As(&c);
        check(a.Get()==b.Get()&&b.Get()==c.Get(),"BMD filter/pin/renderer share canonical COM identity");
    }
    check(memory->ReceiveCanBlock()==S_OK,"BMD callback truthfully declares possible blocking");
    for(unsigned cycle=0;cycle<2;++cycle){
        check(input->ReceiveConnection(output.Get(),&type)==S_OK,"BMD HDYC connects/reconnects with unchanged contract");
        ComPtr<IMemAllocator> offered;
        const auto offeredHr=memory->GetAllocator(&offered);
        check(offeredHr==VFW_E_NO_ALLOCATOR&&!offered,"BMD sink does not offer generic allocator to producer");
        ALLOCATOR_PROPERTIES requirements{99,99,99,99};
        check(memory->GetAllocatorRequirements(&requirements)==E_NOTIMPL,"BMD sink leaves buffer requirements to driver");
        check(memory->GetAllocator(nullptr)==E_POINTER&&memory->NotifyAllocator(nullptr,TRUE)==E_POINTER,"BMD allocator null arguments rejected");
        ComPtr<IMemAllocator> driver;
        ALLOCATOR_PROPERTIES wanted{4,64,16,32},actual{};
        const bool ready=SUCCEEDED(CoCreateInstance(CLSID_MemoryAllocator,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&driver)))&&
            SUCCEEDED(driver->SetProperties(&wanted,&actual))&&SUCCEEDED(driver->Commit());
        check(ready,"BMD fixture upstream owns real aligned allocator with prefix");
        if(ready){
            check(memory->NotifyAllocator(driver.Get(),TRUE)==S_OK,"BMD accepts producer read-only pool");
            offered.Reset();check(memory->GetAllocator(&offered)==S_OK&&offered.Get()==driver.Get(),"BMD selected allocator remains driver object for diagnostics");
            ALLOCATOR_PROPERTIES queried{};
            check(queryCaptureAllocatorProperties(input.Get(),queried)==S_OK&&queried.cBuffers==actual.cBuffers&&queried.cbBuffer==actual.cbBuffer&&queried.cbAlign==actual.cbAlign&&queried.cbPrefix==actual.cbPrefix,"BMD diagnostic properties preserve producer pool/alignment/prefix");
            ComPtr<IMediaSample> sample;const auto sampleHr=driver->GetBuffer(&sample,nullptr,nullptr,0);
            check(sampleHr==S_OK&&sample,"BMD producer sample acquired");
            if(sample){
                BYTE* bytes=nullptr;sample->GetPointer(&bytes);std::array<BYTE,64> original{};
                for(unsigned i=0;i<original.size();++i)original[i]=BYTE((i*37+cycle*19)%251);
                std::copy(original.begin(),original.end(),bytes);sample->SetActualDataLength(64);
                const auto before=delivered;
                check(memory->Receive(sample.Get())==VFW_E_WRONG_STATE&&delivered==before,"BMD stopped terminal rejects delivery");
                filter->Run(0);
                check(memory->Receive(sample.Get())==S_OK&&delivered==before+1&&observed==original&&std::equal(original.begin(),original.end(),bytes),"BMD source pixels delivered exactly and read-only sample untouched");
                input->BeginFlush();check(memory->Receive(sample.Get())==S_FALSE&&delivered==before+1,"BMD flush suppresses callback");input->EndFlush();
                sample->SetActualDataLength(63);check(memory->Receive(sample.Get())==VFW_E_BUFFER_UNDERFLOW&&delivered==before+1,"BMD short frame rejected before callback");sample->SetActualDataLength(64);
                vi.bmiHeader.biHeight=8;sample->SetMediaType(&type);
                check(memory->Receive(sample.Get())==VFW_E_INVALIDMEDIATYPE&&delivered==before+1,"BMD mode change requires reopen rather than reinterpretation");
                vi.bmiHeader.biHeight=4;sample->SetMediaType(nullptr);
                vi.dwInterlaceFlags=AMINTERLACE_IsInterlaced;sample->SetMediaType(&type);
                check(memory->Receive(sample.Get())==VFW_E_INVALIDMEDIATYPE&&delivered==before+1,"BMD dynamic interlace change cannot keep stale temporal contract");
                vi.dwInterlaceFlags=0;sample->SetMediaType(nullptr);
                const unsigned beforeResume=delivered;
                check(memory->Receive(sample.Get())==S_OK&&observed==original&&delivered==beforeResume+1,"BMD unchanged frame resumes after rejected mode");
                filter->Stop();
                check(memory->Receive(sample.Get())==VFW_E_WRONG_STATE&&delivered==beforeResume+1,"BMD Stop quiesces callback");
            }
            sample.Reset();driver->Decommit();
        }
        offered.Reset();
        check(input->Disconnect()==S_OK,"BMD disconnect drops upstream peer and allocator");
        check(memory->GetAllocator(&offered)==VFW_E_NO_ALLOCATOR&&!offered,"BMD reconnect cannot reuse previous producer allocator");
    }
    // Default factory keeps the existing UVC/audio allocator contract.
    filter.Reset();input.Reset();memory.Reset();
    check(createNativeCaptureSink(type,callback,filter,input)==S_OK&&SUCCEEDED(filter.As(&memory)),"ordinary capture factory remains available");
    ComPtr<IMemAllocator> ordinary;ALLOCATOR_PROPERTIES requirements{};
    check(memory->GetAllocator(&ordinary)==S_OK&&ordinary&&memory->GetAllocatorRequirements(&requirements)==S_OK&&requirements.cBuffers==3&&requirements.cbBuffer==64,"ordinary capture retains sink allocator and bounded requirements");
    flags.Reset();check(filter.As(&flags)==E_NOINTERFACE,"ordinary graph renderer discovery unchanged");
}
