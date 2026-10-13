#pragma once
#include <windows.h>
#include <cstdint>
#include <mutex>
#include <string>
namespace veyra::gfx {
// Variable refresh (G-SYNC / G-SYNC Compatible) state of the monitor showing
// a window, for diagnostics and the status line. Veyra never changes it; the
// user enables it in the NVIDIA Control Panel.
// Minimal ABI declarations checked against NVIDIA/nvapi public interfaces
// (nvapi_interface.h function IDs, NV_GET_VRR_INFO_V1 layout), MIT; same
// source and loading rules as ReflexSession: System32 nvapi64.dll only, pinned
// for the life of the process. No SDK header or runtime is distributed.
struct VrrState {
    bool known=false;      // NVIDIA driver answered for this monitor
    bool possible=false;   // the display can run VRR with the current mode/connection
    bool requested=false;  // VRR is enabled for this display in the driver
    bool enabled=false;    // VRR mode is currently active on the display
    bool indicator=false;  // the driver's VRR indicator overlay is on
    bool inVrrMode=false;  // the display itself reports VRR mode
    int status=0;          // last NvAPI status (0 = OK)
    bool operator==(const VrrState&)const=default;
};
inline VrrState queryVrr(HWND window){
    struct Info {uint32_t version;uint32_t flags;uint32_t reservedEx[4];};
    static_assert(sizeof(Info)==24);
    using Query=void*(__cdecl*)(uint32_t);
    using Init=int(__cdecl*)();
    using IdByName=int(__cdecl*)(const char*,uint32_t*);
    using GetVrr=int(__cdecl*)(uint32_t,Info*);
    static std::once_flag once;static IdByName idByName=nullptr;static GetVrr getVrr=nullptr;static bool ready=false;
    std::call_once(once,[]{
        wchar_t system[MAX_PATH]{};GetSystemDirectoryW(system,MAX_PATH);
        const std::wstring path=std::wstring(system)+L"\\nvapi64.dll";
        HMODULE module=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!module)return;
        HMODULE pinned=nullptr;GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(module),&pinned);
        auto query=reinterpret_cast<Query>(GetProcAddress(module,"nvapi_QueryInterface"));
        if(!query)return;
        auto init=reinterpret_cast<Init>(query(0x0150E828));
        idByName=reinterpret_cast<IdByName>(query(0xAE457190));
        getVrr=reinterpret_cast<GetVrr>(query(0xDF8FDA57));
        ready=init&&idByName&&getVrr&&init()==0;
    });
    VrrState state;
    if(!ready||!window)return state;
    MONITORINFOEXW monitor{};monitor.cbSize=sizeof(monitor);
    if(!GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor))return state;
    char name[CCHDEVICENAME*2]{};
    if(!WideCharToMultiByte(CP_ACP,0,monitor.szDevice,-1,name,int(sizeof(name)),nullptr,nullptr))return state;
    uint32_t displayId=0;
    state.status=idByName(name,&displayId);
    if(state.status!=0)return state;
    Info info{};info.version=uint32_t(sizeof(Info))|(1u<<16);
    state.status=getVrr(displayId,&info);
    if(state.status!=0)return state;
    state.known=true;
    state.enabled=info.flags&1u;state.possible=info.flags&2u;state.requested=info.flags&4u;
    state.indicator=info.flags&8u;state.inVrrMode=info.flags&16u;
    return state;
}
}
