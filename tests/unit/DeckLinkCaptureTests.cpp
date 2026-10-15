#include "veyra/source/DeckLinkCapture.h"
#include "veyra/source/DeckLinkInputConnection.h"
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>
using namespace veyra::source::decklink;
using namespace veyra::pipeline;
namespace {
Mode makeMode(uint32_t id=1,unsigned width=1920){
    Mode m;m.id=id;m.width=width;m.height=1080;m.duration=1001;m.scale=60000;m.name=L"fixture";
    m.color.range=ColorRange::Limited;m.color.matrix=YuvMatrix::BT709;m.color.primaries=ColorPrimaries::BT709;m.color.transfer=TransferFunction::BT709;m.color.preserveSdrCodeValues=true;return m;
}
struct FakeDriver:Driver {
    DriverSink* sink=nullptr;Mode mode=makeMode();Pixel pixel=Pixel::V210;
    bool automatic=true,noSignal=false,occupied=false,rejectTen=false,failFlush=false,failStart=false,sendRgbChange=false,detectDuringEnable=false;
    bool oversizedFirst=false;
    unsigned enabled=0,started=0,paused=0,flushed=0,stopped=0;
    std::vector<uint8_t> pixels;
    std::thread::id owner=std::this_thread::get_id();bool allControlsOnOwner=true;
    void control(){allControlsOnOwner&=std::this_thread::get_id()==owner;}
    bool open(std::wstring_view,DriverSink& target,std::wstring& error)override{control();sink=&target;if(occupied){error=L"busy";return false;}return true;}
    std::vector<Mode> modes()override{auto out=std::vector<Mode>{makeMode(),makeMode(2,1280)};if(oversizedFirst){auto huge=makeMode(3,7680);huge.height=4320;out.insert(out.begin(),huge);}return out;}
    bool autoDetection()const override{return automatic;}
    bool enable(const Mode& m,Pixel p,bool,bool,std::wstring& error)override{control();++enabled;if(rejectTen&&(p==Pixel::V210||p==Pixel::Rgb10xLE)){error=L"unsupported";return false;}mode=m;pixel=p;
        if(detectDuringEnable&&enabled==1){std::thread callback([&]{sink->formatChanged({makeMode(2,1280),true,10});});callback.join();}
        return true;}
    Frame frame(){
        Frame f;f.width=mode.width;f.height=mode.height;f.stride=unsigned(rowBytes(pixel,mode.width))+128;f.pixel=pixel;
        pixels.resize(size_t(f.stride)*f.height,0);f.data=pixels.data();f.bytes=pixels.size();f.pts100ns=123456789;f.duration100ns=166833;f.noInput=noSignal;
        f.color=mode.color;f.color.pixelFormat=pixel==Pixel::V210?SourcePixelFormat::V210:pixel==Pixel::Rgb10xLE?SourcePixelFormat::Rgb10xLE:SourcePixelFormat::Uyvy;return f;
    }
    bool start(std::wstring& error)override{
        control();++started;if(failStart){error=L"start failed";return false;}
        // A real driver callback thread: synchronous join intentionally makes
        // any callback re-entry into owner control observable, without sleeps.
        std::thread callback([&]{if(sendRgbChange&&started==1)sink->formatChanged({makeMode(2,1280),true,10});else if(!noSignal)sink->video(frame());else{Frame f;f.noInput=true;sink->video(f);}});callback.join();return true;
    }
    bool pause(std::wstring&)override{control();++paused;return true;}
    bool flush(std::wstring& error)override{control();++flushed;if(failFlush){error=L"flush failed";return false;}return true;}
    void stop()noexcept override{control();++stopped;sink=nullptr;}
};
}
int main(){
    int failures=0;auto check=[&](bool value,const char* name){printf("%s %s\n",value?"PASS":"FAIL",name);if(!value)++failures;};
    auto fixture=[](){auto driver=std::make_unique<FakeDriver>();auto* raw=driver.get();return std::pair{std::move(driver),raw};};
    constexpr auto path=L"decklink-native:p:0000000000000123:0";
    {
        const auto hdmi=inputConnectionPath(path,2),sdi=inputConnectionPath(path,1);
        check(inputConnectionSelection(path).valid&&inputConnectionSelection(path).connection==0,"old device identity still follows driver");
        check(hdmi!=sdi&&inputConnectionSelection(hdmi).device==path&&inputConnectionSelection(hdmi).connection==2,"HDMI and SDI have separate stable saved identities");
        for(auto bad:{L"decklink-native:p:123:0:input=00000000",L"decklink-native:p:123:0:input=00000003",L"decklink-native:p:123:0:input=00000002extra",L"decklink-native:p:123:0:input=zzzzzzzz"})
            check(!inputConnectionSelection(bad).valid,"malformed or combined port cannot silently select another input");
        uint32_t port=1;unsigned writes=0;
        auto read=[&](uint32_t& value){value=port;return true;};
        auto write=[&](uint32_t value){port=value;++writes;return true;};
        InputConnectionLease lease;
        check(lease.apply(2,3,read,write)&&port==2&&writes==1,"explicit HDMI overrides driver SDI and verifies actual value");
        check(lease.apply(2,3,read,write)&&writes==1,"repeated pixel negotiation does not rewrite selected port");
        check(lease.restore(read,write)&&port==1&&writes==2,"close restores previous driver input without persistent preferences");
        InputConnectionLease external;
        check(external.apply(2,3,read,write),"second input lease applies");port=8;const auto before=writes;
        check(external.restore(read,write)&&port==8&&writes==before,"close never overwrites another application's changed input");
        InputConnectionLease unsupported;
        check(!unsupported.apply(2,1,read,write)&&writes==before,"unsupported connector fails without changing driver");
        InputConnectionLease ignored;port=1;
        check(!ignored.apply(2,3,read,[&](uint32_t){return true;})&&port==1,"successful Set call with ignored port is rejected by readback");
        InputConnectionLease readFailure;port=1;unsigned reads=0;
        auto intermittent=[&](uint32_t& value){value=port;return ++reads!=2;};
        check(!readFailure.apply(2,3,intermittent,write)&&port==2,"readback failure is not reported as successful input selection");
        check(readFailure.apply(2,3,read,write)&&readFailure.restore(read,write)&&port==1,"retry preserves original SDI for rollback after failed readback");
        InputConnectionLease follow;const auto unchanged=writes;
        check(follow.apply(0,3,read,write)&&follow.restore(read,write)&&writes==unchanged,"legacy follow-driver selection never switches ports");
    }
    {
        auto [driver,fake]=fixture();fake->oversizedFirst=true;Capture capture(std::move(driver));
        check(capture.configure(path,L"decklink:auto",false,50)&&capture.mode().id==1,"all driver modes retained without choosing an unsupported automatic seed");
        check(!capture.configure(path,modeKey(3),false,50)&&!capture.error().empty(),"oversized manual driver mode fails explicitly before enabling");
    }
    {
        auto [driver,fake]=fixture();Capture capture(std::move(driver));unsigned video=0,audio=0;
        check(capture.configure(path,L"decklink:auto",true,50),"valid signal probes without retaining a frame");
        check(capture.contract().data==nullptr&&capture.contract().stride==unsigned(rowBytes(Pixel::V210,1920))+128,"probe preserves real stride, releases SDK pixels");
        check(capture.start([&](const Frame& f){++video;check(f.pts100ns==123456789&&f.duration100ns==166833,"SDK timestamps preserved including fractional rate");},[&](const void*,size_t,int64_t){++audio;}),"deferred start publishes into consumer");
        check(video==1,"valid black pixels are delivered (not classified as missing signal)");
        fake->sink->formatChanged({makeMode(),false,10});check(!capture.needsReopen(),"duplicate detection after start does not reconnect forever");
        uint8_t pcm[4]{};fake->sink->audio(pcm,sizeof(pcm),123456789);check(audio==1,"audio begins only after consumer start");
        Frame invalid;invalid.noInput=true;fake->sink->video(invalid);fake->sink->video(fake->frame());fake->sink->audio(pcm,sizeof(pcm),0);
        check(capture.needsReopen()&&video==1&&audio==1&&!capture.error().empty(),"no-input flag gates stale video/audio and requests recovery");
        capture.stop();check(fake->sink==nullptr&&fake->allControlsOnOwner,"stop detaches sink, all controls stay off callback thread");
    }
    {
        auto [driver,fake]=fixture();fake->sendRgbChange=true;Capture capture(std::move(driver));
        check(capture.configure(path,L"decklink:auto",false,100)&&capture.mode().id==2&&capture.contract().pixel==Pixel::Rgb10xLE,"automatic RGB mode change reconfigures mode and ten-bit packing");
        check(fake->enabled==2&&fake->paused>=2&&fake->flushed==2&&fake->allControlsOnOwner,"owner Pause/Enable/Flush/Start, bounded renegotiation");
        unsigned frames=0;capture.start([&](const Frame&){++frames;});auto changed=fake->frame();changed.color.transfer=TransferFunction::PQ;changed.color.primaries=ColorPrimaries::BT2020;fake->sink->video(changed);
        check(capture.needsReopen()&&frames==1,"same-size SDR to HDR change requires new graph contract");
    }
    {
        auto [driver,fake]=fixture();fake->detectDuringEnable=true;Capture capture(std::move(driver));
        check(capture.configure(path,L"decklink:auto",false,100)&&capture.mode().id==2&&capture.contract().pixel==Pixel::Rgb10xLE&&fake->allControlsOnOwner,
            "detection during EnableVideoInput is retained before StartStreams");
    }
    {
        auto [driver,fake]=fixture();fake->noSignal=true;Capture capture(std::move(driver));const auto before=std::chrono::steady_clock::now();
        check(!capture.configure(path,L"decklink:auto",false,30)&&!capture.error().empty()&&std::chrono::steady_clock::now()-before<std::chrono::seconds(1),"no-signal timeout is bounded and descriptive");
        check(fake->sink==nullptr,"timeout stops and releases callback before return");
    }
    {
        auto [driver,fake]=fixture();fake->automatic=false;Capture capture(std::move(driver));
        check(!capture.configure(path,L"decklink:auto",false,30),"old card without automatic detection rejects auto explicitly");
        check(capture.configure(path,modeKey(2),false,30)&&capture.mode().id==2,"manual exact mode remains available on old card");
        check(!capture.configure(path,modeKey(999),false,30),"removed saved mode never falls back to ordinal");
    }
    for(unsigned scenario=0;scenario<3;++scenario){
        auto [driver,fake]=fixture();fake->occupied=scenario==0;fake->failFlush=scenario==1;fake->failStart=scenario==2;Capture capture(std::move(driver));
        check(!capture.configure(path,L"decklink:auto",false,30)&&!capture.error().empty()&&fake->sink==nullptr,"driver busy/start/flush failure closes resources");
    }
    {
        auto [driver,fake]=fixture();fake->rejectTen=true;Capture capture(std::move(driver));
        check(capture.configure(path,L"decklink:auto",false,30)&&capture.contract().pixel==Pixel::Uyvy8,"8-bit-only SDR card has explicit fallback");
        unsigned frames=0;capture.start([&](const Frame&){++frames;});auto bad=fake->frame();bad.color.transfer=TransferFunction::PQ;fake->sink->video(bad);
        check(capture.needsReopen()&&frames==1,"eight-bit output cannot silently carry HDR");
    }
    {
        auto [driver,fake]=fixture();Capture capture(std::move(driver));capture.configure(path,L"decklink:auto",false,30);capture.start([](const Frame&){});
        auto bad=fake->frame();bad.stride=1;fake->sink->video(bad);check(capture.needsReopen(),"invalid driver stride rejected before any ingest copy");
    }
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);const auto actual=devices();printf("INFO installed native devices=%zu (not hardware acceptance)\n",actual.size());
    {Capture missing;check(!missing.configure(L"decklink-native:missing-test-device",L"decklink:auto",false,30)&&!missing.error().empty(),"missing native driver/device fails explicitly without capture or null dereference");}
    CoUninitialize();
    return failures?1:0;
}
