#include "veyra/engine/Subtitles.h"
#include "veyra/engine/SubtitleCacheBudget.h"
#include <cstdlib>
#include <iostream>
#include <numeric>

int wmain(int argc,wchar_t** argv){
    using namespace veyra::engine;
    if(argc!=2)return 2;
    int failures=0;auto check=[&](bool ok,const char* text){std::cout<<(ok?"PASS ":"FAIL ")<<text<<std::endl;failures+=!ok;};
    check(subtitleCacheBudgetBytes({})==4294967296ull,"default budget is 4 GiB, not 32-bit overflow to zero");
    check(subtitleCacheBudgetBytes("65536")==68719476736ull&&subtitleCacheBudgetBytes("1")==1048576ull,"environment boundary values use 64-bit arithmetic");
    for(auto value:{"0","-1","65537","64junk"," 64","1.5","18446744073709551616"})
        check(subtitleCacheBudgetBytes(value)==kDefaultSubtitleCacheBytes,"malformed budget falls back safely");
    _putenv_s("VEYRA_SUBTITLE_CACHE_MB","64");const auto capped=loadEmbeddedSubtitleTracks(argv[1]);
    _putenv_s("VEYRA_SUBTITLE_CACHE_MB","");const auto complete=loadEmbeddedSubtitleTracks(argv[1]);
    check(capped.size()==5&&complete.size()==5,"five PGS languages retained in both budget modes");
    size_t stored=0;unsigned fullTracks=0,cappedTracks=0;
    for(const auto& t:complete){
        bool ok=t.usable()&&t.note.empty()&&t.cues.size()==40;
        if(ok){++fullTracks;const auto last=cuesAt(t,119.0);ok=last.size()==1&&last.front()->bitmap;
            if(ok){const auto pixels=last.front()->bitmap->images.front().pixels();ok=pixels.size()==512u*192u&&pixels.front()==pixels[2]&&pixels.front()!=pixels[1];}}
        check(ok,"late subtitle survives beyond old 64 MiB total and expands to authored pixels");
        for(const auto& cue:t.cues)if(cue.bitmap)for(const auto& image:cue.bitmap->images)stored+=image.runs.size()*sizeof(uint32_t);
    }
    for(const auto& t:capped)if(t.cues.size()<40&&!t.note.empty())++cappedTracks;
    check(stored>64ull*1024*1024&&stored<96ull*1024*1024,"functional fixture crosses old limit with bounded actual cache");
    check(fullTracks==5&&cappedTracks>0,"same decoder reproduces old cutoff and corrected late cues");
    std::cout<<"CACHE runBytes="<<stored<<" fullTracks="<<fullTracks<<" oldBudgetTruncated="<<cappedTracks<<" failures="<<failures<<std::endl;
    return failures?1:0;
}
