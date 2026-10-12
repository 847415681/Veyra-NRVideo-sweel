#pragma once
#include "veyra/source/CaptureFrameRate.h"
#include <algorithm>
#include <cstdint>
#include <format>
#include <span>
#include <vector>

namespace veyra::source {
// `pixel` / `scan` / `interlaced` describe the same media type as `key`.
// The capture dialog offers them as three linked choices; `key` stays the
// persisted identity so an old combined selection still restores one type.
struct CaptureFormat {int index=0;unsigned width=0,height=0;double fps=0;std::wstring label;std::wstring key;int rank=0;int tier=0;bool driverCurrent=false;std::wstring pixel;std::wstring scan;bool interlaced=false;int64_t interval100ns=0;bool supported=true;bool automatic=false;};
enum class CaptureFormatAxis { Resolution, FrameRate };
struct CaptureFormatOption { std::wstring id, label; bool disabled=false; };

inline const CaptureFormat* selectCaptureFormat(std::span<const CaptureFormat> formats,int legacyIndex,std::wstring_view key){
    for(const auto& format:formats)
        if(key.empty()?format.index==legacyIndex:format.key==key)return &format;
    // Old VideoInfo2 keys omitted interlace/field-order flags. A single match
    // is safe to migrate; p30/i60 or top/bottom-field ambiguity requires the
    // user to choose explicitly. Never guess a mode from driver list order.
    if(!key.empty()&&key.find(L":scan=")==key.npos){
        const CaptureFormat* match=nullptr;const std::wstring prefix=std::wstring(key)+L":scan=";
        for(const auto& format:formats)if(format.key.starts_with(prefix)){
            if(match)return nullptr;match=&format;
        }
        return match;
    }
    return nullptr;
}

inline std::wstring captureResolutionId(const CaptureFormat& format){
    if(format.automatic)return L"auto";
    return std::format(L"{}x{}",format.width,format.height);
}
inline std::wstring captureFrameRateId(const CaptureFormat& format){
    if(format.automatic)return L"auto";
    // Progressive and interlaced modes that share a numeric rate stay distinct.
    // Field order stays on the pixel list, so it does not create two identical fps rows.
    return std::format(L"{}|{}",format.interval100ns>0?format.interval100ns:format.fps>0?captureFrameInterval(format.fps):0,format.interlaced?1:0);
}
inline std::wstring captureResolutionLabel(const CaptureFormat& format){
    if(format.automatic)return L"自动检测实际信号";
    return std::format(L"{} × {}",format.width,format.height);
}
inline std::wstring captureFrameRateLabel(const CaptureFormat& format){
    if(format.automatic)return L"跟随实际信号";
    if(format.fps<=0)return L"设备默认帧率";
    auto label=std::format(L"{:.3f} fps",format.fps);
    if(!format.scan.empty())label+=L" · "+format.scan;
    return label;
}
inline std::wstring capturePixelLabel(const CaptureFormat& format,bool ambiguous){
    std::wstring label=format.pixel.empty()?L"未知格式":format.pixel;
    if(ambiguous){
        if(!format.scan.empty())label+=L" · "+format.scan;
        label+=std::format(L" [format {}]",format.index);
    }
    if(format.driverCurrent)label+=L" · 驱动默认";
    if(!format.supported)label+=L" · 当前播放器不支持";
    return label;
}
inline std::vector<CaptureFormatOption> captureResolutionOptions(std::span<const CaptureFormat> formats){
    std::vector<CaptureFormatOption> out;
    for(const auto& format:formats){
        const auto id=captureResolutionId(format);
        if(std::any_of(out.begin(),out.end(),[&](const CaptureFormatOption& option){return option.id==id;}))continue;
        out.push_back({id,captureResolutionLabel(format)});
    }
    return out;
}
inline std::vector<CaptureFormatOption> captureFrameRateOptions(std::span<const CaptureFormat> formats,const CaptureFormat* current){
    std::vector<CaptureFormatOption> out;
    if(!current)return out;
    const auto resolution=captureResolutionId(*current);
    for(const auto& format:formats){
        if(captureResolutionId(format)!=resolution)continue;
        const auto id=captureFrameRateId(format);
        if(std::any_of(out.begin(),out.end(),[&](const CaptureFormatOption& option){return option.id==id;}))continue;
        auto label=captureFrameRateLabel(format);
        if(std::any_of(out.begin(),out.end(),[&](const CaptureFormatOption& option){return option.label==label;}))
            label+=std::format(L" [format {}]",format.index);
        out.push_back({id,std::move(label)});
    }
    return out;
}
inline std::vector<CaptureFormatOption> capturePixelOptions(std::span<const CaptureFormat> formats,const CaptureFormat* current){
    std::vector<CaptureFormatOption> out;
    if(!current)return out;
    const auto resolution=captureResolutionId(*current);
    const auto rate=captureFrameRateId(*current);
    std::vector<const CaptureFormat*> matched;
    for(const auto& format:formats)
        if(captureResolutionId(format)==resolution&&captureFrameRateId(format)==rate)matched.push_back(&format);
    for(const auto* format:matched){
        int copies=0;
        for(const auto* other:matched)if(other->pixel==format->pixel)++copies;
        if(std::any_of(out.begin(),out.end(),[&](const CaptureFormatOption& option){return option.id==format->key;}))continue;
        out.push_back({format->key,capturePixelLabel(*format,copies>1),!format->supported});
    }
    return out;
}
// Keep the other two axes when the newly chosen resolution or frame rate still
// offers them. Otherwise take the first remaining media type in list order
// (driver-current, then lower latency). Never invent a triple the device did
// not enumerate.
inline const CaptureFormat* retargetCaptureFormat(std::span<const CaptureFormat> formats,const CaptureFormat* current,CaptureFormatAxis axis,std::wstring_view id){
    if(id.empty()||(axis==CaptureFormatAxis::FrameRate&&!current))return nullptr;
    const auto resolution=current?captureResolutionId(*current):std::wstring{};
    const auto rate=current?captureFrameRateId(*current):std::wstring{};
    const auto pixel=current?current->pixel:std::wstring{};
    const CaptureFormat* fallback=nullptr;
    const CaptureFormat* samePixel=nullptr;
    const CaptureFormat* sameRate=nullptr;
    const CaptureFormat* sameBoth=nullptr;
    for(const auto& format:formats){
        const bool match=axis==CaptureFormatAxis::Resolution?captureResolutionId(format)==id
            :captureResolutionId(format)==resolution&&captureFrameRateId(format)==id;
        if(!match)continue;
        if(!fallback)fallback=&format;
        if(!current)continue;
        const bool rateMatch=captureFrameRateId(format)==rate;
        const bool pixelMatch=!pixel.empty()&&format.pixel==pixel;
        if(rateMatch&&pixelMatch){if(!sameBoth)sameBoth=&format;}
        else if(rateMatch){if(!sameRate)sameRate=&format;}
        else if(pixelMatch){if(!samePixel)samePixel=&format;}
    }
    if(axis==CaptureFormatAxis::FrameRate)return samePixel?samePixel:fallback;
    return sameBoth?sameBoth:sameRate?sameRate:samePixel?samePixel:fallback;
}

inline std::wstring capturePathOptions(double fps,std::wstring_view encodedKey){
    std::wstring result;
    if(fps>0)result=std::format(L"?fps={:.6f}",fps);
    if(!encodedKey.empty())result+=std::format(L"{}format={}",result.empty()?L"?":L"&",encodedKey);
    return result;
}

inline bool parseCapturePathOptions(std::wstring_view query,double& fps,std::wstring& encodedKey){
    fps=0;encodedKey.clear();bool haveFps=false,haveKey=false;
    if(query.empty())return false;
    while(!query.empty()){
        const auto end=query.find(L'&');const auto option=query.substr(0,end);
        if(option.starts_with(L"fps=")){
            if(haveFps||!parseCaptureFrameRate(option.substr(4),fps))return false;
            haveFps=true;
        }else if(option.starts_with(L"format=")){
            const auto key=option.substr(7);
            if(haveKey||key.empty()||key.size()>4096||key.size()%4||key.find_first_not_of(L"0123456789abcdefABCDEF")!=std::wstring_view::npos)return false;
            haveKey=true;encodedKey=key;
        }else return false;
        if(end==std::wstring_view::npos)break;
        query.remove_prefix(end+1);
        if(query.empty())return false;
    }
    return true;
}
}
