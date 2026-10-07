// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <cmath>
#include <optional>

namespace veyra::engine {
// A reduced VFG group executes one interpolation, not the requested M-1.
// Its queued work is base + the measured first interpolation. Retain the full
// estimate when measurement/history is unavailable, and never increase it.
inline std::optional<double> vfgQueuedGroupCost(std::optional<double> full,
        std::optional<double> base,std::optional<double> first,bool reduced,
        unsigned evaluations,bool warmingHistory){
    const auto valid=[](std::optional<double> value){return value&&std::isfinite(*value)&&*value>=0;};
    if(!reduced||evaluations!=1||warmingHistory||!valid(full)||!valid(base)||!valid(first)||*full<*base)return full;
    return std::min(*full,*base+*first);
}
}
