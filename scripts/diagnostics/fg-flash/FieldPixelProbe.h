// Diagnostic executable only. Product libraries, settings and timing remain unchanged.
// Samples 9 pixels from each input/output on the GPU, then reads them once at close.
#pragma once
#include "FlashPixelProbe.h"
#include "veyra/Log.h"
#include "veyra/RuntimePaths.h"
#include <format>
#include <stdexcept>

namespace flashField {
struct Event {size_t sourceOrdinal,bufferOrdinal;uint64_t frame,epoch,revision,fence;int64_t pts,recorded,presentEnd;unsigned backBuffer;int result=-1;};
inline std::vector<Event> events;
inline Microsoft::WRL::ComPtr<ID3D12Fence> lastFence;
inline uint64_t lastValue=0;
inline size_t submittedEntries=0;
inline int64_t wall100ns(){FILETIME f{};GetSystemTimePreciseAsFileTime(&f);ULARGE_INTEGER n{};n.LowPart=f.dwLowDateTime;n.HighPart=f.dwHighDateTime;return int64_t(n.QuadPart-116444736000000000ull);}
inline void record(ID3D12Device* device,ID3D12GraphicsCommandList* list,ID3D12Resource* source,ID3D12Resource* buffer,bool generated,uint64_t frame,unsigned slot,uint64_t epoch,uint64_t revision,int64_t pts,unsigned backBuffer){
    if(flashProbe::entries.size()+2>flashProbe::capacity){flashProbe::overflow=true;return;}
    const size_t begin=flashProbe::entries.size();
    flashProbe::record(device,list,source,generated,frame,slot,epoch,revision,pts,"present-input");
    flashProbe::record(device,list,buffer,generated,frame,slot,epoch,revision,pts,"present");
    if(flashProbe::entries.size()==begin+2)events.push_back({begin,begin+1,frame,epoch,revision,0,pts,wall100ns(),0,backBuffer});
}
inline void submitted(ID3D12Fence* fence,uint64_t value){
    if(flashProbe::entries.size()==submittedEntries)return;
    submittedEntries=flashProbe::entries.size();lastFence=fence;lastValue=value;
    if(!events.empty())events.back().fence=value;
}
inline void presented(bool ok){
    if(!events.empty()&&events.back().presentEnd==0){events.back().presentEnd=wall100ns();events.back().result=ok?1:0;}
}
inline void finish(){
    if(flashProbe::entries.empty())return;
    // Close already drains the private presenter queue. Shared-queue paths use
    // their submitted fence; wait only here, never in the per-frame loop.
    if(lastFence&&lastFence->GetCompletedValue()<lastValue){
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        const HRESULT hr=event?lastFence->SetEventOnCompletion(lastValue,event):E_FAIL;
        const DWORD waited=SUCCEEDED(hr)?WaitForSingleObject(event,10000):WAIT_FAILED;
        if(event)CloseHandle(event);
        if(waited!=WAIT_OBJECT_0){veyra::log::error("flash-probe",std::format("teardown sample fence not complete hr=0x{:X} wait={}; not mapping",unsigned(hr),waited));return;}
    }
    // If recording was followed by a failed submit, exclude the unsubmitted tail.
    flashProbe::entries.resize(submittedEntries);
    try{
        wchar_t overridePath[32768]{};
        const DWORD length=GetEnvironmentVariableW(L"VEYRA_FLASH_PROBE_DIR",overridePath,32768);
        const auto dir=length&&length<32768?std::filesystem::path(overridePath):veyra::runtime::logsDirectory();
        std::filesystem::create_directories(dir);
        const auto stem=std::format(L"flash-pixels-{}-{}",GetCurrentProcessId(),wall100ns());
        const auto csv=dir/(stem+L".csv");
        const auto count=flashProbe::entries.size();const bool capped=flashProbe::overflow;
        const bool nonblack=flashProbe::dump(csv.c_str());
        std::ofstream out(dir/(stem+L"-events.csv"));
        out<<"sourceOrdinal,bufferOrdinal,frame,epoch,revision,pts100ns,recordUnix100ns,presentEndUnix100ns,backBuffer,submitFence,presentOk\n";
        for(const auto& e:events)if(e.bufferOrdinal<count)out<<e.sourceOrdinal<<','<<e.bufferOrdinal<<','<<e.frame<<','<<e.epoch<<','<<e.revision<<','<<e.pts<<','<<e.recorded<<','<<e.presentEnd<<','<<e.backBuffer<<','<<e.fence<<','<<e.result<<'\n';
        const bool saved=std::filesystem::exists(csv)&&std::filesystem::file_size(csv)>0;
        const auto utf8=csv.u8string();const std::string displayPath(utf8.begin(),utf8.end());
        if(saved)veyra::log::info("flash-probe",std::format("saved={} samples={} capped={} nonblack={} (diagnostic samples; normal black scenes are valid; XeSS-internal frames are not sampled)",displayPath,count,capped,nonblack));
        else veyra::log::error("flash-probe",std::format("no pixel CSV written; samples={} samplerFailed={} (supported RGBA8/RGB10 only)",count,flashProbe::failed));
    }catch(const std::exception& e){veyra::log::error("flash-probe",e.what());}
    events.clear();lastFence.Reset();lastValue=0;submittedEntries=0;
    flashProbe::readback.Reset();flashProbe::entries.clear();flashProbe::failed=false;flashProbe::overflow=false;
}
}
