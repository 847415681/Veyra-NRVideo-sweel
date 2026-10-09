#include "veyra/engine/Subtitles.h"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <thread>

int wmain(int argc,wchar_t** argv){
    using namespace veyra::engine;
    if(argc!=4)return 2; // authored fixtures, two independent public PGS files
    const std::filesystem::path root=argv[1];
    int checks=0,failures=0;
    auto check=[&](bool ok,const char* label){++checks;failures+=!ok;std::cout<<(ok?"PASS ":"FAIL ")<<label<<'\n';};
    auto times=[&](const SubtitleTrack& track){
        check(track.usable()&&track.cues.size()==2,"two authored cues decoded");
        if(track.cues.size()!=2)return;
        check(std::abs(track.cues[0].begin-1)<.015&&std::abs(track.cues[0].end-3)<.015,"first display timing");
        check(std::abs(track.cues[1].begin-5)<.015&&std::abs(track.cues[1].end-7)<.015,"last display timing");
        for(double at:{0.,3.,4.,7.,8.})check(cuesAt(track,at).empty(),"clear gap and exact end boundary");
        for(double at:{6.,2.,5.,1.})check(cuesAt(track,at).size()==1,"backward seek and exact start boundary");
    };
    for(const auto* name:{L"microdvd.sub",L"mpl2.mpl",L"subviewer.sub",L"subviewer.sbv",L"spruce.stl",L"sami.smi",L"sami.sami"}){
        std::wcout<<L"FORMAT "<<name<<L'\n';
        const auto track=loadSubtitleFile((root/name).wstring());times(track);
        if(track.cues.size()==2){
            check(track.cues[0].text==L"First\nTwo","authored line break preserved through FFmpeg text decoder");
            check(track.cues[1].text==L"Second","plain text retained");
        }
        check(!track.embedded,"external text is labeled external");
    }
    const auto unicode=loadSubtitleFile((root/L"中文 路径.srt").wstring());times(unicode);
    check(unicode.usable()&&unicode.cues[0].text==L"中文，第一行\nEnglish, second line","Unicode content and path retained");
    const auto sniffed=loadSubtitleFile((root/L"字幕.txt").wstring());
    check(sniffed.usable()&&sniffed.cues[0].text==unicode.cues[0].text,"text extension uses recognizable subtitle content");
    for(const auto* name:{L"bitmap.sup",L"bitmap.pgs",L"vobsub.idx",L"vobsub.sub"}){
        std::wcout<<L"BITMAP "<<name<<L'\n';
        const auto tracks=loadExternalSubtitleTracks((root/name).wstring());
        const bool dvd=std::wstring_view(name).starts_with(L"vobsub");
        check(tracks.size()==size_t(dvd?2:1),"all external bitmap languages retained");
        for(const auto& track:tracks){
            times(track);check(!track.embedded,"external bitmap is labeled external");
            check(track.usable()&&track.cues[0].bitmap&&!track.cues[0].bitmap->images.empty(),"decoded bitmap reaches cue");
            if(track.usable()&&track.cues[0].bitmap){
                const auto& frame=*track.cues[0].bitmap;
                check(frame.width==1280&&frame.height==720,"authored bitmap canvas");
                for(const auto& image:frame.images)check(!image.pixels().empty(),"cached bitmap expands to real pixels");
            }
        }
    }
    bool loading=false,progress=false;
    const auto compressed=loadEmbeddedSubtitleTracks((root/L"compressed.mkv").wstring(),{},[&](const auto& tracks){
        if(tracks.size()==5){loading|=!tracks[0].usable()&&tracks[0].note==L"正在读取字幕";progress|=tracks[0].usable();}
    });
    const auto plain=loadEmbeddedSubtitleTracks((root/L"plain.mkv").wstring());
    check(loading&&progress,"loading state and first cue published during scan");
    check(compressed.size()==5&&plain.size()==5,"five compressed and five plain PGS languages discovered");
    for(size_t k=0;k<compressed.size();++k){
        times(compressed[k]);check(compressed[k].note.empty(),"successful track clears loading status");
        if(k<plain.size()&&compressed[k].cues.size()==2&&plain[k].cues.size()==2){
            const auto a=compressed[k].cues[0].bitmap,b=plain[k].cues[0].bitmap;
            check(a&&b&&a->images[0].pixels()==b->images[0].pixels(),"compressed PGS pixels match uncompressed reference");
        }
    }
    const auto duration=loadEmbeddedSubtitleTracks((root/L"duration-only.mkv").wstring());
    check(duration.size()==1,"PGS with container duration discovered");
    if(duration.size()==1)times(duration[0]);
    const auto corrupt=loadEmbeddedSubtitleTracks((root/L"corrupt.mkv").wstring());
    check(corrupt.size()==1&&!corrupt[0].usable()&&!corrupt[0].note.empty()&&corrupt[0].note!=L"正在读取字幕","broken PGS reports a final reason");
    for(const auto* name:{L"broken.sup",L"missing.idx",L"orphan.sub",L"absent.sup",L"oversized.srt",L"oversized-vobsub.sub"}){
        const auto track=loadSubtitleFile((root/name).wstring());
        check(!track.usable()&&!track.note.empty(),"bad or incomplete external file reports reason");
    }
    for(int k=2;k<4;++k){
        const auto tracks=loadEmbeddedSubtitleTracks(argv[k]);
        check(tracks.size()==1&&tracks[0].usable()&&tracks[0].cues[0].bitmap,"independent public PGS decodes actual bitmap");
    }
    std::stop_source stopped;stopped.request_stop();
    check(loadExternalSubtitleTracks((root/L"bitmap.sup").wstring(),stopped.get_token()).empty(),"pre-cancelled external load performs no work");
    SubtitleLoader loader;
    auto await=[&](uint64_t id){
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(std::chrono::steady_clock::now()<deadline){
            if(auto result=loader.poll()){
                check(result->generation==id,"no cancelled result reaches current generation");
                if(result->generation==id&&result->complete)return *result;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return SubtitleLoader::Result{};
    };
    loader.requestFile((root/L"vobsub.idx").wstring());
    check(await(loader.requestFile(L"")).tracks.empty(),"cancelled external work cannot add old tracks");
    const auto loaded=await(loader.requestFile((root/L"vobsub.idx").wstring()));
    check(loaded.complete&&loaded.tracks.size()==2&&loaded.tracks[0].usable(),"async external VobSub retains both languages");
    loader.requestFile((root/L"compressed.mkv").wstring());
    const auto replaced=await(loader.request((root/L"duration-only.mkv").wstring()));
    check(replaced.complete&&replaced.tracks.size()==1&&replaced.tracks[0].embedded,"changing from external file to source cancels stale task");
    std::cout<<"checks="<<checks<<" failures="<<failures<<'\n';
    return failures?1:0;
}
