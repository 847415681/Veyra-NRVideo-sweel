// SPDX-License-Identifier: GPL-3.0-only
// Shot-change test for one VFG input pair (VfgBackend). It replaces the SDK's
// AutomaticShotChangeDetection, which synchronised the CPU with every Run and
// was evaluated again for every subframe of the same pair.
//
// Same measures and thresholds as core::SceneCadenceAnalyzer on the CPU path:
// 64x36 luma samples, 256-bin histogram distance (L1 / 2) and normalised SAD;
// a flash (large SAD, near-identical histogram) is not a cut. The samples come
// from the encoded A/B buffers VFG itself reads, so every source kind (capture,
// software and hardware decode, streaming) is covered without a CPU readback.
//
// flags[parity] (64-bit, offset parity*8) = 1 when the pair crosses a cut; the
// subframe copies of that pair are predicated on it and show B instead.

cbuffer Constants : register(b0)
{
    uint width;
    uint height;
    uint pitch;      // bytes per row of both buffers
    uint tenBit;     // 1: R10G10B10A2, 0: R8G8B8A8
    uint parity;
    uint compare;    // 0 when the pair has no valid A (reset): never a cut
    float cutSad;    // SceneCadenceAnalyzer::Config::sceneCutSadThreshold
    float cutHistogram;
    float replacementSad;
    float replacementHistogram;
    float flashSad;
    float flashHistogram;
};

ByteAddressBuffer previousFrame : register(t0);
ByteAddressBuffer currentFrame : register(t1);
RWByteAddressBuffer flags : register(u0);

static const uint kColumns = 64;
static const uint kRows = 36;
static const uint kSamples = kColumns * kRows;

groupshared uint histogramPrevious[256];
groupshared uint histogramCurrent[256];
groupshared uint sadSum;
groupshared uint histogramL1;

// Integer luma weights of the CPU sampler (packedIngressLuma): 8-bit
// (54R+183G+19B+128)>>8, 10-bit min(255,(54R+183G+19B+512)>>10).
uint lumaAt(ByteAddressBuffer frame, uint x, uint y)
{
    const uint word = frame.Load(y * pitch + x * 4);
    if (tenBit != 0) {
        const uint r = word & 1023u, g = (word >> 10) & 1023u, b = (word >> 20) & 1023u;
        return min(255u, (54u * r + 183u * g + 19u * b + 512u) >> 10);
    }
    const uint r = word & 255u, g = (word >> 8) & 255u, b = (word >> 16) & 255u;
    return (54u * r + 183u * g + 19u * b + 128u) >> 8;
}

[numthreads(256, 1, 1)]
void main(uint index : SV_GroupIndex)
{
    histogramPrevious[index] = 0;
    histogramCurrent[index] = 0;
    if (index == 0) {
        sadSum = 0;
        histogramL1 = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    if (compare != 0) {
        for (uint i = index; i < kSamples; i += 256) {
            const uint x = (i % kColumns) * width / kColumns;
            const uint y = (i / kColumns) * height / kRows;
            const uint a = lumaAt(previousFrame, x, y);
            const uint b = lumaAt(currentFrame, x, y);
            InterlockedAdd(histogramPrevious[a], 1);
            InterlockedAdd(histogramCurrent[b], 1);
            InterlockedAdd(sadSum, a > b ? a - b : b - a);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    const uint p = histogramPrevious[index], c = histogramCurrent[index];
    InterlockedAdd(histogramL1, p > c ? p - c : c - p);
    GroupMemoryBarrierWithGroupSync();
    if (index == 0) {
        const float sad = float(sadSum) / (255.0 * kSamples);
        const float histogram = float(histogramL1) / (2.0 * kSamples);
        const bool flash = sad > flashSad && histogram < flashHistogram;
        const bool cut = compare != 0 && !flash &&
            ((sad > cutSad && histogram > cutHistogram) ||
             (sad > replacementSad && histogram > replacementHistogram));
        flags.Store2(parity * 8, uint2(cut ? 1u : 0u, 0u));
        // Diagnostics for tests: last scores per parity and a running cut count.
        flags.Store3(16 + parity * 16, uint3(asuint(sad), asuint(histogram), compare));
        if (cut) {
            uint previousCount;
            flags.InterlockedAdd(48, 1, previousCount);
        }
    }
}
