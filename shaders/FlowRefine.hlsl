// Frame-generation motion refinement (XeSS, motion source "optical flow").
// NVOF returns one current->previous vector per grid cell, so a cell that straddles a
// moving character and the background moves both with one vector. Per flow pixel, pick
// the vector of the 3x3 neighbouring cells whose 3x3 luma patch matches the previous
// frame best: motion boundaries follow the picture instead of the grid. Unlike
// NvofDensify nothing is zeroed; the best cost is kept for FlowFill to find pixels that
// have no correspondence at all (background uncovered by a moving object).
Texture2D<int2> rawFlow : register(t0);          // grid extent, S10.5 flow pixels
Texture2D<float4> previousColor : register(t1);  // flow extent
Texture2D<float4> currentColor : register(t2);   // flow extent
RWTexture2D<float4> refined : register(u0);      // xy vector, z best cost, w own-cell cost

cbuffer FlowRefineConstants : register(b0)
{
    uint gridW;
    uint gridH;
    uint flowW;
    uint flowH;
    uint gridSize;
    float tieBias;   // added to every neighbour cell's cost: the own cell wins ties
    uint unused0;
    uint unused1;
};

float luma(float3 c) { return dot(c, float3(.2126, .7152, .0722)); }

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= flowW || id.y >= flowH) return;
    const int2 p = int2(id.xy);
    const int2 limit = int2(flowW - 1, flowH - 1);
    float current[9];
    [unroll] for (int j = 0; j < 9; ++j)
        current[j] = luma(currentColor[clamp(p + int2(j % 3 - 1, j / 3 - 1), int2(0, 0), limit)].rgb);
    const int2 cell = p / int(gridSize);
    const int2 cellLimit = int2(gridW - 1, gridH - 1);
    float bestCost = 1e9, ownCost = 0;
    float2 best = 0;
    [unroll] for (int k = 0; k < 9; ++k) {
        const int2 offset = int2(k % 3 - 1, k / 3 - 1);
        const float2 v = float2(rawFlow[clamp(cell + offset, int2(0, 0), cellLimit)]) / 32.0;
        const int2 q = p + int2(round(v));
        float cost = 0;
        [unroll] for (int j = 0; j < 9; ++j)
            cost += abs(current[j] - luma(previousColor[clamp(q + int2(j % 3 - 1, j / 3 - 1), int2(0, 0), limit)].rgb));
        cost /= 9;
        if (k == 4) ownCost = cost; else cost += tieBias;
        if (cost < bestCost) { bestCost = cost; best = v; }
    }
    refined[p] = float4(best, bestCost, ownCost);
}
