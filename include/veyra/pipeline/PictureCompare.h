#pragma once
// Repeated-picture detection with a capture-noise band (2026-10-12).
//
// Content dedup (ContentDedup.h) has to know whether a sample shows the same
// picture as the last new one. Two earlier answers failed on a real card:
//  - 64x36 sampled points with an 8-level gate (2026-10-11) missed small real
//    motion seen from a still position and dropped real pictures (6 fps).
//  - Byte-identical whole pictures never matched: the MCS T911 at 2560x1440@60
//    NV12 changes ~18% of the pixels of a repeated console frame by 1 level, a
//    few hundred by 2 and at most one by 3, while a new 30 fps game frame seen
//    from a still position has at least 240 pixels beyond 2 levels and peaks at
//    6-16 (tests/claude-cadence-exact-20261012/harness/card_diff.py, 173 pairs).
// A sample repeats the reference only when EVERY component of every plane of
// the whole picture is within 3 levels on an 8-bit scale (bandFor(): the same
// fraction of a deeper component); one component beyond it makes a new picture. Packed
// layouts whose bytes are not components (v210, RGB10, RGB555/565) compare
// exactly. The reference is the last NEW picture, so a slow change accumulates
// past the band instead of slipping through it one small step at a time.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <emmintrin.h>
#include <intrin.h>
#include <smmintrin.h>
#include <vector>

namespace veyra::pipeline {

struct PicturePlane {
    const uint8_t* data = nullptr;
    size_t pitch = 0, rowBytes = 0;
    unsigned rows = 0;
};
enum class PictureComponents { Bytes, Words, Exact };

class PictureReference {
public:
    static constexpr unsigned kBand8 = 3; // levels on an 8-bit scale
    // The band for components of `depth` bits stored `shift` bits up (P010: 10, 6 -> 768).
    static constexpr unsigned bandFor(unsigned depth, unsigned shift) {
        return depth <= 8 ? kBand8 : ((kBand8 * ((1u << depth) - 1) + 127) / 255) << shift;
    }
    struct Result {
        uint64_t id = 0;       // picture number; equal ids show the same picture
        bool repeat = false;   // every component within the band of the reference
        uint64_t over = 0;     // components beyond the band (0 for a repeat)
        unsigned max8 = 0;     // largest difference, 8-bit scale
    };
    // Ids of different references never collide (space in the top byte).
    explicit PictureReference(unsigned space = 0) : space_(uint64_t(space & 0xFF) << 56) {}
    void reset() { valid_ = false; }

    // band: per-component tolerance in the stored unit (bandFor(); ignored for Exact).
    Result observe(const PicturePlane* planes, unsigned count, PictureComponents kind, unsigned band = kBand8) {
        if (kind == PictureComponents::Exact) band = 0;
        Result r;
        size_t total = 0;
        for (unsigned p = 0; p < count; ++p) total += planes[p].rowBytes * planes[p].rows;
        if (valid_ && total == bytes_ && kind == kind_ && band == band_) {
            const uint8_t* ref = reference_.data(); unsigned max = 0;
            for (unsigned p = 0; p < count; ++p)
                for (unsigned y = 0; y < planes[p].rows; ++y, ref += planes[p].rowBytes) {
                    const uint8_t* row = planes[p].data + size_t(y) * planes[p].pitch;
                    if (kind == PictureComponents::Words) compareWords(row, ref, planes[p].rowBytes, band, r.over, max);
                    else compareBytes(row, ref, planes[p].rowBytes, band, r.over, max);
                }
            r.max8 = band ? (max * kBand8 + band / 2) / band : max;
            if (r.over == 0) { r.id = space_ | id_; r.repeat = true; return r; }
        }
        // A new picture becomes the reference.
        if (reference_.size() < total) reference_.resize(total);
        uint8_t* ref = reference_.data();
        for (unsigned p = 0; p < count; ++p)
            for (unsigned y = 0; y < planes[p].rows; ++y, ref += planes[p].rowBytes)
                std::memcpy(ref, planes[p].data + size_t(y) * planes[p].pitch, planes[p].rowBytes);
        bytes_ = total; kind_ = kind; band_ = band; valid_ = true; id_ = (id_ + 1) & 0x00FFFFFFFFFFFFFFull;
        r.id = space_ | id_;
        return r;
    }

private:
    static void compareBytes(const uint8_t* a, const uint8_t* b, size_t n, unsigned band, uint64_t& over, unsigned& max) {
        const __m128i limit = _mm_set1_epi8(char(band)), zero = _mm_setzero_si128();
        __m128i peak = zero; size_t i = 0; uint64_t count = 0;
        for (; i + 16 <= n; i += 16) {
            const __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a + i)), y = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + i));
            const __m128i d = _mm_or_si128(_mm_subs_epu8(x, y), _mm_subs_epu8(y, x));
            peak = _mm_max_epu8(peak, d);
            count += __popcnt(~unsigned(_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_subs_epu8(d, limit), zero))) & 0xFFFFu);
        }
        for (; i < n; ++i) { const unsigned d = a[i] > b[i] ? a[i] - b[i] : b[i] - a[i]; max = std::max(max, d); count += d > band; }
        alignas(16) uint8_t lanes[16]; _mm_store_si128(reinterpret_cast<__m128i*>(lanes), peak);
        for (const uint8_t v : lanes) max = std::max<unsigned>(max, v);
        over += count;
    }
    // Little-endian 16-bit components (P010/P016 and >8-bit planar frames).
    static void compareWords(const uint8_t* a, const uint8_t* b, size_t n, unsigned band, uint64_t& over, unsigned& max) {
        const __m128i limit = _mm_set1_epi16(short(band)), zero = _mm_setzero_si128();
        __m128i peak = zero; size_t i = 0; uint64_t count = 0;
        for (; i + 16 <= n; i += 16) {
            const __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a + i)), y = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + i));
            const __m128i d = _mm_or_si128(_mm_subs_epu16(x, y), _mm_subs_epu16(y, x));
            peak = _mm_max_epu16(peak, d);
            count += __popcnt(~unsigned(_mm_movemask_epi8(_mm_cmpeq_epi16(_mm_subs_epu16(d, limit), zero))) & 0xFFFFu) / 2;
        }
        for (; i + 2 <= n; i += 2) {
            const unsigned x = a[i] | (unsigned(a[i + 1]) << 8), y = b[i] | (unsigned(b[i + 1]) << 8), d = x > y ? x - y : y - x;
            max = std::max(max, d); count += d > band;
        }
        alignas(16) uint16_t lanes[8]; _mm_store_si128(reinterpret_cast<__m128i*>(lanes), peak);
        for (const uint16_t v : lanes) max = std::max<unsigned>(max, v);
        over += count;
    }

    std::vector<uint8_t> reference_;
    size_t bytes_ = 0;
    uint64_t id_ = 0, space_ = 0;
    unsigned band_ = 0;
    bool valid_ = false;
    PictureComponents kind_ = PictureComponents::Bytes;
};

} // namespace veyra::pipeline
