#include "veyra/engine/PresetStore.h"
#include "veyra/engine/PresetLibrary.h"
#include "veyra/engine/VfgQueueBudget.h"
#include "veyra/engine/VfgOverloadEstimate.h"
#include "veyra/pipeline/FrameBatch.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <limits>
using namespace veyra::engine;
namespace {unsigned checks=0,failures=0;void check(bool ok,const char* why){++checks;if(!ok){++failures;std::cout<<"FAIL "<<why<<'\n';}}
std::string bytes(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary);return {(std::istreambuf_iterator<char>(f)),{}};}}
int wmain(int argc,wchar_t** argv){
    if(argc!=2||!std::filesystem::path(argv[1]).is_absolute())return 2;
    const std::filesystem::path root=argv[1];if(!std::filesystem::create_directories(root))return 2;
    check(vfgQueuedGroupCost(38.,7.,12.,true,1,false)==19.,"queued reduced group charges one interpolation instead of seven");
    check(vfgQueuedGroupCost(38.,7.,12.,false,7,false)==38.,"full group keeps its measured full cost");
    check(vfgQueuedGroupCost(38.,7.,12.,true,1,true)==38.,"history recovery retains conservative full cost");
    check(vfgQueuedGroupCost(38.,7.,{},true,1,false)==38.&&vfgQueuedGroupCost(38.,{},12.,true,1,false)==38.,"missing measurements preserve full estimate");
    check(!vfgQueuedGroupCost({},7.,12.,true,1,false),"unknown full cost stays unknown");
    check(vfgQueuedGroupCost(38.,7.,12.,true,2,false)==38.,"unexpected multi-evaluation reduced group is not undercharged");
    check(vfgQueuedGroupCost(38.,7.,std::numeric_limits<double>::quiet_NaN(),true,1,false)==38.&&vfgQueuedGroupCost(38.,7.,-1.,true,1,false)==38.,"invalid first timing cannot lower queue budget");
    check(vfgQueuedGroupCost(15.,7.,12.,true,1,false)==15.,"reduced estimate never exceeds existing full cost");
    VfgOverloadEstimate overload;
    check(!overload.severeTotal(7.,20.,33.),"unknown full group is not invented");
    check(!overload.allowProbe(300000000),"unknown work does not invent a probe");
    overload.observe(114.,20.,0);
    check(overload.severeTotal(7.,20.,33.)==121.,"expired severe cost remains an estimate instead of free work");
    check(!overload.severeTotal(7.,15.,33.),"a meaningfully faster first interpolation reopens full probing");
    check(overload.severeTotal(7.,25.,33.)==149.5,"slower current work cannot lower retained full estimate");
    check(!overload.severeTotal({},20.,33.)&&!overload.severeTotal(7.,{},33.)&&!overload.severeTotal(7.,20.,0.),"missing measurements or cadence preserve legacy admission");
    overload.observe(0.,0.,0);check(overload.severeTotal(7.,20.,33.)==121.,"invalid observation does not erase known severe work");
    check(!overload.allowProbe(299999999)&&overload.allowProbe(300000000),"unchanged first cost still permits eventual full recovery probing");
    check(!overload.allowProbe(300000000)&&!overload.allowProbe(599999999),"pending probes do not form a burst of full groups");
    overload.observe(114.,20.,400000000);check(!overload.allowProbe(699999999)&&overload.allowProbe(700000000),"a fresh full measurement restarts the low-frequency probe interval");
    overload.reset();check(!overload.severeTotal(7.,20.,33.),"source and revision reset discard retained relationship");
    overload.observe(38.,20.,0);check(!overload.severeTotal(7.,20.,33.),"mildly overloaded 3X groups retain existing recovery");
    check(VfgOverloadEstimate::fits(100.,7.,96.,4.,0.,0.,0.)&&!VfgOverloadEstimate::fits(100.,7.,95.9,4.,0.,0.,0.),"retained admission uses the original deadline and grace boundary");
    check(!VfgOverloadEstimate::fits(121.,7.,113.,0.,1000.,0.,0.),"elapsed CPU time cannot erase unsubmitted interpolation cost");
    check(!motionUsesFlow(MotionSource::OpticalFlow,FrameGenerationBackend::Vfg)&&
          !motionUsesFlow(MotionSource::Automatic,FrameGenerationBackend::Vfg),"VFG uses its own motion even after a DLSS flow preset");
    check(motionUsesFlow(MotionSource::OpticalFlow)&&motionUsesFlow(MotionSource::Automatic),"SR/NR external motion remains enabled");
    EnhancementSettings legacy;PresetStore old(root/L"legacy.txt");check(old.load()&&old.put(L"old",legacy),"write legacy default");
    PresetStore legacyReader(root/L"legacy.txt");check(legacyReader.load()&&legacyReader.entries()[0].settings.vfgQuality==1,"old presets default to Medium");
    const auto original=bytes(root/L"legacy.txt");
    PresetStore store(root/L"settings.txt");check(store.load(),"new store");
    // Mixed tiers: the middle subframe runs the first model, the others the second.
    check(!vfgMixedQuality(0)&&!vfgMixedQuality(2)&&vfgMixedQuality(3)&&vfgMixedQuality(5)&&!vfgMixedQuality(6),"mixed VFG tiers are 3-5");
    check(vfgPrimaryMode(3)==2&&vfgSecondaryMode(3)==1&&vfgPrimaryMode(4)==2&&vfgSecondaryMode(4)==0&&vfgPrimaryMode(5)==1&&vfgSecondaryMode(5)==0,"High+Medium, High+Low, Medium+Low");
    for(unsigned q=0;q<3;++q)check(vfgPrimaryMode(q)==q&&vfgSecondaryMode(q)==q,"single-model tiers keep their mode");
    check(vfgPrimarySubframe(2)==1&&vfgPrimarySubframe(3)==2&&vfgPrimarySubframe(4)==2&&vfgPrimarySubframe(5)==3&&vfgPrimarySubframe(6)==3&&vfgPrimarySubframe(7)==4&&vfgPrimarySubframe(8)==4,"first model on the middle subframe");
    for(unsigned quality=0;quality<kVfgQualityCount;++quality)for(unsigned m=2;m<=8;++m){
        EnhancementSettings s;s.frameGenerationBackend=FrameGenerationBackend::Vfg;s.multiplier=m;s.vfgQuality=quality;
        check(s.validate().empty(),"VFG legal multiplier/quality");
        const auto name=std::to_wstring(m)+L"x-q"+std::to_wstring(quality);
        check(store.put(name,s),"legacy settings store writes VFG");
        const auto path=root/(name+L".preset");PresetLibrary library(path);library.setIncludeBuiltins(false);
        auto e=std::make_unique<PresetEntry>();e->name=name;e->chain=toChain(s);e->fg={m,FrameGenerationBackend::Vfg,quality};e->globals=ChainGlobalSettings::capture(s);
        check(library.load()&&library.put(*e),"library saves VFG");
        PresetLibrary reloaded(path);reloaded.setIncludeBuiltins(false);
        check(reloaded.load()&&reloaded.entries().size()==1&&reloaded.entries()[0]==*e,"library restores full VFG state");
        auto applied=legacy;PresetLibrary::apply(*e,applied);
        check(applied.multiplier==m&&applied.frameGenerationBackend==FrameGenerationBackend::Vfg&&applied.vfgQuality==quality,"preset applies multiplier backend quality");
        e->contents=presetContentMask(PresetContent::Color);applied=s;applied.vfgQuality=(quality+1)%kVfgQualityCount;
        const auto kept=applied.vfgQuality;PresetLibrary::apply(*e,applied);
        check(applied.frameGenerationBackend==FrameGenerationBackend::Vfg&&applied.multiplier==m&&applied.vfgQuality==kept,"partial color preset preserves VFG");
        auto session=std::make_unique<ChainSession>(ChainSession::initial(s));check(session->select(ChainMode::Node),"VFG node mode initializes");
        auto editor=std::make_shared<NodeEditorDocument>();editor->nodes=session->configurations[1].chain;
        editor->globals=ChainGlobalSettings::capture(s);check(editor->layout.initialize(editor->nodes).accepted,"VFG node draft layout");
        session->configurations[1].editor=editor;check(session->valid(),"VFG session valid");
        auto restored=std::make_unique<ChainSession>();
        const auto sessionPath=root/(name+L".session");ChainSessionStore sessionWriter(sessionPath);
        check(sessionWriter.save(*session),"session atomic save");ChainSessionStore sessionReader(sessionPath);
        check(sessionReader.load(*restored)&&*session==*restored,"session restart finds VFG suffix");
        auto bad=s;bad.multiplier=9;check(!bad.validate().empty(),"reject VFG 9X");bad=s;bad.vfgQuality=kVfgQualityCount;check(!bad.validate().empty(),"reject unknown quality");
        bad=s;bad.frameGenerationBackend=FrameGenerationBackend::Dlss;if(m>6)check(!bad.validate().empty(),"DLSS ceiling remains 6X");
        veyra::pipeline::FrameBatch batch;batch.identity={1,2,3};batch.a100ns=10000000;batch.b100ns=10333333;
        for(unsigned j=1;j<m;++j){veyra::pipeline::BatchFrame f;f.identity=batch.identity;f.subframe=j;f.pts100ns=batch.interpolate(batch.a100ns,batch.b100ns,j,m);f.kind=veyra::pipeline::FrameKind::Generated;batch.append(f);}
        veyra::pipeline::BatchFrame real;real.identity=batch.identity;real.pts100ns=batch.b100ns;batch.append(real);
        check(batch.count==m&&batch.frames[m-1].pts100ns==batch.b100ns,"2..8 ordered PTS with every source frame");
        if(m==8){bool rejected=false;try{real.pts100ns++;batch.append(real);}catch(const std::invalid_argument&){rejected=true;}check(rejected&&batch.count==8,"8X batch rejects ninth frame");}
    }
    PresetStore restoredStore(root/L"settings.txt");check(restoredStore.load()&&restoredStore.entries().size()==7*kVfgQualityCount,"all VFG settings restored");
    for(const auto& e:restoredStore.entries())check(e.settings.frameGenerationBackend==FrameGenerationBackend::Vfg&&e.settings.vfgQuality<kVfgQualityCount&&e.settings.multiplier>=2&&e.settings.multiplier<=8,"stored VFG values legal");
    check(bytes(root/L"legacy.txt")==original,"legacy file unchanged");
    std::cout<<"VFG settings "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
