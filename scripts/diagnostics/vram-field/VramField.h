#pragma once
// Diagnostic-only instrumentation. No SDK hooks, resource references, GPU work
// or waits. Only resources created at the explicitly replaced call sites count.
#include "veyra/pipeline/GpuPassUtils.h"
#include <dxgi1_6.h>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace vramField {
inline uint64_t tick() { return GetTickCount64(); }
inline uint64_t utcMs() {
    FILETIME ft{}; GetSystemTimeAsFileTime(&ft);
    return ((uint64_t(ft.dwHighDateTime)<<32)|ft.dwLowDateTime)/10000-11644473600000ull;
}
struct Item { uint64_t id,bytes,born; unsigned group; };
struct Retired { Item item; uint64_t utc,time; };
struct Ledger {
    std::mutex mutex;
    std::map<uint64_t,Item> live;
    uint64_t next=0,created=0,destroyed=0,liveBytes=0,peakBytes=0,attachFailures=0,unknownSizes=0;
    uint64_t createdBy[3]{},destroyedBy[3]{},bytesBy[3]{};
    std::vector<Retired> retired;
    uint64_t droppedReleaseEvents=0;
};
struct Adapter {
    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter;
    ID3D12Fence* fence=nullptr; // protected by State::mutex; unregister before owner release
    uint64_t luid=0,generation=0;
    bool active=false;
};
struct State {
    std::shared_ptr<Ledger> ledger=std::make_shared<Ledger>();
    std::mutex mutex;
    std::condition_variable cv;
    std::map<const void*,Adapter> adapters;
    std::ofstream samples,events,windows,live;
    uint64_t generation=0;
    bool enabled=false,quit=false;
    std::thread worker;
    State() {
        wchar_t dir[32768]{};
        const auto n=GetEnvironmentVariableW(L"VEYRA_VRAM_DIAGNOSTIC_DIR",dir,32768);
        if(!n||n>=32768)return;
        const std::filesystem::path path(dir);
        samples.open(path/L"dxgi-memory.csv",std::ios::binary);
        events.open(path/L"resource-events.csv",std::ios::binary);
        windows.open(path/L"windows.csv",std::ios::binary);
        live.open(path/L"tracked-live-resources.csv",std::ios::binary);
        enabled=bool(samples)&&bool(events)&&bool(windows)&&bool(live);
        if(!enabled)return;
        samples<<"utcMs,tickMs,pid,luid,generation,contextActive,segment,queryHr,usageBytes,budgetBytes,reservationBytes,availableReservationBytes,fenceAvailable,fenceCompleted,trackedCreated,trackedDestroyed,trackedLive,trackedBytesEstimate,trackedPeakBytesEstimate,attachFailures,unknownSizes,graphLive,nrLive,uploadLive\n";
        events<<"utcMs,tickMs,event,owner,resourceId,group,allocationBytesEstimate,detail\n";
        windows<<"utcMs,tickMs,hwnd,foreground,visible,dpi,left,top,width,height,monitorWidth,monitorHeight,displayWidth,displayHeight,refreshHz,coversMonitor\n";
        live<<"utcMs,tickMs,resourceId,group,allocationBytesEstimate,ageMs,droppedReleaseEvents\n";
        events.flush();samples.flush();
        worker=std::thread([this]{run();});
    }
    ~State() {
        {std::lock_guard lock(mutex);quit=true;}
        cv.notify_all();if(worker.joinable())worker.join();
        if(enabled){std::lock_guard lock(ledger->mutex);flushRetired();}
    }
    void flushRetired(){ // caller holds ledger->mutex and sampler owns the stream
        for(const auto& r:ledger->retired)events<<r.utc<<','<<r.time<<",resource-final-release,0,"<<r.item.id<<','<<r.item.group<<','<<r.item.bytes<<",0\n";
        ledger->retired.clear();events.flush();
    }
    static BOOL CALLBACK window(HWND hwnd,LPARAM parameter){
        DWORD pid=0;GetWindowThreadProcessId(hwnd,&pid);
        if(pid!=GetCurrentProcessId()||!IsWindowVisible(hwnd))return TRUE;
        auto& s=*reinterpret_cast<State*>(parameter);RECT r{};if(!GetWindowRect(hwnd,&r))return TRUE;
        MONITORINFOEXW monitor{};monitor.cbSize=sizeof(monitor);
        if(!GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&monitor))return TRUE;
        DEVMODEW mode{};mode.dmSize=sizeof(mode);const auto available=EnumDisplaySettingsW(monitor.szDevice,ENUM_CURRENT_SETTINGS,&mode);
        const auto& m=monitor.rcMonitor;
        s.windows<<utcMs()<<','<<tick()<<','<<uint64_t(reinterpret_cast<uintptr_t>(hwnd))<<','<<(GetForegroundWindow()==hwnd)<<",1,"<<GetDpiForWindow(hwnd)<<','<<r.left<<','<<r.top<<','<<r.right-r.left<<','<<r.bottom-r.top<<','<<m.right-m.left<<','<<m.bottom-m.top<<',';
        if(available)s.windows<<mode.dmPelsWidth<<','<<mode.dmPelsHeight<<','<<mode.dmDisplayFrequency;
        else s.windows<<",,";
        s.windows<<','<<(r.left<=m.left&&r.top<=m.top&&r.right>=m.right&&r.bottom>=m.bottom)<<'\n';
        return TRUE;
    }
    void run() {
        std::unique_lock lock(mutex);
        unsigned cycle=0;
        while(!quit) {
            EnumWindows(window,reinterpret_cast<LPARAM>(this));windows.flush();
            for(auto& [owner,a]:adapters) {
                for(unsigned segment=0;segment<2;++segment) {
                    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
                    const HRESULT hr=a.adapter?a.adapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP(segment),&info):E_NOINTERFACE;
                    samples<<utcMs()<<','<<tick()<<','<<GetCurrentProcessId()<<','<<a.luid<<','<<a.generation<<','<<a.active<<','<<(segment?"nonlocal":"local")<<','<<uint32_t(hr)<<',';
                    if(SUCCEEDED(hr))samples<<info.CurrentUsage<<','<<info.Budget<<','<<info.CurrentReservation<<','<<info.AvailableForReservation;
                    else samples<<",,,"; // unavailable, never a fake zero
                    samples<<','<<(a.fence!=nullptr)<<',';
                    if(a.fence)samples<<a.fence->GetCompletedValue();
                    std::lock_guard counters(ledger->mutex);
                    samples<<','<<ledger->created<<','<<ledger->destroyed<<','<<ledger->live.size()<<','<<ledger->liveBytes<<','<<ledger->peakBytes<<','<<ledger->attachFailures<<','<<ledger->unknownSizes;
                    for(unsigned group=0;group<3;++group)samples<<','<<ledger->createdBy[group]-ledger->destroyedBy[group];
                    samples<<'\n';
                }
            }
            {std::lock_guard counters(ledger->mutex);flushRetired();
                if(cycle++%5==0){for(const auto& [id,item]:ledger->live)
                    live<<utcMs()<<','<<tick()<<','<<id<<','<<item.group<<','<<item.bytes<<','<<tick()-item.born<<','<<ledger->droppedReleaseEvents<<'\n';
                    live.flush();}}
            samples.flush();
            cv.wait_for(lock,std::chrono::seconds(2),[this]{return quit;});
        }
    }
    void event(const char* name,const void* owner,uint64_t id=0,unsigned group=0,uint64_t bytes=0,uint64_t detail=0) {
        if(!enabled)return;
        std::lock_guard lock(mutex);
        events<<utcMs()<<','<<tick()<<','<<name<<','<<uint64_t(reinterpret_cast<uintptr_t>(owner))<<','<<id<<','<<group<<','<<bytes<<','<<detail<<'\n';
        events.flush();
    }
};
inline State& state(){static State s;return s;}
inline void event(const char* name,const void* owner,uint64_t detail=0){state().event(name,owner,0,0,0,detail);}
inline void contextOpened(const void* owner,IDXGIAdapter1* adapter,ID3D12Fence* fence,uint64_t luid){
    auto& s=state();if(!s.enabled)return;
    {std::lock_guard lock(s.mutex);auto& a=s.adapters[owner];
        if(adapter)adapter->QueryInterface(IID_PPV_ARGS(&a.adapter));
        a.fence=fence;a.luid=luid;a.active=true;a.generation=++s.generation;}
    s.event("context-open",owner);s.cv.notify_all();
}
inline void contextClosing(const void* owner){
    auto& s=state();if(!s.enabled)return;
    s.event("context-close",owner);
    std::lock_guard lock(s.mutex);const auto it=s.adapters.find(owner);
    if(it!=s.adapters.end()){it->second.fence=nullptr;it->second.active=false;}
}
// A resource owns its token; the token owns no COM object. Final Release of
// the resource releases the token, exposing SDK-retained application textures.
inline constexpr GUID tokenGuid{0x868b6eea,0x2e39,0x41da,{0xb1,0x83,0xca,0x32,0xc3,0xb5,0xd2,0x60}};
class Token final:public IUnknown {
    std::atomic<ULONG> refs{1};
    std::shared_ptr<Ledger> ledger;
    Item item;
public:
    bool registered=false;
    Token(std::shared_ptr<Ledger> l,Item i):ledger(std::move(l)),item(i){}
    ~Token(){if(registered){std::lock_guard lock(ledger->mutex);ledger->live.erase(item.id);
        ++ledger->destroyed;++ledger->destroyedBy[item.group];ledger->liveBytes-=item.bytes;ledger->bytesBy[item.group]-=item.bytes;
        if(ledger->retired.size()<4096)ledger->retired.push_back({item,utcMs(),tick()});else ++ledger->droppedReleaseEvents;}}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out)override{
        if(!out)return E_POINTER;*out=nullptr;if(iid!=IID_IUnknown)return E_NOINTERFACE;*out=this;AddRef();return S_OK;}
    ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}
    ULONG STDMETHODCALLTYPE Release()override{auto n=--refs;if(!n)delete this;return n;}
};
inline void track(ID3D12Device* device,ID3D12Resource* resource,unsigned group){
    auto& s=state();if(!s.enabled||!device||!resource)return;
    // Do not replace an existing token or double-count aliased resources.
    IUnknown* existing=nullptr;UINT size=sizeof(existing);
    if(SUCCEEDED(resource->GetPrivateData(tokenGuid,&size,&existing))&&existing){existing->Release();return;}
    const auto desc=resource->GetDesc();const auto allocation=device->GetResourceAllocationInfo(0,1,&desc);
    const bool known=allocation.SizeInBytes!=UINT64_MAX;
    auto ledger=s.ledger;Item item{};
    {std::lock_guard lock(ledger->mutex);item={++ledger->next,known?allocation.SizeInBytes:0,tick(),group};}
    auto* token=new Token(ledger,item);
    const auto hr=resource->SetPrivateDataInterface(tokenGuid,token);
    {std::lock_guard lock(ledger->mutex);
        if(SUCCEEDED(hr)){token->registered=true;ledger->live.emplace(item.id,item);++ledger->created;++ledger->createdBy[group];
            ledger->liveBytes+=item.bytes;ledger->bytesBy[group]+=item.bytes;ledger->peakBytes=std::max(ledger->peakBytes,ledger->liveBytes);ledger->unknownSizes+=!known;}
        else ++ledger->attachFailures;}
    token->Release();s.event(SUCCEEDED(hr)?"resource-create":"resource-track-failed",resource,item.id,group,item.bytes,uint32_t(hr));
}
inline auto graphTexture(ID3D12Device* d,uint32_t w,uint32_t h,DXGI_FORMAT f,bool u){auto r=veyra::pipeline::makeTexture(d,w,h,f,u);track(d,r.Get(),0);return r;}
inline auto nrTexture(ID3D12Device* d,uint32_t w,uint32_t h,DXGI_FORMAT f,bool u){auto r=veyra::pipeline::makeTexture(d,w,h,f,u);track(d,r.Get(),1);return r;}
inline auto graphUpload(ID3D12Device* d,uint64_t bytes){auto r=veyra::pipeline::makeUploadBuffer(d,bytes);track(d,r.Get(),2);return r;}
}
