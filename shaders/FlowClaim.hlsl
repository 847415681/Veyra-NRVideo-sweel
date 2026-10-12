// Disocclusion test for frame-generation motion without backward flow (FidelityFX optical
// flow estimates one direction only). Every flow pixel claims the previous-frame pixel its
// refined vector lands on, and the best photometric match keeps the claim. Background a
// moving object has just uncovered lands either on the object, which is still there and
// matches itself exactly, or on background that moved on and is claimed by the pixel now
// showing it - with a different vector either way. The disagreement with the winner's
// vector then stands in for NVOF's forward-backward check in FlowFill. Pixels sharing a
// target with the same motion (rounding, zoom) disagree by nothing.
// Three dispatches per frame: clear, claim, resolve.
Texture2D<float4> refined : register(t0);        // xy vector, z cost (FlowRefine)
RWTexture2D<uint> claims : register(u0);         // flow extent: inverted cost << indexBits | pixel index
RWTexture2D<float> agreement : register(u1);     // flow extent: disagreement with the winner, flow pixels

cbuffer FlowClaimConstants : register(b0)
{
    uint flowW;
    uint flowH;
    uint mode;       // 0 clear, 1 claim, 2 resolve
    uint indexBits;  // bits holding the claiming pixel's index; the rest rank its cost
    uint unused0;
    uint unused1;
    uint unused2;
    uint unused3;
};

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= flowW || id.y >= flowH) return;
    const int2 p = int2(id.xy);
    if (mode == 0) { claims[p] = 0; return; }
    const float4 r = refined[p];
    const int2 t = p + int2(round(r.xy));
    const bool inside = all(t >= 0) && t.x < int(flowW) && t.y < int(flowH);
    if (mode == 1) {
        if (!inside) return;
        const uint rankMax = (1u << (32u - indexBits)) - 1u;
        // Lower cost ranks higher; costs above 0.25 all rank last.
        const uint rank = rankMax - uint(saturate(r.z * 4.0) * float(rankMax));
        uint previous;
        InterlockedMax(claims[t], (rank << indexBits) | (uint(p.y) * flowW + uint(p.x)), previous);
        return;
    }
    // Outside the previous frame there is nothing to compare: keep the vector.
    float disagreement = 0;
    if (inside) {
        const uint index = claims[t] & ((1u << indexBits) - 1u);
        disagreement = length(r.xy - refined[int2(index % flowW, index / flowW)].xy);
    }
    agreement[p] = disagreement;
}
