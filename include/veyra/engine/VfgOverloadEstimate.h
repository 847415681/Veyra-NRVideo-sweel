// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace veyra::engine {
// Retained estimates are NOT fresh GPU samples. A severely overloaded full
// group must not become free merely because only affordable 2X groups ran
// during the measurement window. A faster measured first interpolation opens
// the probe again. A bounded, low-frequency full probe also permits recovery
// from a later-subframe outlier that did not affect the first interpolation.
// Source/history/settings resets discard this relationship.
class VfgOverloadEstimate {
    std::optional<double> full_,first_;
    int64_t nextProbe_=0;
    static constexpr int64_t probeInterval=300000000; // 30 s, in 100 ns units.
    static bool positive(std::optional<double> x){return x&&std::isfinite(*x)&&*x>0;}
public:
    void reset(){full_.reset();first_.reset();nextProbe_=0;}
    void observe(std::optional<double> full,std::optional<double> first,int64_t now){
        if(positive(full)&&positive(first)&&*full>=*first){full_=full;first_=first;nextProbe_=now+probeInterval;}
    }
    // Call only if the original admission accepts this pair. Reserve one
    // probe before submission so several in-flight pairs cannot all retry.
    bool allowProbe(int64_t now){
        if(!positive(full_)||now<nextProbe_)return false;
        nextProbe_=now+probeInterval;return true;
    }
    std::optional<double> severeTotal(std::optional<double> base,std::optional<double> currentFirst,double sourceIntervalMs)const{
        if(!positive(full_)||!positive(first_)||!positive(currentFirst)||!base||!std::isfinite(*base)||*base<0||
           !std::isfinite(sourceIntervalMs)||sourceIntervalMs<=0||*currentFirst<*first_*0.8)return {};
        const double total=*base+*full_*(*currentFirst / *first_);
        if(!std::isfinite(total)||total<=2*sourceIntervalMs)return {};
        return total;
    }
    static bool fits(double total,double base,double deadlineMs,double graceMs,double elapsed,double queued,double blit){
        const double queue=std::isfinite(queued)?std::max(0.0,queued):0.0;
        const double progress=std::clamp(elapsed,0.0,queue+base);
        return deadlineMs+std::max(0.0,graceMs)>=std::max(0.0,queue+total-progress)+std::max(0.0,blit);
    }
};
}
