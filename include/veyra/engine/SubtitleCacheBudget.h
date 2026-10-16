#pragma once
#include <charconv>
#include <cstdint>
#include <string_view>

namespace veyra::engine {
// User-supplied fan patch, 2026-10-16 (provenance in THIRD_PARTY_NOTICES).
// A ceiling on compressed cue storage, not an allocation/reservation of 4 GiB.
inline constexpr uint64_t kDefaultSubtitleCacheBytes = 4096ull * 1024ull * 1024ull;
inline uint64_t subtitleCacheBudgetBytes(std::string_view mib) {
    if (mib.empty()) return kDefaultSubtitleCacheBytes;
    uint64_t value = 0;
    const auto parsed = std::from_chars(mib.data(), mib.data() + mib.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != mib.data() + mib.size() || value < 1 || value > 65536)
        return kDefaultSubtitleCacheBytes;
    return value * 1024ull * 1024ull;
}
}
