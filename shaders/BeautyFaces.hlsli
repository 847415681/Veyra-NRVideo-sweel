// Tracked faces shared by every beauty node of one graph. BeautyTrack.hlsl owns the
// buffer; the node passes read it. Entry 0 is the header, entries 1..8 the faces.
#ifndef VEYRA_BEAUTY_FACES_HLSLI
#define VEYRA_BEAUTY_FACES_HLSLI
struct BeautyFace {
    float4 box;     // centre x, centre y, half width, half height: fractions of the source frame
    float4 shape;   // cos roll, sin roll (eye line), recent peak detector score, presence 0..1
    float4 motion;  // filtered speed x, y (fractions per ms), ms since the last detection match, half-width speed
    uint4 meta;     // header: count, next id, frame counter, raw candidates;
                    // face: id, hits, filtered centre x, y at the last match (float bits)
};
static const uint kBeautyMaxFaces = 8u;
#endif
