// Disocclusion fill for frame-generation motion (after FlowRefine). A pixel whose best
// candidate still does not match the previous frame has no correspondence there: usually
// background that a moving object has just uncovered. Zero or a wrong vector makes the
// interpolator drag that background with the object (the "jelly" around characters).
// Take the vector of the most similar-coloured confident pixel nearby instead; uncovered
// background resembles the background beside it, which moves with the background.
Texture2D<float4> refined : register(t0);        // xy vector, z best cost (FlowRefine)
Texture2D<float4> currentColor : register(t1);   // flow extent
RWTexture2D<float2> output : register(u0);       // flow extent, current->previous pixels

cbuffer FlowFillConstants : register(b0)
{
    uint flowW;
    uint flowH;
    float threshold;      // best cost above this has no correspondence
    float distanceWeight; // colour distance added per pixel of search distance
    uint unused0;
    uint unused1;
    uint unused2;
    uint unused3;
};

static const int2 kDirections[8] = { int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1), int2(1, 1), int2(-1, 1), int2(1, -1), int2(-1, -1) };
static const int kRadii[9] = { 2, 4, 8, 12, 16, 24, 32, 48, 64 };

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= flowW || id.y >= flowH) return;
    const int2 p = int2(id.xy);
    const float4 own = refined[p];
    if (own.z <= threshold) { output[p] = own.xy; return; }
    const float3 colour = currentColor[p].rgb;
    float bestScore = 1e9;
    float2 vector = own.xy;
    [loop] for (int r = 0; r < 9; ++r) {
        [unroll] for (int d = 0; d < 8; ++d) {
            const int2 q = p + kDirections[d] * kRadii[r];
            if (any(q < 0) || q.x >= int(flowW) || q.y >= int(flowH)) continue;
            const float4 candidate = refined[q];
            if (candidate.z > threshold) continue;
            const float3 delta = currentColor[q].rgb - colour;
            const float score = dot(abs(delta), float3(1, 1, 1)) + distanceWeight * kRadii[r];
            if (score < bestScore) { bestScore = score; vector = candidate.xy; }
        }
    }
    output[p] = vector;
}
