#pragma once
// Exact picture identity for repeated-frame removal (2026-10-12).
//
// ContentDedup judged a repeat on 64x36 sampled points with a change gate of 8
// levels, so a still scene with small real motion lost real pictures (field log
// 2026-10-12: a running 1440p P010 HDR game kept 6-16 of 60 frames). A picture
// now repeats only when every byte of every plane is identical: rows are
// absorbed into eight 128-bit lanes, two AES rounds per 16 bytes. Each step is a
// bijection of the lane state, so a changed byte changes its lane and the lanes
// re-converge only if a later difference cancels a pseudo-random 128-bit state
// difference exactly. The 128-byte state is compared whole; nothing is
// thresholded. ~0.2 ms for a 1440p P010 picture on one core (bench 2026-10-12).
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <emmintrin.h>
#include <intrin.h>
#include <wmmintrin.h>

namespace veyra::pipeline {

struct PictureFingerprint {
    std::array<uint64_t, 16> words{};
    bool operator==(const PictureFingerprint&) const = default;
};

class PictureHasher {
public:
    static constexpr unsigned kLanes = 8;
    PictureHasher() {
        for (unsigned i = 0; i < kLanes; ++i) {
            lanes_[i] = _mm_set_epi64x(int64_t(0x9E3779B97F4A7C15ull * (i + 1)), int64_t(0xC2B2AE3D27D4EB4Full ^ (i * 0x165667B19E3779F9ull)));
            scalar_[2 * i] = 0x9E3779B97F4A7C15ull * (2 * i + 1); scalar_[2 * i + 1] = 0xC2B2AE3D27D4EB4Full * (2 * i + 2);
        }
    }
    // One row (or any byte run); call in the same order for every picture.
    void absorb(const uint8_t* data, size_t bytes) {
        total_ += bytes;
        if (!aes()) { absorbScalar(data, bytes); return; }
        const __m128i k1 = _mm_set_epi64x(0x243F6A8885A308D3ll, 0x13198A2E03707344ll), k2 = _mm_set_epi64x(int64_t(0xA4093822299F31D0ull), 0x082EFA98EC4E6C89ll);
        size_t offset = 0;
        // Lanes stay in registers across the row (a member array is reloaded every block).
        __m128i a = lanes_[0], b = lanes_[1], c = lanes_[2], d = lanes_[3], e = lanes_[4], f = lanes_[5], g = lanes_[6], h = lanes_[7];
        const auto mix = [&](__m128i s, size_t at) { return _mm_aesenc_si128(_mm_aesenc_si128(_mm_xor_si128(s, _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + at))), k1), k2); };
        for (; offset + 16 * kLanes <= bytes; offset += 16 * kLanes) {
            a = mix(a, offset); b = mix(b, offset + 16); c = mix(c, offset + 32); d = mix(d, offset + 48);
            e = mix(e, offset + 64); f = mix(f, offset + 80); g = mix(g, offset + 96); h = mix(h, offset + 112);
        }
        lanes_[0] = a; lanes_[1] = b; lanes_[2] = c; lanes_[3] = d; lanes_[4] = e; lanes_[5] = f; lanes_[6] = g; lanes_[7] = h;
        for (unsigned i = 0; offset < bytes; ++i, offset += 16) {
            alignas(16) uint8_t tail[16] = {};
            std::memcpy(tail, data + offset, bytes - offset < 16 ? bytes - offset : 16);
            lanes_[i] = _mm_aesenc_si128(_mm_aesenc_si128(_mm_xor_si128(lanes_[i], _mm_load_si128(reinterpret_cast<const __m128i*>(tail))), k1), k2);
        }
    }
    void rows(const uint8_t* base, size_t pitch, size_t rowBytes, unsigned count) {
        for (unsigned y = 0; y < count; ++y) absorb(base + size_t(y) * pitch, rowBytes);
    }
    PictureFingerprint finish() const {
        PictureFingerprint out;
        if (aes()) for (unsigned i = 0; i < kLanes; ++i) _mm_storeu_si128(reinterpret_cast<__m128i*>(out.words.data() + 2 * i), lanes_[i]);
        else out.words = scalar_;
        out.words[0] ^= total_;
        return out;
    }
    static bool aes() {
        static const bool available = [] { int info[4] = {}; __cpuid(info, 1); return (info[2] & (1 << 25)) != 0; }();
        return available;
    }

private:
    // CPUs without AES-NI: sixteen 64-bit lanes, xor-multiply-rotate-multiply per word (each a bijection).
    void absorbScalar(const uint8_t* data, size_t bytes) {
        for (size_t offset = 0; offset < bytes; offset += 8) {
            uint64_t word = 0; std::memcpy(&word, data + offset, bytes - offset < 8 ? bytes - offset : 8);
            uint64_t& lane = scalar_[(offset / 8) % scalar_.size()];
            lane = _rotl64((lane ^ word) * 0x9FB21C651E98DF25ull, 29) * 0xD6E8FEB86659FD93ull;
        }
    }
    __m128i lanes_[kLanes];
    std::array<uint64_t, 16> scalar_{};
    uint64_t total_ = 0;
};

} // namespace veyra::pipeline
