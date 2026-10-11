#include "veyra/sink/WasapiAudioSink.h"
#include "veyra/sink/AudioGain.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cmath>
#include <functional>
#include <array>

using namespace veyra::sink;
using namespace std::chrono_literals;
namespace {
int failures = 0;
void check(bool ok, const char* text) {
    std::cout << (ok ? "PASS " : "FAIL ") << text << '\n';
    failures += !ok;
}
bool until(const std::function<bool()>& ready) {
    const auto limit = std::chrono::steady_clock::now() + 3s;
    while (!ready() && std::chrono::steady_clock::now() < limit) std::this_thread::sleep_for(2ms);
    return ready();
}
void fixture(const std::filesystem::path& file, uint32_t rate, unsigned durationMs=750) {
    std::ofstream out(file, std::ios::binary);
    auto word = [&](uint32_t v, unsigned bytes) { for (unsigned i=0;i<bytes;++i) out.put(char(v >> (8*i))); };
    const uint32_t frames = rate * durationMs / 1000;
    out.write("RIFF",4); word(36+frames*2,4); out.write("WAVEfmt ",8); word(16,4);
    word(1,2); word(1,2); word(rate,4); word(rate*2,4); word(2,2); word(16,2);
    out.write("data",4); word(frames*2,4);
    for (uint32_t i=0;i<frames;++i) word(uint16_t(int16_t(12000*std::sin(i*0.031))),2);
}
void decode(const std::filesystem::path& file) {
    AudioPipeline pipe;
    check(pipe.open(file.wstring()), "open PCM fixture");
    pipe.startThread(nullptr);
    check(until([&] { return pipe.decodingComplete(); }), "decoder and resampler fully drained");
    check(std::abs(pipe.headPtsMs()) < 0.05, "first PCM timestamp is block start at zero");
    check(std::abs(pipe.tailPtsMs()-750) < 0.05, "resampled tail preserves 750ms duration");
    size_t total=0; double pts=0, maxError=0; std::vector<float> block(pipe.pcmFormat().channels*137);
    while (auto n=pipe.pull(block.data(),137,&pts)) {
        maxError=std::max(maxError,std::abs(pts-1000.0*total/kAudioRate)); total+=n;
    }
    std::cout << "samples=" << total << " maxPtsErrorMs=" << maxError << '\n';
    check(total==36000 && maxError<0.05, "no samples lost and PTS contiguous across packet boundaries");
    const double seek=pipe.requestSeek(123.25);
    check(seek>=123.25-0.001 && seek<123.25+1000.0/kAudioRate+0.001, "seek trims within a block at sample precision");
    check(until([&]{return pipe.decodingComplete();}), "seek drains through end");
    check(std::abs(pipe.tailPtsMs()-750)<0.05, "seek resets resampler history and preserves end PTS");
    check(pipe.overruns()==0, "bounded PCM ring did not overflow");
    pipe.stopThread();
}
int startupSeek(const std::filesystem::path& dir) {
    const auto file=dir/"startup-seek.wav";fixture(file,48000,4000);
    for(unsigned attempt=0;attempt<3;++attempt){
        AudioPipeline pipe;AudioRenderer renderer;renderer.setGain(0);
        check(pipe.open(file.wstring()),"open startup seek fixture");
        pipe.holdForVideo();pipe.startThread(&renderer,true);
        // Interrupt the initial prefill while the owned WASAPI endpoint opens.
        const double target=pipe.requestSeek(1000);
        check(std::abs(target-1000)<.05&&renderer.started(),"startup seek anchors actual PCM");
        check(!pipe.endpointRecovering(),"successful startup seek clears endpoint recovery");
        const double held=renderer.mediaTimeMs();std::this_thread::sleep_for(40ms);
        check(std::abs(held-1000)<.1&&std::abs(renderer.mediaTimeMs()-held)<.1,"startup seek remains held for first video");
        pipe.videoPresented(1042);
        check(until([&]{return renderer.mediaTimeMs()>1100;}),"first video releases startup seek audio");
        check(pipe.overruns()==0,"startup seek keeps bounded PCM");
        pipe.stopThread();
    }
    return failures?1:0;
}
int jitter(const std::filesystem::path& dir) {
    const auto file=dir/"video-jitter.wav";fixture(file,48000,6000);
    // Alternating 0/5ms completion jitter around 30fps. Real/generated frames
    // are announced exactly as their owner can observe them; XeSS has only B.
    for(unsigned multiplier:{1u,2u,4u}){
        AudioPipeline pipe;AudioRenderer renderer;renderer.setGain(0);
        check(pipe.open(file.wstring()),"open continuous-audio jitter fixture");
        pipe.holdForVideo();pipe.startThread(&renderer,true);
        check(until([&]{return renderer.started()&&!pipe.endpointRecovering()&&pipe.waitingForVideo();}),"prefill held before jitter replay");
        const auto waits=pipe.videoWaitCount();
        const auto begin=std::chrono::steady_clock::now();
        pipe.videoPresented(1000.0/30/multiplier);
        for(unsigned i=1;i<=90;++i){
            const double pts=i*1000.0/30;
            std::this_thread::sleep_until(begin+std::chrono::microseconds(int64_t((pts+(i%2?5.0:0.0))*1000)));
            for(unsigned sub=1;sub<multiplier;++sub){
                const double generated=pts-1000.0/30+sub*1000.0/30/multiplier;
                pipe.videoReady(generated);pipe.videoPresented(generated+1000.0/30/multiplier);
            }
            pipe.videoReady(pts);pipe.videoPresented(pts+1000.0/30/multiplier);
        }
        const double elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
        const double media=renderer.mediaTimeMs();const auto stops=pipe.videoWaitCount()-waits;
        std::cout<<"JITTER multiplier="<<multiplier<<" wallMs="<<elapsed<<" audioMs="<<media<<" additionalVideoPauses="<<stops<<" underruns="<<renderer.underruns()<<'\n';
        check(stops==0,"5ms video jitter never stops continuous audio");
        check(std::abs(media-elapsed)<35,"real endpoint keeps one-times audio playback through jitter");
        check(renderer.underruns()==0&&pipe.overruns()==0,"jitter does not drop PCM or insert silent underruns");
        pipe.stopThread();
    }
    return failures?1:0;
}
int underrate(const std::filesystem::path& dir) {
    // Deterministic sustained under-rate: a 60fps source whose enhancement
    // completes at only ~51fps — the RTX4060 revision11 log signature.
    // Presented coverage advances slower than media time while real PCM plays.
    // The audio master clock must keep one-times continuous playback with no
    // rebuffer; video must drop preview work instead of pausing sound.
    const auto file=dir/"video-underrate.wav";fixture(file,48000,8000);
    AudioPipeline pipe;AudioRenderer renderer;renderer.setGain(0);
    check(pipe.open(file.wstring()),"open sustained under-rate fixture");
    pipe.holdForVideo();pipe.startThread(&renderer,true);
    check(until([&]{return renderer.started()&&!pipe.endpointRecovering()&&pipe.waitingForVideo();}),"prefill held before under-rate replay");
    const auto waits=pipe.videoWaitCount();
    const auto begin=std::chrono::steady_clock::now();
    const double frameCostMs=1000.0/51.0,intervalMs=1000.0/60.0;
    pipe.videoPresented(intervalMs);
    for(int n=1;n<=204;++n){
        std::this_thread::sleep_until(begin+std::chrono::microseconds(int64_t(n*frameCostMs*1000)));
        const double presentedPts=n*intervalMs;
        pipe.videoReady(presentedPts);
        pipe.videoPresented(presentedPts+intervalMs);
    }
    const double wall=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
    const double media=renderer.mediaTimeMs();const auto stops=pipe.videoWaitCount()-waits;
    std::cout<<"UNDERRATE60 wallMs="<<wall<<" audioMs="<<media<<" additionalVideoPauses="<<stops<<" underruns="<<renderer.underruns()<<'\n';
    check(stops==0,"51/60 sustained under-rate never stops audio");
    check(std::abs(media-wall)<35,"audio keeps one-times speed while enhancement under-rates");
    check(renderer.underruns()==0&&pipe.overruns()==0,"under-rate loses no PCM and inserts no silence");
    pipe.stopThread();
    return failures?1:0;
}
int readStall(const std::filesystem::path& dir,bool direct) {
    // Slow storage (logs16, RTX 3060 Ti, 4K HDR remuxes): a single file read on
    // the audio thread took 250-600 ms, the 22 ms endpoint ran dry, the audio
    // master clock stopped and video froze with it. Here one read in every
    // 700 ms takes 300 ms. Direct reads (the old path) must reproduce the
    // freeze; read-ahead must keep the clock running.
    const auto file=dir/"read-stall.wav";fixture(file,48000,9000);
    SetEnvironmentVariableW(L"VEYRA_TEST_AUDIO_READ_STALL_MS",L"300");
    SetEnvironmentVariableW(L"VEYRA_TEST_AUDIO_READ_STALL_EVERY_MS",L"700");
    SetEnvironmentVariableW(L"VEYRA_TEST_AUDIO_DIRECT_READ",direct?L"1":nullptr);
    AudioPipeline pipe;AudioRenderer renderer;renderer.setGain(0);
    check(pipe.open(file.wstring()),"open read-stall fixture");
    pipe.holdForVideo();pipe.startThread(&renderer,true);
    SetEnvironmentVariableW(L"VEYRA_TEST_AUDIO_READ_STALL_MS",nullptr);
    SetEnvironmentVariableW(L"VEYRA_TEST_AUDIO_READ_STALL_EVERY_MS",nullptr);
    SetEnvironmentVariableW(L"VEYRA_TEST_AUDIO_DIRECT_READ",nullptr);
    check(until([&]{return renderer.started()&&!pipe.endpointRecovering()&&pipe.waitingForVideo();}),"prefill held before read-stall replay");
    pipe.videoPresented(1e9);
    check(until([&]{return std::isfinite(renderer.mediaTimeMs())&&renderer.mediaTimeMs()>20;}),"read-stall playback starts");
    const auto begin=std::chrono::steady_clock::now();const double startMedia=renderer.mediaTimeMs();
    const auto startUnderruns=renderer.underruns();
    double lastMedia=startMedia,maxFreezeMs=0;auto lastAdvance=begin;
    while(std::chrono::steady_clock::now()-begin<5s){
        std::this_thread::sleep_for(2ms);
        const double media=renderer.mediaTimeMs();const auto now=std::chrono::steady_clock::now();
        if(std::isfinite(media)&&media>lastMedia+0.5){lastMedia=media;lastAdvance=now;}
        maxFreezeMs=std::max(maxFreezeMs,std::chrono::duration<double,std::milli>(now-lastAdvance).count());
    }
    const double wall=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
    const double media=renderer.mediaTimeMs()-startMedia;const auto underruns=renderer.underruns()-startUnderruns;
    std::cout<<"READ_STALL directRead="<<direct<<" wallMs="<<wall<<" audioMs="<<media<<" maxClockFreezeMs="<<maxFreezeMs<<" underruns="<<underruns<<'\n';
    if(direct){
        check(maxFreezeMs>200,"old direct reads freeze the master clock on a 300 ms read (reproduces logs16)");
    }else{
        check(maxFreezeMs<60,"read-ahead keeps the master clock running through 300 ms reads");
        check(underruns==0,"read-ahead never starves the endpoint");
        check(std::abs(media-wall)<35,"audio stays at one-times speed through slow reads");
    }
    check(pipe.overruns()==0,"slow reads keep the PCM queue bounded");
    const double seek=pipe.requestSeek(6000);
    check(std::abs(seek-6000)<.05,"seek after read-ahead trims at sample precision");
    pipe.videoPresented(1e9);
    check(until([&]{return renderer.mediaTimeMs()>6050;}),"playback continues from the seek target with read-ahead");
    pipe.stopThread();
    return failures?1:0;
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc!=2&&!(argc==3&&(wcscmp(argv[2],L"--jitter")==0||wcscmp(argv[2],L"--underrate")==0||wcscmp(argv[2],L"--startup-seek")==0||
        wcscmp(argv[2],L"--read-stall")==0||wcscmp(argv[2],L"--read-stall-direct")==0))) return 2;
    const std::filesystem::path dir=argv[1]; std::filesystem::create_directories(dir);
    if(argc==3&&wcscmp(argv[2],L"--read-stall")==0)return readStall(dir,false);
    if(argc==3&&wcscmp(argv[2],L"--read-stall-direct")==0)return readStall(dir,true);
    if(argc==3&&wcscmp(argv[2],L"--startup-seek")==0)return startupSeek(dir);
    if(argc==3&&wcscmp(argv[2],L"--underrate")==0)return underrate(dir);
    if(argc==3)return jitter(dir);
    for(uint32_t rate:{48000u,44100u}) { const auto file=dir/(std::to_string(rate)+".wav"); fixture(file,rate); decode(file); }
    AudioRenderer renderer;
    check(!std::isfinite(renderer.mediaTimeMs()), "unstarted clock is invalid");
    AudioPipeline pipe; check(pipe.open((dir/"48000.wav").wstring()),"open renderer fixture");
    if (!renderer.start(pipe.pcmFormat())) { check(false,"WASAPI endpoint initialization"); renderer.shutdown(); return 1; }
    renderer.setGain(0);
    pipe.startThread(&renderer);
    check(until([&]{return renderer.started();}),"WASAPI prefilled and started");
    check(renderer.framesWritten()>0 && std::isfinite(renderer.mediaTimeMs()),"valid clock only after actual PCM submission");
    std::this_thread::sleep_for(80ms); pipe.setPaused(true); std::this_thread::sleep_for(100ms);
    const auto before=renderer.mediaTimeMs(); std::this_thread::sleep_for(70ms);
    check(std::abs(renderer.mediaTimeMs()-before)<2,"paused device clock remains fixed");
    const auto seek=pipe.requestSeek(250);
    std::this_thread::sleep_for(30ms);
    check(std::abs(seek-250)<0.05 && std::abs(renderer.mediaTimeMs()-250)<20,"paused seek reanchors actual PCM");
    pipe.stopThread(); renderer.stopAndReset();
    check(!std::isfinite(renderer.mediaTimeMs()),"reset invalidates clock");
    renderer.shutdown();
    struct StretchedPcm final:AudioPcmSource {
        size_t frames=0;
        size_t pull(float* samples,size_t count,double* pts)override{*pts=100+frames*500.0/kAudioRate;std::fill_n(samples,count*2,0.0f);frames+=count;return count;}
        std::optional<double> lastPullEndPtsMs()const override{return 100+frames*500.0/kAudioRate;}
    } stretched;
    check(renderer.start()&&renderer.startAnchored(stretched),"start mapped resampled PCM");
    const auto wall=std::chrono::steady_clock::now();
    while(std::chrono::steady_clock::now()-wall<300ms){double pts=0;if(!renderer.pumpOnce(stretched,&pts))break;}
    const double elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-wall).count();
    check(std::abs(renderer.mediaTimeMs()-(100+elapsed*.5))<12,"device clock follows media span at half speed, not output sample duration");
    renderer.stopAndReset();check(!std::isfinite(renderer.mediaTimeMs()),"reset clears mapped resampler timeline");
    renderer.shutdown();
    struct LivePcm final:AudioPcmSource {
        bool dry=false;double pts=100;
        size_t pull(float* samples,size_t count,double* first)override{*first=pts;if(dry)return 0;std::fill_n(samples,count*2,0.0f);pts+=count*1000.0/kAudioRate;return count;}
        std::optional<double> lastPullEndPtsMs()const override{return dry?std::nullopt:std::optional<double>(pts);}
        bool padUnderruns()const override{return false;}
    } live;
    check(renderer.start()&&renderer.startAnchored(live),"start live PCM timeline");
    live.dry=true;
    const auto stall=std::chrono::steady_clock::now();
    while(std::chrono::steady_clock::now()-stall<100ms){double pts=0;if(!renderer.pumpOnce(live,&pts))break;}
    check(!std::isfinite(renderer.mediaTimeMs()),"starved live endpoint has no invented media timestamp");
    live.dry=false;live.pts=500;
    const auto recovery=std::chrono::steady_clock::now();
    while(std::chrono::steady_clock::now()-recovery<100ms){double pts=0;if(!renderer.pumpOnce(live,&pts))break;}
    const double recovered=renderer.mediaTimeMs();
    check(std::isfinite(recovered)&&recovered>=500&&recovered<=live.pts,"recovered live clock references new PCM after device silence");
    std::array<float,480> fadeSamples;fadeSamples.fill(1);
    fadeStereoTail(fadeSamples.data(),240,.25f);
    bool monotonic=true;for(size_t i=1;i<240;++i)monotonic&=fadeSamples[i*2]<=fadeSamples[(i-1)*2]&&fadeSamples[i*2]==fadeSamples[i*2+1];
    check(fadeSamples.front()==.25f&&fadeSamples.back()==0&&monotonic,"five millisecond PCM fade reaches zero monotonically on both channels");
    std::atomic<bool> cancelFade=false;
    const auto faded=renderer.fadeAndReset(live,cancelFade);
    check(faded==AudioFadeResult::Drained,"real WASAPI consumes submitted fade before reset");
    check(!renderer.started()&&!std::isfinite(renderer.mediaTimeMs()),"fade reset invalidates old media clock");
    check(renderer.startAnchored(live,true),"prefill paused endpoint after fade");
    check(renderer.fadeAndReset(live,cancelFade)==AudioFadeResult::NotPlaying,"already paused endpoint resets without starting a fade");
    check(renderer.startAnchored(live),"resume fresh PCM after controlled reset");
    cancelFade=true;const auto cancelBegin=std::chrono::steady_clock::now();
    check(renderer.fadeAndReset(live,cancelFade)==AudioFadeResult::Cancelled&&std::chrono::steady_clock::now()-cancelBegin<50ms,"cancellation does not wait for fade deadline");
    renderer.shutdown();
    const auto longFile=dir/"recovery.wav";fixture(longFile,48000,4000);
    AudioPipeline recoveryPipe;AudioRenderer recoveryRenderer;recoveryRenderer.setGain(0);
    check(recoveryPipe.open(longFile.wstring()),"open recovery PCM fixture");
    recoveryPipe.startThread(&recoveryRenderer,true);
    check(until([&]{return recoveryRenderer.started()&&!recoveryPipe.endpointRecovering();}),"audio thread owns endpoint initialization");
    std::atomic<bool> reading=true;std::atomic<uint64_t> reads=0;
    std::thread reader([&]{while(reading){(void)recoveryRenderer.mediaTimeMs();++reads;std::this_thread::sleep_for(1ms);}});
    std::this_thread::sleep_for(100ms);
    const double beforeLoss=recoveryRenderer.mediaTimeMs();
    recoveryPipe.requestEndpointLossForTest();
    check(until([&]{return recoveryPipe.endpointRecovering();}),"endpoint loss publishes recovery state");
    check(!std::isfinite(recoveryRenderer.mediaTimeMs()),"released endpoint clock is invalid during retry");
    check(recoveryPipe.endpointError()==AUDCLNT_E_DEVICE_INVALIDATED,"endpoint HRESULT remains available");
    check(until([&]{return recoveryPipe.endpointRecoveries()==1&&!recoveryPipe.endpointRecovering();}),"endpoint reopens without exiting audio thread");
    const double restored=recoveryRenderer.mediaTimeMs();
    std::cout<<"recovery before="<<beforeLoss<<" restored="<<restored<<'\n';
    check(restored>=beforeLoss-30&&restored<=beforeLoss+50,"recovery replays from last clock, not decoded-ahead head");
    const double liveSeek=recoveryPipe.requestSeek(600);
    check(std::abs(liveSeek-600)<.05&&until([&]{return recoveryRenderer.mediaTimeMs()>=600;}),"playing file seek drains fade and anchors requested PCM");
    recoveryPipe.setPaused(true);std::this_thread::sleep_for(70ms);
    recoveryPipe.requestEndpointLossForTest();
    check(until([&]{return recoveryPipe.endpointRecovering();}),"paused endpoint loss is detected");
    const double seekRecovered=recoveryPipe.requestSeek(1234.25);
    check(std::abs(seekRecovered-1234.25)<.05,"seek while disconnected updates recovery PTS");
    check(until([&]{return recoveryPipe.endpointRecoveries()==2&&!recoveryPipe.endpointRecovering();}),"paused endpoint reconnects");
    const double pausedRecovered=recoveryRenderer.mediaTimeMs();std::this_thread::sleep_for(100ms);
    check(std::abs(pausedRecovered-1234.25)<.1&&std::abs(recoveryRenderer.mediaTimeMs()-pausedRecovered)<.1,"reconnect does not start paused PCM");
    recoveryPipe.setPaused(false);
    check(until([&]{return recoveryRenderer.mediaTimeMs()>pausedRecovered+50;}),"resume after reconnect advances real endpoint clock");
    recoveryPipe.requestEndpointLossForTest();
    check(until([&]{return recoveryPipe.endpointRecovering();}),"third loss enters retry");
    const auto stopping=std::chrono::steady_clock::now();recoveryPipe.stopThread();
    check(std::chrono::steady_clock::now()-stopping<300ms,"stop interrupts endpoint retry promptly");
    reading=false;reader.join();
    std::cout<<"concurrentClockReads="<<reads.load()<<'\n';
    check(reads>10&&!std::isfinite(recoveryRenderer.mediaTimeMs()),"concurrent clock readers survive endpoint releases and shutdown");
    check(recoveryPipe.overruns()==0,"endpoint outages preserve bounded file PCM queue");
    recoveryRenderer.shutdown();
    // Real PCM and a real WASAPI clock with a deliberately stalled video owner.
    // No GPU-duration estimate and no physical capture delay are fed to audio.
    AudioPipeline gated;AudioRenderer gatedRenderer;gatedRenderer.setGain(0);
    check(gated.open(longFile.wstring()),"open software video-delay fixture");
    gated.holdForVideo();gated.startThread(&gatedRenderer,true);
    check(until([&]{return gatedRenderer.started()&&!gated.endpointRecovering();}),"video gate prefills endpoint without playing it");
    const double initial=gatedRenderer.mediaTimeMs();
    gated.videoReady(0);std::this_thread::sleep_for(350ms);
    check(std::abs(gatedRenderer.mediaTimeMs()-initial)<.1,"350ms NR startup and GPU-ready alone cannot advance sound");
    gated.videoPresented(100);
    // Sustained under-rate: the video owner presents nothing for 600ms while
    // coverage stays at 100ms. The audio master clock keeps playing real PCM
    // at one-times speed — steady-state video lag never pauses sound (P1).
    const auto underrateBegin=std::chrono::steady_clock::now();
    std::this_thread::sleep_for(600ms);
    const double underrateWall=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-underrateBegin).count();
    const double underrateMedia=gatedRenderer.mediaTimeMs();
    std::cout<<"VIDEO_GATE underrateWallMs="<<underrateWall<<" underrateMediaMs="<<underrateMedia<<" coverageMs=100 waits="<<gated.videoWaitCount()<<'\n';
    check(!gated.waitingForVideo(),"sustained video under-rate never marks audio rebuffering");
    check(std::abs(underrateMedia-underrateWall)<35,"600ms video stall keeps one-times continuous audio");
    check(gatedRenderer.underruns()==0,"under-rate does not starve the endpoint or insert silence");
    gated.videoPresented(1200);
    check(until([&]{return gatedRenderer.mediaTimeMs()>underrateMedia+50;}),"audio keeps advancing after video coverage catches up");
    gated.holdForVideo();std::this_thread::sleep_for(70ms);
    const double rebuilding=gatedRenderer.mediaTimeMs();std::this_thread::sleep_for(250ms);
    check(std::abs(gatedRenderer.mediaTimeMs()-rebuilding)<.1,"settings rebuild freezes PCM position without skipping samples");
    const auto target=gated.requestSeek(1200);
    gated.videoReady(1200);std::this_thread::sleep_for(200ms);
    check(std::abs(target-1200)<.1&&std::abs(gatedRenderer.mediaTimeMs()-1200)<.1,"playing seek stays held until new video is presented");
    gated.setPaused(true);gated.videoPresented(1300);std::this_thread::sleep_for(70ms);
    check(std::abs(gatedRenderer.mediaTimeMs()-1200)<.1,"paused seek preview cannot start audio");
    gated.holdForVideo();gated.setPaused(false);std::this_thread::sleep_for(150ms);
    check(std::abs(gatedRenderer.mediaTimeMs()-1200)<.1,"resume waits for video history warmup");
    gated.videoPresented(1300);
    check(until([&]{return gatedRenderer.mediaTimeMs()>1250;}),"resume uses real retained PCM after video anchor");
    check(gated.videoWaitCount()>=3&&gated.overruns()==0,"explicit transport holds are observed and PCM queue remains bounded");
    gated.stopThread();
    return failures ? 1 : 0;
}
