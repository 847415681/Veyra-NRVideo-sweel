// Beauty node: shared face tracking, entirely on the GPU, once per source frame.
//
// On a detection frame the YuNet candidates (BeautyYuNetDecode.hlsl) go through
// the same non-maximum suppression as OpenCV's FaceDetectorYN (whole-pixel boxes,
// highest score first, ties in anchor order, IoU above tNms drops a box), keeping
// at most tMaxFaces faces. Those faces update up to eight tracks: stable ids, an
// alpha-beta filter (position weight tAlpha, velocity weight kBeta) so a face
// does not jitter between detections or lag behind on steady motion, and a
// grace period followed by a fade when a face is lost. Every other frame only
// advances the tracks by their velocity. Nothing is read back for the picture.
#include "BeautyFaces.hlsli"

cbuffer TrackParams : register(b0) {
    uint tDetWidth; uint tDetHeight;    // detector content size, pixels
    uint tCapacity;                     // candidate list capacity
    uint tFlags;                        // bit 0 detected this frame, bit 1 reset, bit 2 still picture, bit 3 no prediction
    float tDtMs;                        // time since the previous frame (source timestamps)
    float tAlpha;                       // weight of a new detection, 1 = follow it exactly
    float tNms;                         // suppression IoU
    float tGraceMs;                     // a lost face stays this long
    float tFadeMs;                      // then fades out over this long
    uint tMaxFaces;                     // 1 .. 8
    float tScoreThreshold;
    uint tReserved0; uint tReserved1; uint tReserved2; uint tReserved3; uint tReserved4;
};
RWStructuredBuffer<BeautyFace> Faces : register(u0);
RWByteAddressBuffer Candidates : register(u1);

static const uint kGroup = 1024u;
static const float kBeta = 0.25;          // velocity correction per detection
static const float kMaxSpeed = 0.004;     // frame fractions per ms
static const float kMatchIou = 0.15;

// 24 KB of the 32 KB limit: boxes are whole pixels as 16-bit pairs, a removed
// candidate has a negative score.
groupshared float gScore[kGroup];
groupshared uint gId[kGroup];
groupshared uint2 gBox[kGroup];
groupshared float gBestScore[kGroup];
groupshared uint gBestIndex[kGroup];
groupshared uint gKept[kBeautyMaxFaces];
groupshared uint gKeptCount;

uint pack16(int a, int b) { return (uint(a) & 0xffffu) | (uint(b) << 16); }
int4 unpackBox(uint2 p) {
    return int4((int(p.x << 16)) >> 16, int(p.x) >> 16, (int(p.y << 16)) >> 16, int(p.y) >> 16);
}
float overlapInt(int4 a, int4 b) {
    const int x1 = max(a.x, b.x), y1 = max(a.y, b.y);
    const int x2 = min(a.x + a.z, b.x + b.z), y2 = min(a.y + a.w, b.y + b.w);
    const int inter = (x2 > x1 && y2 > y1) ? (x2 - x1) * (y2 - y1) : 0;
    const int uni = a.z * a.w + b.z * b.w - inter;
    return uni > 0 ? float(inter) / float(uni) : 0.0;
}
// Normalised centre / half-size boxes.
float overlap(float4 a, float4 b) {
    const float2 lo = max(a.xy - a.zw, b.xy - b.zw), hi = min(a.xy + a.zw, b.xy + b.zw);
    const float2 extent = max(hi - lo, 0.0);
    const float inter = extent.x * extent.y;
    const float uni = 4.0 * (a.z * a.w + b.z * b.w) - inter;
    return uni > 0.0 ? inter / uni : 0.0;
}

[numthreads(1024, 1, 1)]
void main(uint t : SV_GroupIndex) {
    const bool detected = (tFlags & 1u) != 0;
    const uint n = detected ? min(Candidates.Load(0), min(tCapacity, kGroup)) : 0u;
    gScore[t] = -1.0;
    gId[t] = 0xffffffffu;
    gBox[t] = uint2(0u, 0u);
    if (t < n) {
        const uint o = 16u + t * 64u;
        const float4 box = asfloat(Candidates.Load4(o));
        const uint4 tail = Candidates.Load4(o + 48u);
        const float score = asfloat(tail.z);
        const bool valid = score >= tScoreThreshold && box.z > 0.0 && box.w > 0.0 && !any(isnan(box)) && !any(isinf(box)) &&
            all(abs(box) < 16384.0);
        const int4 b = int4(box);   // truncation toward zero, like OpenCV's Rect2i
        gScore[t] = valid ? score : -1.0;
        gId[t] = tail.w;
        gBox[t] = uint2(pack16(b.x, b.y), pack16(b.z, b.w));
    }
    if (t == 0) gKeptCount = 0u;
    GroupMemoryBarrierWithGroupSync();

    // Greedy suppression, tMaxFaces rounds of "best remaining box, drop what overlaps it".
    for (uint k = 0; k < min(tMaxFaces, kBeautyMaxFaces); ++k) {
        gBestScore[t] = gScore[t];
        gBestIndex[t] = t;
        GroupMemoryBarrierWithGroupSync();
        for (uint stride = kGroup / 2; stride > 0; stride >>= 1) {
            if (t < stride) {
                const float a = gBestScore[t], b = gBestScore[t + stride];
                const uint ia = gBestIndex[t], ib = gBestIndex[t + stride];
                if (b > a || (b == a && b >= 0.0 && gId[ib] < gId[ia])) { gBestScore[t] = b; gBestIndex[t] = ib; }
            }
            GroupMemoryBarrierWithGroupSync();
        }
        const bool found = gBestScore[0] >= 0.0;
        const uint winner = gBestIndex[0];
        if (found && t == 0) { gKept[k] = winner; gKeptCount = k + 1u; }
        if (found && t < n && gScore[t] >= 0.0 && (t == winner || overlapInt(unpackBox(gBox[t]), unpackBox(gBox[winner])) > tNms))
            gScore[t] = -1.0;
        GroupMemoryBarrierWithGroupSync();
    }
    if (t != 0) return;

    const bool reset = (tFlags & 2u) != 0, still = (tFlags & 4u) != 0, predict = (tFlags & 8u) == 0 && !still;
    BeautyFace header = Faces[0];
    uint count = (reset || still) ? 0u : min(header.meta.x, kBeautyMaxFaces);
    uint nextId = reset ? 1u : max(header.meta.y, 1u);
    BeautyFace tracks[kBeautyMaxFaces];
    bool matched[kBeautyMaxFaces];
    for (uint i = 0; i < kBeautyMaxFaces; ++i) { tracks[i] = Faces[1 + i]; matched[i] = false; }

    // 1. Advance every track to this frame.
    for (uint a = 0; a < count; ++a) {
        tracks[a].motion.z += tDtMs;
        if (predict && tracks[a].motion.z <= tGraceMs) tracks[a].box.xy += tracks[a].motion.xy * tDtMs;
    }
    // 2. Detections, best first, update the closest unmatched track or start one.
    const float2 det = float2(max(tDetWidth, 1u), max(tDetHeight, 1u));
    for (uint d = 0; d < gKeptCount; ++d) {
        const uint o = 16u + gKept[d] * 64u;
        const float4 raw = asfloat(Candidates.Load4(o));
        const float4 eyes = asfloat(Candidates.Load4(o + 16u));       // right eye x, y, left eye x, y
        const float score = asfloat(Candidates.Load(o + 56u));
        const float4 box = float4((raw.xy + raw.zw * 0.5) / det, raw.zw * 0.5 / det);
        float2 roll = eyes.zw - eyes.xy;
        if (roll.x < 0.0) roll = -roll;
        roll = dot(roll, roll) > 1e-6 ? normalize(roll) : float2(1.0, 0.0);
        int best = -1; float bestOverlap = kMatchIou;
        for (uint j = 0; j < count; ++j) {
            if (matched[j]) continue;
            const float v = overlap(tracks[j].box, box);
            if (v > bestOverlap) { bestOverlap = v; best = int(j); }
        }
        if (best >= 0) {
            BeautyFace f = tracks[best];
            const float2 innovation = box.xy - f.box.xy;
            // A jump of more than half a face is a new position, not noise.
            const bool jump = length(innovation / max(f.box.zw, 1e-4)) > 1.0;
            const float alpha = (still || jump) ? 1.0 : tAlpha;
            if (predict && f.motion.z > 0.0) {
                const float2 v = f.motion.xy + kBeta * innovation / max(f.motion.z, 1.0);
                f.motion.xy = clamp(v, -kMaxSpeed, kMaxSpeed);
            }
            if (jump) f.motion.xy = 0.0;
            f.box = lerp(f.box, box, alpha);
            const float2 r = lerp(f.shape.xy, roll, alpha);
            f.shape.xy = dot(r, r) > 1e-6 ? normalize(r) : roll;
            f.shape.z = score;
            f.shape.w = 1.0;
            f.motion.z = 0.0;
            f.meta.y += 1u;
            tracks[best] = f;
            matched[best] = true;
        } else if (count < min(tMaxFaces, kBeautyMaxFaces)) {
            BeautyFace f;
            f.box = box;
            f.shape = float4(roll, score, 1.0);
            f.motion = 0.0;
            f.meta = uint4(nextId++, 1u, 0u, 0u);
            tracks[count] = f;
            matched[count] = true;
            ++count;
        }
    }
    // 3. Presence: full during the grace period after the last match, then a fade.
    uint kept = 0;
    for (uint m = 0; m < count; ++m) {
        BeautyFace f = tracks[m];
        f.shape.w = saturate(1.0 - max(f.motion.z - tGraceMs, 0.0) / max(tFadeMs, 1.0));
        if (f.motion.z > tGraceMs) f.motion.xy = 0.0;
        if (f.shape.w <= 0.0) continue;
        tracks[kept++] = f;
    }
    header.meta = uint4(kept, nextId, header.meta.z + 1u, n);
    Faces[0] = header;
    for (uint w = 0; w < kBeautyMaxFaces; ++w) {
        BeautyFace f = tracks[w];
        if (w >= kept) { f.box = 0.0; f.shape = float4(1.0, 0.0, 0.0, 0.0); f.motion = 0.0; f.meta = 0u; }
        Faces[1 + w] = f;
    }
}
