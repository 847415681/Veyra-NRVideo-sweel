// J0 for the "jelly" field report (2026-10-12): how much of the wobble around a
// foreground silhouette comes from the motion vectors frame generation receives?
// Synthetic 4K third-person scenes with exact mid-frame ground truth run through the
// product EnhanceGraph (FG 2X, flow at 1920x1080 like 4K capture with realtime NR).
// Only the FG motion input changes between runs:
//   zero     - all zero (the provider's own optical flow only)
//   product  - no probe: product NVOF -> NvofDensify -> FlowAdapt, what users get
//   ideal    - exact current->previous motion per pixel, background motion in the
//              disoccluded strip (fgMotionProbe)
//   blocky   - ideal sampled at the centre of each 8x8 block (the grid alone)
//   zeroband - ideal, zeroed where the product checks zero it: the disoccluded
//              strip and one block either side of the silhouette (zeroing alone)
//   inverted - ideal with the opposite sign, and garbage - +120 px everywhere: controls
//              showing whether the provider uses this input at all
//   nrflow / nrzero - NR on (1920x1080 realtime, like 4K capture), NR motion from the
//              product flow or zero; FG motion zero. Real frames are then NR output.
// Scenes: orbit (camera turns: background pans, character static on screen) and
// walk (static camera, character moves). Diagnostic readback only.
// usage: veyra_fg_motion_boundary_tests <outDir> <dlss|fsr> <orbit|walk> <variant>
#include "veyra/pipeline/EnhanceGraph.h"
#include "veyra/pipeline/GpuPassUtils.h"
#include "veyra/gfx/D3D12DeviceContext.h"
#include "veyra/gfx/CommandSlotRing.h"
#include "veyra/sink/ImageExportSink.h"
#include "veyra/RuntimePaths.h"
#include <d3d12sdklayers.h>
#include <DirectXPackedVector.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <iostream>
#include <string>
#include <vector>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}
using namespace veyra;
namespace {
constexpr int W=3840,H=2160,FlowW=1920,FlowH=1080,N=10,D=40,Band=64,Warmup=2,Block=8;
constexpr int BW=W+(N+2)*D; // background strip wide enough for the orbit pan

float hash01(int x,int y,uint32_t seed){
    uint32_t v=uint32_t(x)*0x9e3779b9u^uint32_t(y)*0x85ebca6bu^seed*0xc2b2ae35u;
    v^=v>>16;v*=0x7feb352du;v^=v>>15;v*=0x846ca68bu;v^=v>>16;return float(v&0xffffu)/65535.f;
}
float noise(float x,float y,float cell,uint32_t seed){
    const float gx=x/cell,gy=y/cell;const int ix=int(std::floor(gx)),iy=int(std::floor(gy));
    float tx=gx-ix,ty=gy-iy;tx=tx*tx*(3-2*tx);ty=ty*ty*(3-2*ty);
    const float a=hash01(ix,iy,seed),b=hash01(ix+1,iy,seed),c=hash01(ix,iy+1,seed),d=hash01(ix+1,iy+1,seed);
    return (a*(1-tx)+b*tx)*(1-ty)+(c*(1-tx)+d*tx)*ty;
}
// Character = union of capsules; part 0 head, 1 torso, 2 legs, 3 arms.
struct Capsule{float ax,ay,bx,by,r;int part;};
constexpr Capsule kShape[]={{1920,560,1920,560,120,0},{1920,790,1920,1290,230,1},{1820,1360,1800,1900,85,2},
                            {2020,1360,2040,1900,85,2},{1660,830,1600,1360,70,3},{2180,830,2240,1360,70,3}};
float sdf(float x,float y,int* part=nullptr){
    float best=1e9f;int which=-1;
    for(const auto& c:kShape){
        const float px=x-c.ax,py=y-c.ay,bx=c.bx-c.ax,by=c.by-c.ay,len=bx*bx+by*by;
        const float h=len>0?std::clamp((px*bx+py*by)/len,0.f,1.f):0.f;
        const float d=std::hypot(px-bx*h,py-by*h)-c.r;
        if(d<best){best=d;which=c.part;}
    }
    if(part)*part=which;return best;
}
struct Scene {
    bool orbit=true;
    std::vector<std::array<uint8_t,3>> background;   // BW x H
    std::vector<std::array<uint8_t,4>> character;    // W x H, alpha = coverage
    std::vector<float> distance;                     // W x H signed distance (character at offset 0)
    int bgOffset(double frame)const{return orbit?int(std::lround(frame*D)):0;}
    int chOffset(double frame)const{return orbit?0:int(std::lround(frame*D));}
    // Character layer and distance sampled with the character shifted right by `o`.
    const std::array<uint8_t,4>* ch(int x,int y,int o)const{const int sx=x-o;return sx>=0&&sx<W?&character[size_t(y)*W+sx]:nullptr;}
    float dist(int x,int y,int o)const{const int sx=x-o;return sx>=0&&sx<W?distance[size_t(y)*W+sx]:1e9f;}
    bool covered(int x,int y,int o)const{auto* c=ch(x,y,o);return c&&(*c)[3]>=128;}
    std::array<uint8_t,3> pixel(int x,int y,double frame)const{
        const auto& b=background[size_t(y)*BW+x+bgOffset(frame)];
        const auto* c=ch(x,y,chOffset(frame));if(!c||!(*c)[3])return b;
        const float a=(*c)[3]/255.f;
        return {uint8_t(std::lround(b[0]*(1-a)+(*c)[0]*a)),uint8_t(std::lround(b[1]*(1-a)+(*c)[1]*a)),uint8_t(std::lround(b[2]*(1-a)+(*c)[2]*a))};
    }
    // Exact current(frame)->previous(frame-1) motion in pixels (product flow convention).
    std::array<float,2> ideal(int x,int y,int frame)const{
        if(covered(x,y,chOffset(frame)))return {orbit?0.f:-float(D),0.f};
        return {orbit?float(D):0.f,0.f};
    }
    bool disoccluded(int x,int y,int frame)const{
        if(covered(x,y,chOffset(frame)))return false;
        // Background visible now, hidden by the character in the previous frame.
        return orbit?covered(std::min(x+D,W-1),y,chOffset(frame-1)):covered(x,y,chOffset(frame-1));
    }
};
Scene makeScene(bool orbit){
    Scene s;s.orbit=orbit;s.background.resize(size_t(BW)*H);s.character.resize(size_t(W)*H);s.distance.resize(size_t(W)*H);
    for(int y=0;y<H;++y)for(int x=0;x<BW;++x){
        const float n=.5f*noise(float(x),float(y),96,1)+.3f*noise(float(x),float(y),24,2)+.2f*noise(float(x),float(y),6,3);
        const float t=float(y)/H;
        float r=110*(1-t)+86*t,g=138*(1-t)+112*t,b=168*(1-t)+70*t; // sky -> ground
        const float m=.55f+.9f*n;r*=m;g*=m;b*=m;
        if(x%283<18){r*=.35f;g*=.32f;b*=.3f;}              // poles/trunks: strong vertical edges
        if((y/180)%3==1&&x%97<5){r=std::min(255.f,r+60);g=std::min(255.f,g+55);b=std::min(255.f,b+40);}
        s.background[size_t(y)*BW+x]={uint8_t(std::clamp(r,0.f,255.f)),uint8_t(std::clamp(g,0.f,255.f)),uint8_t(std::clamp(b,0.f,255.f))};
    }
    constexpr std::array<std::array<float,3>,4> base{{{205,160,130},{48,66,118},{42,42,48},{52,70,124}}};
    for(int y=0;y<H;++y)for(int x=0;x<W;++x){
        int part=0;const float d=sdf(x+.5f,y+.5f,&part);s.distance[size_t(y)*W+x]=d;
        const float a=std::clamp(.5f-d,0.f,1.f);
        if(a<=0){s.character[size_t(y)*W+x]={0,0,0,0};continue;}
        const float n=.7f+.6f*noise(float(x),float(y),8,11+part);
        const float stripe=part==1&&((x/16+y/16)%2)?18.f:0.f;
        const auto& c=base[size_t(part)];
        s.character[size_t(y)*W+x]={uint8_t(std::clamp(c[0]*n+stripe,0.f,255.f)),uint8_t(std::clamp(c[1]*n+stripe,0.f,255.f)),
                                    uint8_t(std::clamp(c[2]*n+stripe,0.f,255.f)),uint8_t(std::lround(a*255))};
    }
    return s;
}
bool uploadMotion(gfx::D3D12DeviceContext& ctx,gfx::CommandSlotRing& ring,ID3D12Resource* motion,ID3D12Resource* upload,
                  const std::vector<uint16_t>& halves,bool first,bool common){
    // The FSR path hands registered textures back in COMMON; DLSS reads the probe as SRV.
    const auto rest=common?D3D12_RESOURCE_STATE_COMMON:D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    void* data=nullptr;if(FAILED(upload->Map(0,nullptr,&data)))return false;
    std::memcpy(data,halves.data(),halves.size()*2);upload->Unmap(0,nullptr);
    Status st=Status::Ok;unsigned slot=0;auto* list=ring.acquireNext(slot,st);if(!list)return false;
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={motion,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,first?D3D12_RESOURCE_STATE_COMMON:rest,D3D12_RESOURCE_STATE_COPY_DEST};
    list->ResourceBarrier(1,&b);
    D3D12_TEXTURE_COPY_LOCATION dst{},src{};dst.pResource=motion;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource=upload;src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint.Footprint={DXGI_FORMAT_R16G16_FLOAT,UINT(W),UINT(H),1,UINT(W*4)};
    list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    b.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST;b.Transition.StateAfter=rest;
    list->ResourceBarrier(1,&b);
    return ring.submitAndSignal(slot)&&ring.waitIdle();
}
// R16G16_FLOAT readback of a graph-owned texture from its tracked state and back.
bool readFlow(gfx::D3D12DeviceContext& ctx,gfx::CommandSlotRing& ring,ID3D12Resource* tex,D3D12_RESOURCE_STATES state,std::vector<float>& out){
    auto d=tex->GetDesc();if(d.Format!=DXGI_FORMAT_R16G16_FLOAT)return false;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT rows=0;UINT64 rowBytes=0,total=0;
    ctx.device()->GetCopyableFootprints(&d,0,1,0,&fp,&rows,&rowBytes,&total);
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=total;bd.Height=1;bd.DepthOrArraySize=1;bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    pipeline::ComPtr<ID3D12Resource> rb;
    if(FAILED(ctx.device()->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&rb))))return false;
    Status st=Status::Ok;unsigned slot=0;auto* list=ring.acquireNext(slot,st);if(!list)return false;
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={tex,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,state,D3D12_RESOURCE_STATE_COPY_SOURCE};
    if(state!=D3D12_RESOURCE_STATE_COPY_SOURCE)list->ResourceBarrier(1,&b);
    D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=tex;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource=rb.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fp;
    list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    std::swap(b.Transition.StateBefore,b.Transition.StateAfter);if(state!=D3D12_RESOURCE_STATE_COPY_SOURCE)list->ResourceBarrier(1,&b);
    if(!ring.submitAndSignal(slot)||!ring.waitIdle())return false;
    uint8_t* data=nullptr;D3D12_RANGE range{0,SIZE_T(total)};if(FAILED(rb->Map(0,&range,reinterpret_cast<void**>(&data))))return false;
    out.resize(size_t(d.Width)*d.Height*2);
    for(UINT y=0;y<d.Height;++y){
        const auto* row=reinterpret_cast<const uint16_t*>(data+fp.Offset+size_t(y)*fp.Footprint.RowPitch);
        for(UINT64 i=0;i<d.Width*2;++i)out[size_t(y)*d.Width*2+i]=DirectX::PackedVector::XMConvertHalfToFloat(row[i]);
    }
    D3D12_RANGE empty{0,0};rb->Unmap(0,&empty);return true;
}
bool save(const std::filesystem::path& path,const sink::RgbaImage& image){std::error_code ec;std::filesystem::remove(path,ec);return sink::saveImage(path.wstring(),image);}
sink::RgbaImage crop(const std::vector<std::array<uint8_t,3>>& rgb,int cx,int cy,int size){
    sink::RgbaImage im;im.width=im.height=uint32_t(size);im.pixels.resize(size_t(size)*size*4);
    const int x0=std::clamp(cx-size/2,0,W-size),y0=std::clamp(cy-size/2,0,H-size);
    for(int y=0;y<size;++y)for(int x=0;x<size;++x){const auto& p=rgb[size_t(y0+y)*W+x0+x];auto* q=&im.pixels[(size_t(y)*size+x)*4];q[0]=p[0];q[1]=p[1];q[2]=p[2];q[3]=255;}
    return im;
}
struct Accumulator {
    double sum=0;uint64_t samples=0,bad=0;std::array<uint64_t,256> histogram{};
    void add(int r,int g,int b){const int m=std::max({r,g,b});sum+=r+g+b;samples+=3;bad+=m>32;++histogram[size_t(std::min(m,255))];}
    double mae()const{return samples?sum/samples:-1;}
    double badPercent()const{return samples?100.0*bad/(samples/3):-1;}
    int p95()const{uint64_t total=samples/3,seen=0;for(int i=0;i<256;++i){seen+=histogram[size_t(i)];if(total&&seen*100>=total*95)return i;}return -1;}
};
} // namespace

int wmain(int argc,wchar_t** argv){
    if(argc!=5)return 2;
    const std::filesystem::path dir=argv[1];const std::wstring backend=argv[2],sceneName=argv[3],variant=argv[4];
    const bool fsr=backend==L"fsr";if(!fsr&&backend!=L"dlss")return 2;
    const bool orbit=sceneName==L"orbit";if(!orbit&&sceneName!=L"walk")return 2;
    const int mode=variant==L"zero"?0:variant==L"product"?1:variant==L"ideal"?2:variant==L"blocky"?3:variant==L"zeroband"?4:variant==L"inverted"?5:variant==L"garbage"?6:variant==L"nrflow"?7:variant==L"nrzero"?8:-1;if(mode<0)return 2;
    std::filesystem::create_directories(dir);CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    const std::string label=std::string(fsr?"fsr":"dlss")+"-"+(orbit?"orbit":"walk")+"-"+std::filesystem::path(variant).string();
    const Scene scene=makeScene(orbit);
    gfx::D3D12DeviceContext ctx;gfx::CommandSlotRing ring;Status st=Status::Ok;gfx::DeviceContextDesc dd;dd.enableDebugLayer=true;
    if(!ctx.initialize(dd,st)||!ring.initialize(ctx.device(),ctx.directQueue(),ctx.fence(),ctx.fenceEvent(),6,st))return 2;
    pipeline::EnhanceGraph graph(ctx,ring);pipeline::EnhanceGraphDesc gd;
    gd.sourceWidth=gd.workWidth=W;gd.sourceHeight=gd.workHeight=H;gd.flowWidth=FlowW;gd.flowHeight=FlowH;gd.rgbInput=true;
    gd.enableNr=false;gd.enableSr=false;gd.enableFg=true;gd.fgMultiplier=2;
    gd.frameGenerationBackend=fsr?engine::FrameGenerationBackend::Fsr:engine::FrameGenerationBackend::Dlss;
    gd.fgMotion=mode==1?engine::MotionSource::OpticalFlow:engine::MotionSource::Zero;
    if(mode>=7){gd.enableNr=true;gd.nrWidth=FlowW;gd.nrHeight=FlowH;gd.nrMotion=mode==7?engine::MotionSource::OpticalFlow:engine::MotionSource::Zero;gd.enableNvofStandalone=mode==7;}
    const bool probe=mode!=1&&mode<7;
    // EnhanceGraph::initNvof only creates flow for in-graph FG when the backend is DLSS;
    // FSR receives flow only when NR/SR request it. Run it here as NR users do.
    if(mode==1)gd.enableNvofStandalone=true;
    gd.runtimeAbsPath=runtime::localRuntimeDirectory().wstring();
    pipeline::ComPtr<ID3D12Resource> motion,upload;
    if(probe){
        motion=pipeline::makeTexture(ctx.device(),W,H,DXGI_FORMAT_R16G16_FLOAT,false);upload=pipeline::makeUploadBuffer(ctx.device(),uint64_t(W)*H*4);
        if(!motion||!upload)return 2;gd.fgMotionProbe=motion.Get();
    }
    if(!graph.initialize(gd)||!graph.createViews()){std::cerr<<"graph initialize failed"<<std::endl;return 2;}
    AVFrame* f=av_frame_alloc();if(!f)return 2;
    f->width=W;f->height=H;f->format=AV_PIX_FMT_RGBA;f->color_range=AVCOL_RANGE_JPEG;f->color_trc=AVCOL_TRC_IEC61966_2_1;f->colorspace=AVCOL_SPC_RGB;
    if(av_frame_get_buffer(f,32)<0)return 2;
    std::vector<uint16_t> halves(size_t(W)*H*2);
    Accumulator band,outside,inner,full,real,realBand,realOutside,wobbleEdge,wobbleDeep;
    sink::RgbaImage previousReal;
    // Orbit only: the character is static on screen, so any change between consecutive
    // displayed frames inside its silhouette is wobble. Edge = up to 64 px inside.
    auto wobble=[&](const sink::RgbaImage& a,const sink::RgbaImage& b){
        if(!orbit||a.pixels.size()!=b.pixels.size())return;
        for(int y=0;y<H;y+=2)for(int x=0;x<W;x+=2){const float d=scene.dist(x,y,0);if(d>-2)continue;
            const auto* p=&a.pixels[(size_t(y)*W+x)*4];const auto* q=&b.pixels[(size_t(y)*W+x)*4];
            (d>=-Band?wobbleEdge:wobbleDeep).add(std::abs(p[0]-q[0]),std::abs(p[1]-q[1]),std::abs(p[2]-q[2]));}
    };unsigned generated=0,evaluated=0;
    // Product flow diagnostics at flow resolution, inside the band.
    uint64_t flowBand=0,flowZeroed=0,flowWrong=0,flowFar=0,flowFarWrong=0;double flowBandEpe=0;
    bool ok=true;
    for(int k=0;k<N&&ok;++k){
        if(probe){
            for(int y=0;y<H;++y)for(int x=0;x<W;++x){
                std::array<float,2> v{0,0};
                if(k>0&&mode>=2){
                    if(mode==3){const int bx=x/Block*Block+Block/2,by=y/Block*Block+Block/2;v=scene.ideal(std::min(bx,W-1),std::min(by,H-1),k);}
                    else v=scene.ideal(x,y,k);
                    if(mode==4&&(std::abs(scene.dist(x,y,scene.chOffset(k)))<=Block||scene.disoccluded(x,y,k)))v={0,0};
                    if(mode==5)v={-v[0],-v[1]};
                    if(mode==6)v={120,0};
                }
                halves[(size_t(y)*W+x)*2]=DirectX::PackedVector::XMConvertFloatToHalf(v[0]);
                halves[(size_t(y)*W+x)*2+1]=DirectX::PackedVector::XMConvertFloatToHalf(v[1]);
            }
            if(!uploadMotion(ctx,ring,motion.Get(),upload.Get(),halves,k==0,fsr)){ok=false;break;}
        }
        for(int y=0;y<H;++y){auto* row=f->data[0]+size_t(y)*f->linesize[0];for(int x=0;x<W;++x){const auto p=scene.pixel(x,y,k);row[x*4]=p[0];row[x*4+1]=p[1];row[x*4+2]=p[2];row[x*4+3]=255;}}
        pipeline::EnhanceGraph::FrameOutputs out;sink::RgbaImage shown;
        ok=graph.process(f,k*(1000.0/30),k==0,out,uint64_t(k+1))&&ring.waitIdle()&&graph.resolveGeneration(out)&&
           sink::readRgba8(ctx,ring,graph.videoFrameResource(out.videoSlot),shown);
        if(!ok)break;
        if(k>=Warmup)for(int y=0;y<H;y+=2)for(int x=0;x<W;x+=2){const auto p=scene.pixel(x,y,k);const auto* q=&shown.pixels[(size_t(y)*W+x)*4];
            const int r=std::abs(q[0]-p[0]),g=std::abs(q[1]-p[1]),b=std::abs(q[2]-p[2]);real.add(r,g,b);
            if(std::min(std::abs(scene.dist(x,y,scene.chOffset(k))),std::abs(scene.dist(x,y,scene.chOffset(k-1))))<=Band)realBand.add(r,g,b);else realOutside.add(r,g,b);}
        if(k==N-1&&(mode==0||mode>=7)){
            std::vector<std::array<uint8_t,3>> rgb(size_t(W)*H);for(size_t i=0;i<rgb.size();++i)rgb[i]={shown.pixels[i*4],shown.pixels[i*4+1],shown.pixels[i*4+2]};
            const int o=scene.chOffset(k);ok=save(dir/(label+"-left-real.png"),crop(rgb,1920+o-250,1050,512))&&save(dir/(label+"-right-real.png"),crop(rgb,1920+o+250,1050,512));
            if(!ok)break;}
        if(mode==1&&k>=Warmup){
            std::vector<float> flow;auto* tex=graph.flowResource();
            if(!tex||!readFlow(ctx,ring,tex,graph.diagnosticResourceState(tex),flow)){ok=false;break;}
            const auto fd=tex->GetDesc();const float scale=float(W)/float(fd.Width);
            for(UINT y=0;y<fd.Height;++y)for(UINT x=0;x<fd.Width;++x){
                const int sx=std::min(int((x+.5f)*scale),W-1),sy=std::min(int((y+.5f)*scale),H-1);
                const auto ideal=scene.ideal(sx,sy,k);const float ix=ideal[0]/scale,iy=ideal[1]/scale;
                const float vx=flow[(size_t(y)*fd.Width+x)*2],vy=flow[(size_t(y)*fd.Width+x)*2+1];
                const float epe=std::hypot(vx-ix,vy-iy);
                const bool inBand=std::min(std::abs(scene.dist(sx,sy,scene.chOffset(k))),std::abs(scene.dist(sx,sy,scene.chOffset(k-1))))<=Band;
                if(inBand){++flowBand;flowBandEpe+=epe;flowWrong+=epe>2;flowZeroed+=std::hypot(vx,vy)<1&&std::hypot(ix,iy)>=5;}
                else{++flowFar;flowFarWrong+=epe>2;}
            }
        }
        if(k==0){previousReal=shown;continue;}
        for(unsigned i=0;i<out.batch.count;++i){
            const auto& item=out.batch.frames[i];
            if(item.kind!=pipeline::FrameKind::Generated)continue;
            ++generated;
            if(item.validity!=pipeline::GenerationValidity::Valid||k<Warmup)continue;
            sink::RgbaImage gen;if(!sink::readRgba8(ctx,ring,graph.generatedFrameResource(item.lease->slot),gen)){ok=false;break;}
            if(k>Warmup){wobble(previousReal,gen);wobble(gen,shown);}
            ++evaluated;const double mid=k-.5;const int oG=scene.chOffset(mid),oA=scene.chOffset(k-1),oB=scene.chOffset(k);
            const bool keep=k==N-1;std::vector<std::array<uint8_t,3>> gtImage,genImage,errImage;
            if(keep){gtImage.resize(size_t(W)*H);genImage.resize(size_t(W)*H);errImage.resize(size_t(W)*H);}
            for(int y=0;y<H;++y)for(int x=0;x<W;++x){
                const auto t=scene.pixel(x,y,mid);const auto* q=&gen.pixels[(size_t(y)*W+x)*4];
                const int r=std::abs(q[0]-t[0]),g=std::abs(q[1]-t[1]),b=std::abs(q[2]-t[2]);
                const float nearest=std::min({std::abs(scene.dist(x,y,oG)),std::abs(scene.dist(x,y,oA)),std::abs(scene.dist(x,y,oB))});
                full.add(r,g,b);
                if(nearest<=Band)band.add(r,g,b);else if(scene.covered(x,y,oG))inner.add(r,g,b);else outside.add(r,g,b);
                if(keep){gtImage[size_t(y)*W+x]=t;genImage[size_t(y)*W+x]={q[0],q[1],q[2]};const uint8_t e=uint8_t(std::min(255,(r+g+b)*4/3));errImage[size_t(y)*W+x]={e,e,e};}
            }
            if(keep){
                // Left/right torso edges and the head, around the ground-truth silhouette.
                const std::array<std::array<int,2>,3> centers{{{1920+oG-250,1050},{1920+oG+250,1050},{1920+oG,560}}};
                const char* names[]={"left","right","head"};
                for(size_t c=0;c<centers.size()&&ok;++c){
                    const std::string stem=label+"-"+names[c];
                    ok=save(dir/(stem+"-gen.png"),crop(genImage,centers[c][0],centers[c][1],512))&&
                       save(dir/(stem+"-err.png"),crop(errImage,centers[c][0],centers[c][1],512));
                    if(ok&&mode==0)ok=save(dir/(std::string(fsr?"fsr":"dlss")+"-"+(orbit?"orbit":"walk")+"-"+names[c]+"-gt.png"),crop(gtImage,centers[c][0],centers[c][1],512));
                }
            }
        }
        previousReal=std::move(shown);
    }
    av_frame_free(&f);
    const auto m=graph.metrics();
    ok=ring.drainQueue()&&ok;graph.shutdown();
    pipeline::ComPtr<ID3D12InfoQueue> iq;unsigned errors=0;
    if(SUCCEEDED(ctx.device()->QueryInterface(IID_PPV_ARGS(&iq))))
        for(UINT64 i=0;i<iq->GetNumStoredMessages();++i){SIZE_T bytes=0;if(FAILED(iq->GetMessage(i,nullptr,&bytes)))continue;std::vector<uint8_t>b(bytes);auto* msg=reinterpret_cast<D3D12_MESSAGE*>(b.data());
            if(SUCCEEDED(iq->GetMessage(i,msg,&bytes))&&msg->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++errors;std::cerr<<msg->pDescription<<std::endl;}}
    const bool complete=evaluated==unsigned(N-Warmup);
    std::cout<<std::format("FG_MOTION_BOUNDARY {{\"label\":\"{}\",\"generated\":{},\"evaluated\":{},\"disabled\":{},\"nvof\":{},\"bandMae\":{:.4f},\"bandBadPct\":{:.4f},\"bandP95\":{},"
        "\"farMae\":{:.4f},\"farBadPct\":{:.4f},\"innerMae\":{:.4f},\"fullMae\":{:.4f},\"realMae\":{:.4f},\"realBandMae\":{:.4f},\"realOutsideMae\":{:.4f},\"wobbleEdge\":{:.4f},\"wobbleEdgeBadPct\":{:.4f},\"wobbleDeep\":{:.4f},\"flowBandEpe\":{:.4f},\"flowBandWrongPct\":{:.3f},\"flowBandZeroedPct\":{:.3f},\"flowFarWrongPct\":{:.3f},\"errors\":{},\"pass\":{}}}",
        label,generated,evaluated,m.fgDisabledFrames,m.nvofExecuteCount,band.mae(),band.badPercent(),band.p95(),outside.mae(),outside.badPercent(),inner.mae(),full.mae(),real.mae(),realBand.mae(),realOutside.mae(),wobbleEdge.mae(),wobbleEdge.badPercent(),wobbleDeep.mae(),
        flowBand?flowBandEpe/flowBand:-1.0,flowBand?100.0*flowWrong/flowBand:-1.0,flowBand?100.0*flowZeroed/flowBand:-1.0,flowFar?100.0*flowFarWrong/flowFar:-1.0,
        errors,ok&&complete&&!errors?"true":"false")<<std::endl;
    ring.shutdown();ctx.shutdown();CoUninitialize();
    return ok&&complete&&!errors?0:1;
}
