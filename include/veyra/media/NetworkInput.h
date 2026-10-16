#pragma once
#include <atomic>
#include <chrono>
#include <cwctype>
#include <string>
#include <string_view>
#include <mutex>
extern "C" {
#include <libavformat/avformat.h>
}
namespace veyra::media {
inline bool networkMediaInput(std::wstring_view path) {
    const auto colon=path.find(L':');
    if(colon==std::wstring_view::npos||colon>5)return false;
    std::wstring scheme(path.substr(0,colon));
    for(auto& ch:scheme)ch=std::towlower(ch);
    return scheme==L"http"||scheme==L"https"||scheme==L"rtsp";
}
// Per-operation deadlines only on networks; immediate owner cancellation on all inputs.
class InputInterrupt {
public:
    void configure(bool network,const std::atomic<bool>* owner,const std::atomic<bool>* local=nullptr){network_=network;owner_=owner;local_=local;deadline_=0;}
    void arm(){deadline_=network_?now()+5000000000ll:0;}
    void disarm(){deadline_=0;}
    static int callback(void* opaque){
        const auto& self=*static_cast<InputInterrupt*>(opaque);const auto deadline=self.deadline_.load();
        return (self.owner_&&self.owner_->load())||(self.local_&&self.local_->load())||(deadline&&now()>=deadline);
    }
private:
    static int64_t now(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
    const std::atomic<bool>* owner_=nullptr;const std::atomic<bool>* local_=nullptr;
    bool network_=false;std::atomic<int64_t> deadline_{0};
};
inline void networkMediaOptions(AVDictionary** options){
    static std::once_flag initialized;std::call_once(initialized,[]{avformat_network_init();});
    av_dict_set(options,"rw_timeout","5000000",0);av_dict_set(options,"timeout","5000000",0);
    av_dict_set(options,"rtsp_transport","tcp",0);
}
}
