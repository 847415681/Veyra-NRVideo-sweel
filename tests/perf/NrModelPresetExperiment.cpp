#include "NrExperimentalFixture.h"
#include "veyra/Log.h"
#include <d3d12sdklayers.h>
#include <algorithm>

// Test-only runtime contract probe. Readbacks/waits never enter playback.
// Compare explicit hints with the same input/resources after warmup; do not
// infer enum meaning from a preset name or from an unprocessed black image.
int wmain(int argc,wchar_t** argv) {
    using namespace nrperf;
    if(argc!=3)return 2;
    const std::filesystem::path out=argv[2];std::filesystem::create_directories(out);
    Logger::instance().openFile((out/L"engine.log").wstring());
    Session s;if(!s.init(argv[1]))return 2;
    bool pass=true;std::ofstream csv(out/L"presets.csv");
    csv<<"run,preset,medianEvaluateMs,changedPixels,nonBlackPixels\n";
    const int32_t presets[]={0,1,2,2,1};
    for(unsigned run=0;run<std::size(presets);++run){
        Feature f(s);if(!f.create(s.ring,1920,1080,presets[run])||
            !f.textures(s.ring,1920,1080,1920,1080))return 2;
        std::vector<double> times;
        for(unsigned frame=0;frame<30;++frame){
            if(!f.evaluate(s.ring,1920,1080,frame==0))return 2;
            if(frame>=10)times.push_back(f.lastGpuEvaluateMs);
        }
        auto pixels=f.read(s.ring);if(pixels.size()!=size_t(1920)*1080*4)return 2;
        uint64_t changed=0,nonBlack=0;
        for(size_t i=0;i<pixels.size();i+=4){
            changed+=pixels[i]!=17||pixels[i+1]!=23||pixels[i+2]!=29;
            nonBlack+=pixels[i]+pixels[i+1]+pixels[i+2]>15;
        }
        std::sort(times.begin(),times.end());const double median=times[times.size()/2];
        csv<<run<<','<<presets[run]<<','<<median<<','<<changed<<','<<nonBlack<<'\n';csv.flush();
        std::ofstream raw(out/(L"preset-"+std::to_wstring(run)+L".rgba"),std::ios::binary);
        raw.write(reinterpret_cast<const char*>(pixels.data()),std::streamsize(pixels.size()));
        std::printf("PRESET run=%u hint=%d medianGpuMs=%.6f changed=%llu nonBlack=%llu\n",
            run,presets[run],median,changed,nonBlack);
        pass=pass&&median>0&&changed>size_t(1920)*1080/2&&nonBlack>size_t(1920)*1080/2;
    }
    s.ring.drainQueue();Microsoft::WRL::ComPtr<ID3D12InfoQueue> debug;unsigned errors=0;
    if(!hrOK(s.device.device()->QueryInterface(IID_PPV_ARGS(&debug)),"Debug.Query"))return 2;
    for(UINT64 i=0;i<debug->GetNumStoredMessages();++i){SIZE_T n=0;debug->GetMessage(i,nullptr,&n);
        std::vector<uint8_t> data(n);auto* m=reinterpret_cast<D3D12_MESSAGE*>(data.data());
        if(!hrOK(debug->GetMessage(i,m,&n),"Debug.Read"))return 2;
        if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++errors;std::printf("DEBUG_ERROR %s\n",m->pDescription);}}
    const auto removed=s.device.device()->GetDeviceRemovedReason();
    std::printf("RESULT pass=%d debugErrors=%u deviceRemoved=0x%08X\n",pass,errors,unsigned(removed));
    return pass&&errors==0&&SUCCEEDED(removed)?0:1;
}
