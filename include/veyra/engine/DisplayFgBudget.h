// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "veyra/engine/EnhancementSettings.h"
#include <cmath>
#include <cstdint>

namespace veyra::engine {
// Live frame-generation output budget (claude/latency-min-20261013 B2).
//
// A panel shows at most one new picture per refresh. Generated pictures above
// that rate are queued (VSync: latency) or replaced before scanout (Automatic:
// wasted GPU), and every extra multiplier step makes the real frame wait
// longer: graph FG shows it (M-1)/M of a source interval after it arrived.
// For live sources (files have no input latency to protect):
//   1. the output fits the refresh (content x M <= 1.02 x refresh): unchanged;
//   2. otherwise the largest supported multiplier whose output fits;
//   3. if none fits but the source is slower than the panel (60 fps on a
//      100 Hz panel), 2X: Automatic replaces the surplus at each refresh and
//      still shows more distinct pictures per refresh. Under VSync (queued)
//      the surplus would wait in the display queue instead, so bypass;
//   4. if the source alone already fills the panel (>= 0.97 x refresh, e.g.
//      60 fps on 60 Hz), bypass generation: no generated picture can add a
//      refresh, and real frames present as soon as they are enhanced instead
//      of waiting half an interval behind a generated one.
// XeSS changes its interpolation count only through a model switch Intel
// documents as expensive (shader compilation), so XeSS keeps the requested
// multiplier and only bypasses (xefgSwapChainSetEnabled): under rule 4, and
// under VSync whenever its output does not fit.
inline constexpr double kDisplayFgFitTolerance=1.02;
inline constexpr double kDisplayFgFillRatio=0.97;

constexpr bool displayFgMultiplierSupported(FrameGenerationBackend backend,unsigned multiplier){
    switch(backend){
    case FrameGenerationBackend::Dlss: return multiplier==2||multiplier==3||multiplier==4||multiplier==6;
    case FrameGenerationBackend::Vfg: return multiplier>=2&&multiplier<=8;
    case FrameGenerationBackend::Fsr:
    case FrameGenerationBackend::Fsr4: return multiplier==2;
    case FrameGenerationBackend::XeSS: return false; // never reduced, see above
    }
    return false;
}

// 0 = bypass generation, otherwise a multiplier in [2, requested].
// queued: presents above the refresh wait in the display queue (VSync)
// instead of being replaced (Automatic). Unknown rates keep the request.
inline unsigned displayFgMultiplier(FrameGenerationBackend backend,unsigned requested,double contentFps,double refreshHz,bool queued=false){
    if(requested<2||!std::isfinite(contentFps)||!std::isfinite(refreshHz)||contentFps<=0||refreshHz<=0)return requested;
    if(contentFps>=refreshHz*kDisplayFgFillRatio)return 0;
    if(contentFps*requested<=refreshHz*kDisplayFgFitTolerance)return requested;
    if(backend==FrameGenerationBackend::XeSS)return queued?0:requested;
    for(unsigned m=requested-1;m>=2;--m)
        if(displayFgMultiplierSupported(backend,m)&&contentFps*m<=refreshHz*kDisplayFgFitTolerance)return m;
    return queued?0:2;
}

// A changed decision applies only after it held for the whole hold time, so a
// content rate wobbling around a boundary (30/40 fps games, the dedup
// estimate) or a brief refresh change never toggles generation; a candidate
// that changes again restarts the hold. The first decision after reset()
// applies at once. Units: 100 ns.
class DisplayFgBudgetGate {
public:
    explicit DisplayFgBudgetGate(int64_t hold100ns=15000000):hold_(hold100ns){}
    void reset(){valid_=pendingValid_=false;}
    // Returns true when the applied decision changed.
    bool update(unsigned decision,int64_t now){
        if(!valid_){applied_=decision;valid_=true;pendingValid_=false;return true;}
        if(decision==applied_){pendingValid_=false;return false;}
        if(!pendingValid_||pending_!=decision){pending_=decision;since_=now;pendingValid_=true;return false;}
        if(now-since_<hold_)return false;
        applied_=decision;pendingValid_=false;return true;
    }
    bool valid()const{return valid_;}
    unsigned applied()const{return applied_;}
private:
    int64_t hold_,since_=0;
    unsigned applied_=0,pending_=0;
    bool valid_=false,pendingValid_=false;
};
}
