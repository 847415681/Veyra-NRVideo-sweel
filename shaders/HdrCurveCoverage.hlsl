// Sparse display statistics only: no pixel buffer ever leaves the GPU.
#include "HdrColor.hlsli"
#include "PresentationRotation.hlsli"
Texture2D<float4> sourceTex : register(t0);
RWStructuredBuffer<uint> stats : register(u0);
cbuffer Params : register(b0) { float4 srcDims; float4 dstDims; float4 curve0; float4 curve1; };
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x>=64 || id.y>=36) return;
    float2 uv=(float2(id.xy)+0.5)/float2(64,36);
    if(srcDims.w>0){
        float fit=min(dstDims.x/srcDims.x,dstDims.y/srcDims.y)*srcDims.w;
        uv=(uv-.5)*dstDims.xy/(srcDims.xy*fit)+dstDims.zw;
    }
    uint flags=uint(srcDims.z+0.5);
    float3 rgb=0;
    if(all(uv>=0)&&all(uv<=1)){
        uv=PresentationSourceUv(uv,(flags>>6)&3);
        uint width,height;sourceTex.GetDimensions(width,height);
        rgb=sourceTex.Load(int3(min(uint2(uv*float2(width,height)),uint2(width-1,height-1)),0)).rgb;
        if(flags&32)rgb=HdrDecodePq(rgb);
    }
    float3 nits=rgb*80.0;
    float y=dot(nits,float3(.212639,.715169,.072192));
    InterlockedAdd(stats[0],1);
    if(!isfinite(y)||y<=1e-6)return;
    InterlockedMax(stats[4],asuint(y));
    if(curve0.x!=0&&curve0.y>0&&y<curve0.y)InterlockedAdd(stats[3],1);
    float before=y;
    if(curve0.x!=0&&curve0.y>0){float w=saturate(1-y/curve0.y);y*=1+w*w*(exp2(curve0.x)-1);}
    float peak=curve1.y>0?curve1.y:1000;
    if(curve0.z>0&&abs(curve0.z-203)>.5&&peak>curve0.z+1)y=peak*pow(max(y/peak,0),log(curve0.z/peak)/log(203/peak));
    if(curve0.w>0&&curve1.x>0&&y>curve0.w){InterlockedAdd(stats[2],1);y=curve0.w+(y-curve0.w)/(1+(y-curve0.w)/curve1.x);}
    bool clipped=curve1.y>0&&y>curve1.y;
    // PresentBlit also caps the brightest BT.2020 channel to preserve hue.
    if(curve1.y>0&&max(max(HdrTo2020(nits*y/before).r,HdrTo2020(nits*y/before).g),HdrTo2020(nits*y/before).b)>curve1.y)clipped=true;
    if(clipped)InterlockedAdd(stats[1],1);
}
