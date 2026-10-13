// Container rotation is clockwise. Color and provider motion share this mapping.
float2 PresentationSourceUv(float2 uv,uint turns) {
    if(turns==1)return float2(uv.y,1-uv.x);
    if(turns==2)return 1-uv;
    if(turns==3)return float2(1-uv.y,uv.x);
    return uv;
}
float2 PresentationDisplayUv(float2 uv,uint turns) {
    if(turns==1)return float2(1-uv.y,uv.x);
    if(turns==2)return 1-uv;
    if(turns==3)return float2(uv.y,1-uv.x);
    return uv;
}
