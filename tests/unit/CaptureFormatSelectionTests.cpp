#include "veyra/source/CaptureFormatSelection.h"
#include "veyra/source/CaptureFrameIntervals.h"
#include <array>
#include <iostream>
#include <vector>
#include <cstring>

struct RateDriver final : IAMVideoControl {
    std::vector<LONGLONG> intervals;HRESULT result=S_OK;long seenIndex=-1;SIZE seenSize{};
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void**) override{return E_NOINTERFACE;}
    ULONG STDMETHODCALLTYPE AddRef() override{return 1;}
    ULONG STDMETHODCALLTYPE Release() override{return 1;}
    HRESULT STDMETHODCALLTYPE GetCaps(IPin*,long*) override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetMode(IPin*,long) override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetMode(IPin*,long*) override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetCurrentActualFrameRate(IPin*,LONGLONG*) override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetMaxAvailableFrameRate(IPin*,long,SIZE,LONGLONG*) override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetFrameRateList(IPin*,long index,SIZE dimensions,long* count,LONGLONG** data) override{
        seenIndex=index;seenSize=dimensions;*count=long(intervals.size());
        *data=static_cast<LONGLONG*>(CoTaskMemAlloc(intervals.size()*sizeof(LONGLONG)));
        if(*data)std::memcpy(*data,intervals.data(),intervals.size()*sizeof(LONGLONG));return result;
    }
};

int main(){
    using namespace veyra::source;
    int failures=0;
    auto check=[&](bool ok,const char* name){if(!ok){++failures;std::cerr<<"FAIL "<<name<<'\n';}};
    std::array<CaptureFormat,3> formats{{{7,3840,2160,60,L"",L"4k60-p010"},{2,1920,1080,30,L"",L"1080p30-nv12"},{0,3840,2160,30,L"",L"4k30-p010"}}};
    check(selectCaptureFormat(formats,2,L"4k60-p010")==&formats[0],"stable key overrides stale index");
    check(!selectCaptureFormat(formats,2,L"missing"),"missing key never falls back to another format");
    check(selectCaptureFormat(formats,2,L"")==&formats[1],"legacy request retains index semantics");
    check(!selectCaptureFormat(formats,9,L""),"invalid legacy index rejected");
    std::array<CaptureFormat,2> scan{{{4,1920,1080,30,L"",L"1080-30:scan=0"},{9,1920,1080,30,L"",L"1080-30:scan=3"}}};
    check(!selectCaptureFormat(scan,4,L"1080-30"),"ambiguous legacy p30/i60 key must be reselected");
    check(selectCaptureFormat(scan,4,L"1080-30:scan=3")==&scan[1],"interlace identity overrides stale index");
    check(selectCaptureFormat(std::span<const CaptureFormat>(scan.data(),1),9,L"1080-30")==&scan[0],"unique legacy scan key migrates safely");
    double fps=0;std::wstring key;
    for(double requested:{0.,29.97,40.,60.}){
        const auto query=capturePathOptions(requested,L"0034004B");
        check(parseCapturePathOptions(std::wstring_view(query).substr(1),fps,key)&&fps==requested&&key==L"0034004B","options roundtrip");
    }
    check(parseCapturePathOptions(L"fps=50",fps,key)&&fps==50&&key.empty(),"old fps-only URI supported");
    check(parseCapturePathOptions(L"format=0041&fps=30",fps,key)&&fps==30,"option order independent");
    for(auto bad:{L"",L"format=",L"format=XYZW",L"format=000",L"fps=30&fps=60",L"format=0041&format=0042",L"fps=60&",L"unknown=1",L"fps=nan"})
        check(!parseCapturePathOptions(bad,fps,key),"malformed options rejected");
    check(captureFrameRateMatches(60,166833),"nominal 59.94 allowed");
    check(!captureFrameRateMatches(60,200000),"60 to 50 renegotiation rejected");
    check(!captureFrameRateMatches(60,0),"unknown negotiated rate rejected");

    auto media=[&](int index,unsigned width,unsigned height,double fps,std::wstring key,std::wstring pixel,bool interlaced,std::wstring scan){
        CaptureFormat format;
        format.index=index;format.width=width;format.height=height;format.fps=fps;
        format.key=std::move(key);format.pixel=std::move(pixel);format.interlaced=interlaced;format.scan=std::move(scan);
        return format;
    };
    std::vector<CaptureFormat> listed{
        media(0,1920,1080,60,L"1080p60-nv12",L"NV12",false,L"逐行"),
        media(1,1920,1080,60,L"1080p60-yuy2",L"YUY2",false,L"逐行"),
        media(2,1920,1080,30,L"1080p30-nv12",L"NV12",false,L"逐行"),
        media(3,1920,1080,30,L"1080i60-nv12",L"NV12",true,L"隔行（59.94 场/秒）"),
        media(4,3840,2160,30,L"4k30-yuy2",L"YUY2",false,L"逐行"),
        media(5,1920,1080,60,L"1080p60-nv12-bff",L"NV12",false,L"逐行"),
        media(6,3840,2160,24,L"4k24-yuy2",L"YUY2",false,L"逐行"),
    };
    const auto resolutions=captureResolutionOptions(listed);
    check(resolutions.size()==2&&resolutions[0].id==L"1920x1080"&&resolutions[1].id==L"3840x2160","resolutions stay unique and in list order");
    const auto* p60=selectCaptureFormat(listed,-1,L"1080p60-nv12");
    const auto rates=captureFrameRateOptions(listed,p60);
    check(rates.size()==3,"1080p lists progressive 60, progressive 30 and interlaced 30 only");
    check(captureFrameRateId(listed[2])!=captureFrameRateId(listed[3]),"same numeric rate keeps progressive and interlaced apart");
    bool saw4kOnlyRate=false;for(const auto& rate:rates)if(rate.id==captureFrameRateId(listed[6]))saw4kOnlyRate=true;
    check(!saw4kOnlyRate,"a frame rate that exists only at 4k is hidden while 1080p is selected");
    const auto pixels=capturePixelOptions(listed,p60);
    check(pixels.size()==3&&pixels[0].id==L"1080p60-nv12"&&pixels[1].id==L"1080p60-yuy2"&&pixels[2].id==L"1080p60-nv12-bff","pixel list is the concrete 1080p60 media types");
    check(pixels[0].label.find(L"[format 0]")!=std::wstring::npos&&pixels[2].label.find(L"[format 5]")!=std::wstring::npos,"duplicate pixel names stay selectable");
    check(pixels[1].label==L"YUY2","unique pixel name is not decorated");
    const auto* to4k=retargetCaptureFormat(listed,p60,CaptureFormatAxis::Resolution,L"3840x2160");
    check(!to4k,"missing 4k NV12 never silently switches to YUY2");
    const auto* keepNv12=retargetCaptureFormat(listed,p60,CaptureFormatAxis::FrameRate,captureFrameRateId(listed[2]));
    check(keepNv12==&listed[2],"frame-rate change keeps NV12 instead of another pixel at that rate");
    const auto* toInterlace=retargetCaptureFormat(listed,p60,CaptureFormatAxis::FrameRate,captureFrameRateId(listed[3]));
    check(toInterlace==&listed[3],"interlaced rate selects the interlaced media type");
    check(!retargetCaptureFormat(listed,p60,CaptureFormatAxis::FrameRate,L"50.000000|0"),"missing frame rate is rejected");
    check(!retargetCaptureFormat(listed,p60,CaptureFormatAxis::Resolution,L"1280x720"),"missing resolution is rejected");
    const auto* yuy2=selectCaptureFormat(listed,-1,L"1080p60-yuy2");
    const auto* keptPixel=retargetCaptureFormat(listed,yuy2,CaptureFormatAxis::Resolution,L"3840x2160");
    check(keptPixel==&listed[4],"resolution change keeps YUY2 when that pixel exists at the new size");
    const auto interlacedPixels=capturePixelOptions(listed,&listed[3]);
    check(interlacedPixels.size()==1&&interlacedPixels[0].id==L"1080i60-nv12","interlaced rate does not offer the progressive NV12 row");
    {
        const auto resolution=captureResolutionId(*p60),rate=captureFrameRateId(*p60);
        check(!captureResolutionSample(listed,L""),"blank resolution cannot populate dependent menus");
        check(!captureRateSample(listed,L"",rate)&&!captureRateSample(listed,resolution,L""),"rate requires a selected resolution and rate");
        check(!selectCapturePixelFormat(listed,L"",rate,p60->key),"pixel cannot be selected before resolution");
        check(!selectCapturePixelFormat(listed,resolution,L"",p60->key),"pixel cannot be selected before rate");
        check(selectCapturePixelFormat(listed,resolution,rate,p60->key)==p60,"exact advertised combination accepted");
        check(!selectCapturePixelFormat(listed,L"3840x2160",rate,p60->key),"stale 1080p pixel key cannot open at 4k");
        check(!selectCapturePixelFormat(listed,resolution,captureFrameRateId(listed[2]),p60->key),"stale 60fps key cannot open at 30fps");
        check(!selectCapturePixelFormat(listed,resolution,rate,L"not-advertised"),"unknown pixel key rejected without fallback");
        for(auto pixel:{L"MJPEG",L"MJPG",L"mjpeg",L"mJpG"}){
            auto jpeg=media(50,1920,1080,60,L"jpeg",pixel,false,L"逐行");
            const std::array warningFormats{*p60,jpeg};
            const auto choices=capturePixelOptions(warningFormats,p60);
            check(choices.size()==2&&!choices[0].discouraged&&choices[1].discouraged,"only MJPEG/MJPG receives warning, regardless of case");
            check(!choices[1].disabled,"MJPEG remains selectable with a warning");
        }
    }
    {
        RateDriver driver;driver.intervals={166667,333667,417083,100000,0,-1,333667};
        HRESULT hr=E_FAIL;auto* pin=reinterpret_cast<IPin*>(uintptr_t(1));
        const auto rates=captureFrameIntervals(&driver,pin,19,{1448,1086},166667,&hr);
        check(hr==S_OK&&driver.seenIndex==19&&driver.seenSize.cx==1448&&driver.seenSize.cy==1086,"queries current capability index and exact device dimensions");
        check(rates==std::vector<REFERENCE_TIME>{166667,333667,417083,100000},"all discrete driver rates kept, invalid and duplicates removed without preset rates");
        driver.result=E_NOTIMPL;
        check(captureFrameIntervals(&driver,pin,2,{640,480},200000,&hr)==std::vector<REFERENCE_TIME>{200000}&&hr==E_NOTIMPL,"unavailable rate API retains exact advertised type");
        check(captureFrameIntervals(nullptr,nullptr,0,{640,480},0)==std::vector<REFERENCE_TIME>{0},"unknown default interval stays visible");
    }
    {
        auto unusual=media(25,1448,1086,47.952,L"unusual",L"CUSTOM",false,L"逐行");
        auto huge=media(30,7680,4320,120,L"8k",L"P010",false,L"逐行");huge.supported=false;
        CaptureFormat automatic;automatic.index=-1;automatic.key=L"decklink:auto";automatic.automatic=true;automatic.pixel=L"自动选择像素格式";
        std::vector<CaptureFormat> all{automatic,unusual,huge,unusual};
        const auto sizes=captureResolutionOptions(all);
        check(sizes.size()==3&&sizes[0].id==L"auto"&&sizes[0].label.find(L"0")==std::wstring::npos&&sizes[1].id==L"1448x1086"&&sizes[2].id==L"7680x4320","automatic, nonstandard and oversized device modes all visible");
        check(captureFrameRateOptions(all,&all[0])[0].label==L"跟随实际信号","DeckLink automatic rate never displayed as zero fps");
        check(!selectCapturePixelFormat(all,L"auto",L"auto",L""),"blank pixel key cannot select DeckLink automatic ordinal -1");
        check(capturePixelOptions(all,&all[1]).size()==1,"identical media keys are not duplicated");
        const auto unsupported=capturePixelOptions(all,&all[2]);
        check(unsupported.size()==1&&unsupported[0].disabled&&unsupported[0].label.find(L"不支持")!=std::wstring::npos,"unsupported driver format remains visible with explicit limit");
        unusual.fps=1;unusual.interval100ns=10000000;huge.fps=1.0000001;huge.interval100ns=9999999;
        check(captureFrameRateId(unusual)!=captureFrameRateId(huge),"exact driver intervals never collapse from decimal rounding");
        for(const auto& format:all){
            const auto* chosen=retargetCaptureFormat(all,nullptr,CaptureFormatAxis::Resolution,captureResolutionId(format));
            check(chosen!=nullptr,"every reported resolution is reachable");
            const auto options=capturePixelOptions(all,&format);
            check(std::any_of(options.begin(),options.end(),[&](const CaptureFormatOption& option){return option.id==format.key;}),"every reported media identity is reachable through linked selectors");
        }
    }
    std::cout<<"capture format selection failures="<<failures<<'\n';return failures?1:0;
}
