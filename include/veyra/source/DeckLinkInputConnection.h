#pragma once
#include <array>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace veyra::source::decklink {
// Stable SDK values, without putting SDK headers in the public/source tree.
inline constexpr std::array<uint32_t,6> inputConnections{1,2,4,8,16,32};
inline bool knownInputConnection(uint32_t value){
    for(auto port:inputConnections)if(value==port)return true;
    return false;
}
inline std::wstring_view inputConnectionName(uint32_t value){
    switch(value){case 1:return L"SDI";case 2:return L"HDMI";case 4:return L"Optical SDI";
    case 8:return L"Component";case 16:return L"Composite";case 32:return L"S-Video";default:return L"Driver";}
}
inline std::wstring inputConnectionPath(std::wstring_view base,uint32_t connection){
    return knownInputConnection(connection)?std::format(L"{}:input={:08x}",base,connection):std::wstring{};
}
struct InputConnectionSelection {std::wstring_view device;uint32_t connection=0;bool valid=false;};
inline InputConnectionSelection inputConnectionSelection(std::wstring_view path){
    if(!path.starts_with(L"decklink-native:"))return {};
    const auto at=path.find(L":input=");
    if(at==path.npos)return {path,0,true}; // exact legacy device identity
    const auto value=path.substr(at+7);
    if(value.size()!=8)return {};
    uint32_t port=0;
    for(auto c:value){
        const unsigned nibble=c>=L'0'&&c<=L'9'?c-L'0':c>=L'a'&&c<=L'f'?c-L'a'+10:c>=L'A'&&c<=L'F'?c-L'A'+10:16;
        if(nibble>15)return {};port=(port<<4)|nibble;
    }
    return knownInputConnection(port)?InputConnectionSelection{path.substr(0,at),port,true}:InputConnectionSelection{};
}

// The owner thread changes only an explicit user selection. It does not write
// driver preferences. On close, restore only if nobody else changed the port.
// Read/write callbacks are the same control path exercised by driver tests.
class InputConnectionLease {
    uint32_t original_=0,selected_=0;
    bool changed_=false;
public:
    template<class Read,class Write>
    bool apply(uint32_t requested,uint32_t supported,Read read,Write write){
        if(!requested)return true;
        if(changed_){uint32_t actual=0;return requested==selected_&&read(actual)&&actual==selected_;}
        if(!knownInputConnection(requested)||(supported&requested)==0||!read(original_))return false;
        selected_=requested;
        if(original_==selected_)return true;
        changed_=true;
        if(!write(selected_))return false;
        uint32_t actual=0;return read(actual)&&actual==selected_;
    }
    template<class Read,class Write>
    bool restore(Read read,Write write){
        if(!changed_)return true;
        uint32_t actual=0;
        if(!read(actual))return false;
        if(actual!=selected_){changed_=false;return true;}
        if(!write(original_)||!read(actual)||actual!=original_)return false;
        changed_=false;return true;
    }
};
}
