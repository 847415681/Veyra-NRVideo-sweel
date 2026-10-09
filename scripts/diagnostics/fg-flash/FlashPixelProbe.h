// Diagnostic-only: sample the actual buffer in the same presentation list.
// Never included in the product. Nonblack deterministic inputs are the oracle.
#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <vector>
#include <fstream>
#include <iostream>
#include <cstring>
#include <filesystem>
#include <chrono>

namespace flashProbe {
inline constexpr unsigned capacity=20000, points=9, stride=512;
inline Microsoft::WRL::ComPtr<ID3D12Resource> readback;
struct Entry {bool generated;uint64_t source;uint32_t slot;DXGI_FORMAT format;uint64_t epoch,revision;int64_t pts,unix100ns;const char* stage;};
inline std::vector<Entry> entries;
inline bool overflow=false,failed=false;
inline void record(ID3D12Device* device,ID3D12GraphicsCommandList* list,ID3D12Resource* buffer,bool generated,uint64_t source,unsigned slot,uint64_t epoch=0,uint64_t revision=0,int64_t pts=-1,const char* stage="present"){
    if(entries.size()==capacity){overflow=true;return;}
    if(!readback){
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width=uint64_t(capacity)*points*stride;rd.Height=1;rd.DepthOrArraySize=1;rd.MipLevels=1;rd.SampleDesc.Count=1;rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if(FAILED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)))){failed=true;return;}
    }
    const auto d=buffer->GetDesc();
    if(d.Format!=DXGI_FORMAT_R10G10B10A2_UNORM&&d.Format!=DXGI_FORMAT_R8G8B8A8_UNORM){failed=true;return;}
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=buffer;b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore=D3D12_RESOURCE_STATE_COMMON;b.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;list->ResourceBarrier(1,&b);
    D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=buffer;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    for(unsigned y=0;y<3;++y)for(unsigned x=0;x<3;++x){
        const unsigned xx=unsigned(d.Width)*(x+1)/4,yy=d.Height*(y+1)/4;
        D3D12_BOX box{xx,yy,0,xx+1,yy+1,1};
        D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset=(uint64_t(entries.size())*points+y*3+x)*stride;
        dst.PlacedFootprint.Footprint={d.Format,1,1,1,256};list->CopyTextureRegion(&dst,0,0,0,&src,&box);
    }
    std::swap(b.Transition.StateBefore,b.Transition.StateAfter);list->ResourceBarrier(1,&b);
    FILETIME ft{};GetSystemTimePreciseAsFileTime(&ft);ULARGE_INTEGER raw{};raw.LowPart=ft.dwLowDateTime;raw.HighPart=ft.dwHighDateTime;
    entries.push_back({generated,source,slot,d.Format,epoch,revision,pts,int64_t(raw.QuadPart-116444736000000000ull),stage});
}
inline bool dump(const wchar_t* file){
    if(!readback||entries.empty()||failed)return false;
    void* ptr=nullptr;D3D12_RANGE range{0,entries.size()*points*stride};
    if(FAILED(readback->Map(0,&range,&ptr)))return false;
    std::ofstream out{std::filesystem::path(file)};out<<"ordinal,source,generated,slot,format,minCode,maxCode,meanCode,blackPoints,epoch,revision,pts100ns,unix100ns,stage\n";
    unsigned blackReal=0,blackGenerated=0,reals=0,generated=0;
    for(size_t i=0;i<entries.size();++i){
        const auto e=entries[i];unsigned lo=1023,hi=0,black=0;double mean=0;
        for(unsigned j=0;j<points;++j){
            const auto* p=static_cast<uint8_t*>(ptr)+(i*points+j)*stride;std::array<unsigned,3> rgb{};
            if(e.format==DXGI_FORMAT_R10G10B10A2_UNORM){uint32_t v=0;std::memcpy(&v,p,4);rgb={v&1023,(v>>10)&1023,(v>>20)&1023};}
            else rgb={unsigned(p[0])*1023/255,unsigned(p[1])*1023/255,unsigned(p[2])*1023/255};
            unsigned localMax=0;for(auto v:rgb){lo=std::min(lo,v);hi=std::max(hi,v);localMax=std::max(localMax,v);mean+=v;}
            black+=localMax<=5;
        }
        if(e.generated){++generated;blackGenerated+=black==points;}else{++reals;blackReal+=black==points;}
        out<<i<<','<<e.source<<','<<e.generated<<','<<e.slot<<','<<unsigned(e.format)<<','<<lo<<','<<hi<<','<<mean/(points*3)<<','<<black<<','<<e.epoch<<','<<e.revision<<','<<e.pts<<','<<e.unix100ns<<','<<e.stage<<'\n';
    }
    D3D12_RANGE noWrite{0,0};readback->Unmap(0,&noWrite);
    std::cout<<"PIXEL_PROBE real="<<reals<<" generated="<<generated<<" blackReal="<<blackReal<<" blackGenerated="<<blackGenerated<<" overflow="<<overflow<<std::endl;
    readback.Reset();entries.clear();const bool ok=out.good()&&blackReal==0&&blackGenerated==0&&!overflow;overflow=false;return ok;
}
}
