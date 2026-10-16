#include "veyra/engine/EffectChain.h"
#include "veyra/engine/GraphDescription.h"
#include "veyra/engine/PresetLibrary.h"
#include "veyra/engine/PresetStore.h"
#include "veyra/engine/ExportFeaturePolicy.h"
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace veyra;
using namespace veyra::engine;
namespace {
int failures=0;
void check(bool ok,const char* text){std::cout<<(ok?"PASS ":"FAIL ")<<text<<std::endl;failures+=!ok;}
std::string bytes(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
}
int wmain(int argc,wchar_t** argv){
    if(argc!=2)return 2;
    const std::filesystem::path out=argv[1];std::filesystem::create_directories(out);
    EnhancementSettings s;s.nr=true;
    check(s.nrRuntime==NrRuntime::Ampere&&s.model.performance&&nrRenderPreset(s.nrRuntime,s.model)==2,"fresh NVIDIA settings select SF preset B");
    auto a=s;a.model.performance=false;
    check(nrRenderPreset(a.nrRuntime,a.model)==1&&requiresGraphRebuild(s,a),"active SF model change rebuilds NR");
    for(auto runtime:{NrRuntime::NvidiaOriginal,NrRuntime::LmxxfAmd}){
        auto original=s;original.nrRuntime=runtime;auto edited=original;edited.model.performance=false;
        check(nrRenderPreset(runtime,edited.model)==0&&!requiresGraphRebuild(original,edited)&&
            original.sameActiveVideoParameters(edited),"original and AMD ignore the inactive performance option");
    }
    auto off=s;off.nr=false;auto draft=off;draft.model.performance=false;
    check(!requiresGraphRebuild(off,draft)&&off.sameActiveVideoParameters(draft),"disabled NR model edit does not disturb playback");
    s.nrLayerCount=2;for(auto& n:s.nrLayers){n.enabled=true;n.runtime=s.nrRuntime;}
    a=s;a.nrLayers[1].model.performance=false;
    check(requiresGraphRebuild(s,a),"second active NR layer model change rebuilds");
    s.nrLayers[1].enabled=a.nrLayers[1].enabled=false;
    check(!requiresGraphRebuild(s,a)&&s.sameActiveVideoParameters(a),"bypassed layer model is saved without a rebuild");
    s.nrLayers[1].enabled=a.nrLayers[1].enabled=true;
    const auto frozen=freezeExportSettings(a);EnhancementSettings round=frozen;fromChain(toChain(frozen),round);
    check(round==frozen&&!round.nrLayers[1].model.performance,"export snapshot retains each layer's model");
    StageRequest request;request.width=1920;request.height=1080;request.nr=true;request.exportJob=true;
    pipeline::EnhanceGraphDesc desc;describeStages(request,frozen,desc);
    check(desc.nrLayersModel.size()==2&&desc.nrLayersModel[0].performance&&!desc.nrLayersModel[1].performance,"export graph receives both model choices");
    const auto storePath=out/L"legacy-store.v1";PresetStore legacy(storePath);
    check(legacy.load()&&legacy.put(L"mixed",a)&&legacy.setDefault(0),"legacy preset store accepts mixed models");
    PresetStore legacyRead(storePath);
    check(bytes(storePath).starts_with("VEYRA_PRESETS 32")&&legacyRead.load()&&legacyRead.defaultSettings()==a,"legacy schema 32 round-trips all layers");
    for(auto old:{NrRuntime::Original,NrRuntime::Community,NrRuntime::Ampere,NrRuntime::NvidiaOriginal,NrRuntime::LmxxfAmd}){
        EnhancementSettings oldSettings;oldSettings.nr=true;oldSettings.nrRuntime=old;
        PresetEntry entry;entry.name=L"legacy runtime";entry.chain=toChain(oldSettings);
        std::vector<PresetEntry> decoded;std::wstring def,error;
        check(PresetLibrary::parse(PresetLibrary::encodeEntries({entry},{}),decoded,def,error)&&decoded.size()==1&&
            decoded[0].chain.firstOf(EffectType::NrEnhance)->nr.runtime==currentNrRuntime(old)&&
            decoded[0].chain.firstOf(EffectType::NrEnhance)->nr.model.performance,"old runtime selection migrates without losing original or AMD");
    }
    const auto libraryPath=out/L"library.v1";PresetLibrary library(libraryPath);library.setIncludeBuiltins(false);
    PresetEntry entry;entry.name=L"NR models";entry.chain=toChain(s);
    check(library.load()&&library.put(entry),"save default B library");const auto rollback=bytes(libraryPath);
    entry.chain=toChain(a);
    check(library.put(entry,true)&&bytes(libraryPath)==rollback,"old-model opt-in preserves the earlier library file");
    PresetLibrary reloaded(libraryPath);reloaded.setIncludeBuiltins(false);
    check(reloaded.load()&&reloaded.entries().size()==1&&reloaded.entries()[0]==entry,"library schema 13 restores mixed A/B models");
    entry.chain=toChain(s);check(library.put(entry,true),"return to B in the selected sidecar");
    PresetLibrary again(libraryPath);again.setIncludeBuiltins(false);
    check(again.load()&&again.entries()[0]==entry,"return to B cannot resurrect a stale A choice");
    const auto sessionPath=out/L"session.v1";auto session=std::make_unique<ChainSession>(ChainSession::initial(s));
    check(session->select(ChainMode::Node),"initialize separate list and node configurations");ChainSessionStore sessions(sessionPath);
    check(sessions.save(*session),"save B session");const auto sessionRollback=bytes(sessionPath);
    auto& node=session->configurations[1];
    auto document=std::make_shared<NodeEditorDocument>();document->nodes=node.chain;
    check(document->layout.initialize(document->nodes).accepted,"initialize node editor layout");
    document->nodes.firstOf(EffectType::NrEnhance)->nr.model.performance=false;
    node.editor=document;
    check(sessions.save(*session)&&bytes(sessionPath)==sessionRollback,"detached editor model opts into session 8 and preserves old file");
    auto restored=std::make_unique<ChainSession>();ChainSessionStore reader(sessionPath);
    check(reader.load(*restored)&&*restored==*session,"session restores list, node and editor model choices independently");
    std::cout<<"NR model settings failures="<<failures<<std::endl;return failures?1:0;
}
