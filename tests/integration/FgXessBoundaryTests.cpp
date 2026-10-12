// XeSS counterpart of FgMotionBoundaryTests (J0 for the "jelly" field report, 2026-10-12).
// XeSS generates and presents its frames inside the provider swapchain, so this test drives
// the product EnhanceGraph + VideoPresenter in a borderless full-screen window, asks the
// provider to present only interpolated frames (VEYRA_TEST_XESS_ONLY_INTERPOLATED, test-only
// switch in XessPresenter) and scores what reaches the screen through DXGI desktop duplication.
// Scene: a side-running third-person character (static torso/head, swinging limbs) over a
// background that pans D px per 30 fps source frame (the camera follows the character).
// Motion inputs the provider receives:
//   zero     - the product default for XeSS (motion source Automatic -> zero motion)
//   product  - the product flow (motion source OpticalFlow), flow at half resolution
//   ideal    - exact current->previous motion, limb rotation included (fgMotionProbe)
//   inverted - ideal with the opposite sign (motion-sign convention control)
//   idealdepth / productdepth - ideal or product motion plus a layered depth (character
//              near, front limbs nearest, background far) instead of the constant depth
// usage: veyra_fg_xess_boundary_tests <outDir> <run|orbit> <speedPx> <variant> [tag]
// Product-flow knobs for experiments (environment): VEYRA_TEST_FLOW_FULL=1 (flow at full
// resolution), VEYRA_TEST_FLOW_QUALITY=0|1|2, VEYRA_TEST_FLOW_VALIDATE=0, VEYRA_TEST_FLOW_BACKEND=
// dis|amd, VEYRA_TEST_NVOF_GRID=1|2|4 (read by EnhanceGraph).
#include "veyra/engine/VideoPresenter.h"
#include "veyra/pipeline/EnhanceGraph.h"
#include "veyra/pipeline/GpuPassUtils.h"
#include "veyra/gfx/D3D12DeviceContext.h"
#include "veyra/gfx/CommandSlotRing.h"
#include "veyra/sink/ImageExportSink.h"
#include "veyra/RuntimePaths.h"
#include <d3d11.h>
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <DirectXPackedVector.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <iostream>
#include <mutex>
#include <numbers>
#include <string>
#include <thread>
#include <vector>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}
using namespace veyra;
using Microsoft::WRL::ComPtr;
namespace {
constexpr int W=2560,H=1440,FlowW=1280,FlowH=720,N=16,Warmup=4,Band=40,X0=480,CW=1600;

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
template<class F> void parallelRows(int rows,F&& f){
    const int n=std::max(1u,std::thread::hardware_concurrency());std::vector<std::jthread> pool;
    for(int t=0;t<n;++t)pool.emplace_back([&,t]{for(int y=t;y<rows;y+=n)f(y);});
}
// Parts in draw order (back to front). Limbs rotate about a pivot; angle 0 points down,
// positive angles swing the far end towards +x. Static parts use world texture coordinates.
struct Part{float px,py,len,r;int kind;float swing;bool limb;};
constexpr Part kParts[]={
    {1280,560,300,45,3,+.8f,true},   // back arm
    {1280,900,400,58,2,-1.f,true},   // back leg
    {1280,520,340,150,1,0,false},    // torso: capsule from (1280,520) downwards
    {1280,380,0,80,0,0,false},       // head
    {1280,900,400,58,2,+1.f,true},   // front leg
    {1280,560,300,45,3,-.8f,true}};  // front arm
constexpr std::array<std::array<float,3>,4> kBase{{{205,160,130},{48,66,118},{46,46,52},{52,70,124}}};
struct Scene {
    bool run=true;int speed=0;std::vector<std::array<uint8_t,3>> background;int bw=0;
    float angle(const Part& p,double t)const{
        if(!p.limb||!run)return 0;
        return float(p.swing*35.0*std::numbers::pi/180.0*std::sin(2*std::numbers::pi*t/8.0));
    }
    int bgOffset(double t)const{return int(std::lround(t*speed));}
    // Local capsule coordinates of a world point for part i at time t.
    void local(const Part& p,double t,float x,float y,float& u,float& v)const{
        const float a=angle(p,t),dx=std::sin(a),dy=std::cos(a);const float rx=x-p.px,ry=y-p.py;
        u=rx*dx+ry*dy;v=rx*dy-ry*dx;
    }
    float sdf(const Part& p,double t,float x,float y)const{
        float u,v;local(p,t,x,y,u,v);
        if(u<0)return std::hypot(u,v)-p.r;if(u>p.len)return std::hypot(u-p.len,v)-p.r;return std::abs(v)-p.r;
    }
    std::array<float,3> partColor(const Part& p,double t,float x,float y,int index)const{
        float u,v;if(p.limb)local(p,t,x,y,u,v);else{u=y;v=x;}
        const float n=.7f+.6f*noise(u+300.f*index,v+500.f*index,7,21+index);
        const float stripe=(int(std::floor(u/18))&1)?16.f:0.f;
        const auto& c=kBase[size_t(p.kind)];
        return {c[0]*n+stripe,c[1]*n+stripe,c[2]*n+stripe};
    }
    // Composite colour, topmost part with >= 50% coverage (-1 background) and nearest part edge.
    std::array<uint8_t,3> pixel(int x,int y,double t,int* top=nullptr,float* edge=nullptr)const{
        const auto& b=background[size_t(y)*bw+x+bgOffset(t)];
        float c[3]={float(b[0]),float(b[1]),float(b[2])};int id=-1;float nearest=1e9f;
        for(int i=0;i<6;++i){
            const auto& p=kParts[i];const float d=sdf(p,t,x+.5f,y+.5f);nearest=std::min(nearest,std::abs(d));
            const float a=std::clamp(.5f-d,0.f,1.f);if(a<=0)continue;
            const auto pc=partColor(p,t,x+.5f,y+.5f,i);
            for(int k=0;k<3;++k)c[k]=c[k]*(1-a)+std::clamp(pc[k],0.f,255.f)*a;
            if(a>=.5f)id=i;
        }
        if(top)*top=id;if(edge)*edge=nearest;
        return {uint8_t(std::lround(c[0])),uint8_t(std::lround(c[1])),uint8_t(std::lround(c[2]))};
    }
    // Exact current(frame)->previous(frame-1) motion in pixels.
    std::array<float,2> ideal(int x,int y,int frame,int top)const{
        if(top<0)return {float(speed),0.f};
        const auto& p=kParts[top];if(!p.limb||!run)return {0.f,0.f};
        float u,v;local(p,frame,x+.5f,y+.5f,u,v);
        const float a=angle(p,frame-1),dx=std::sin(a),dy=std::cos(a);
        const float px=p.px+u*dx+v*dy,py=p.py+u*dy-v*dx;
        return {px-(x+.5f),py-(y+.5f)};
    }
};
Scene makeScene(bool run,int speed){
    Scene s;s.run=run;s.speed=speed;s.bw=W+(N+2)*speed;s.background.resize(size_t(s.bw)*H);
    parallelRows(H,[&](int y){
        for(int x=0;x<s.bw;++x){
            const float n=.5f*noise(float(x),float(y),80,1)+.3f*noise(float(x),float(y),20,2)+.2f*noise(float(x),float(y),5,3);
            const float t=float(y)/H;float r=110*(1-t)+86*t,g=138*(1-t)+112*t,b=168*(1-t)+70*t;
            const float m=.55f+.9f*n;r*=m;g*=m;b*=m;
            if(x%211<13){r*=.35f;g*=.32f;b*=.3f;}
            if((y/120)%3==1&&x%67<4){r=std::min(255.f,r+60);g=std::min(255.f,g+55);b=std::min(255.f,b+40);}
            s.background[size_t(y)*s.bw+x]={uint8_t(std::clamp(r,0.f,255.f)),uint8_t(std::clamp(g,0.f,255.f)),uint8_t(std::clamp(b,0.f,255.f))};
        }
    });
    return s;
}
struct Image{std::vector<std::array<uint8_t,3>> rgb;}; // CW x H crop at X0
struct Accumulator {
    double sum=0;uint64_t samples=0,bad=0;
    void add(int r,int g,int b){sum+=r+g+b;samples+=3;bad+=std::max({r,g,b})>32;}
    double mae()const{return samples?sum/samples:-1;}
    double badPercent()const{return samples?100.0*bad/(samples/3):-1;}
};
bool save(const std::filesystem::path& path,const Image& im,int cx,int cy,int size,bool error=false,const Image* other=nullptr){
    sink::RgbaImage out;out.width=out.height=uint32_t(size);out.pixels.resize(size_t(size)*size*4);
    const int x0=std::clamp(cx-size/2-X0,0,CW-size),y0=std::clamp(cy-size/2,0,H-size);
    for(int y=0;y<size;++y)for(int x=0;x<size;++x){
        const auto& p=im.rgb[size_t(y0+y)*CW+x0+x];auto* q=&out.pixels[(size_t(y)*size+x)*4];
        if(error&&other){const auto& o=other->rgb[size_t(y0+y)*CW+x0+x];const int e=std::min(255,(std::abs(p[0]-o[0])+std::abs(p[1]-o[1])+std::abs(p[2]-o[2]))*4/3);q[0]=q[1]=q[2]=uint8_t(e);}
        else{q[0]=p[0];q[1]=p[1];q[2]=p[2];}
        q[3]=255;
    }
    std::error_code ec;std::filesystem::remove(path,ec);return sink::saveImage(path.wstring(),out);
}
LRESULT CALLBACK proc(HWND h,UINT m,WPARAM w,LPARAM l){return DefWindowProcW(h,m,w,l);}
// SDR content on an HDR (advanced colour) desktop is composed as scRGB with the user's SDR
// white level (1000 = 80 nits = scRGB 1.0). Returns that scale for the monitor, 0 if unknown.
float sdrWhiteScale(HMONITOR monitor){
    MONITORINFOEXW info{};info.cbSize=sizeof(info);if(!GetMonitorInfoW(monitor,&info))return 0;
    UINT32 paths=0,modes=0;if(GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS,&paths,&modes)!=ERROR_SUCCESS)return 0;
    std::vector<DISPLAYCONFIG_PATH_INFO> path(paths);std::vector<DISPLAYCONFIG_MODE_INFO> mode(modes);
    if(QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS,&paths,path.data(),&modes,mode.data(),nullptr)!=ERROR_SUCCESS)return 0;
    for(UINT32 i=0;i<paths;++i){
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};source.header.type=DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;source.header.size=sizeof(source);
        source.header.adapterId=path[i].sourceInfo.adapterId;source.header.id=path[i].sourceInfo.id;
        if(DisplayConfigGetDeviceInfo(&source.header)!=ERROR_SUCCESS||std::wstring_view(source.viewGdiDeviceName)!=std::wstring_view(info.szDevice))continue;
        DISPLAYCONFIG_SDR_WHITE_LEVEL white{};white.header.type=DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;white.header.size=sizeof(white);
        white.header.adapterId=path[i].targetInfo.adapterId;white.header.id=path[i].targetInfo.id;
        if(DisplayConfigGetDeviceInfo(&white.header)==ERROR_SUCCESS)return float(white.SDRWhiteLevel)/1000.f;
    }
    return 0;
}
uint8_t encodeSrgb(float linear){
    const float c=std::clamp(linear,0.f,1.f);const float v=c<=.0031308f?12.92f*c:1.055f*std::pow(c,1/2.4f)-.055f;
    return uint8_t(std::lround(std::clamp(v,0.f,1.f)*255));
}
} // namespace

int wmain(int argc,wchar_t** argv){
    if(argc!=5&&argc!=6)return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const std::filesystem::path dir=argv[1];const std::wstring sceneName=argv[2],variant=argv[4];const int speed=_wtoi(argv[3]);
    const bool run=sceneName==L"run";if(!run&&sceneName!=L"orbit")return 2;if(speed<1||speed>200)return 2;
    const int mode=variant==L"zero"?0:variant==L"product"?1:variant==L"ideal"?2:variant==L"inverted"?3:variant==L"idealdepth"?4:variant==L"productdepth"?5:-1;if(mode<0)return 2;
    std::filesystem::create_directories(dir);CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    const std::string label=std::format("xess-{}{}-{}{}",run?"run":"orbit",speed,std::filesystem::path(variant).string(),argc==6?"-"+std::filesystem::path(argv[5]).string():std::string());
    SetEnvironmentVariableW(L"VEYRA_TEST_XESS_ONLY_INTERPOLATED",L"1");
    const HMONITOR monitor=MonitorFromPoint({0,0},MONITOR_DEFAULTTOPRIMARY);MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(monitor,&mi);
    if(mi.rcMonitor.right-mi.rcMonitor.left!=W||mi.rcMonitor.bottom-mi.rcMonitor.top!=H){std::cerr<<"primary monitor must be 2560x1440"<<std::endl;return 2;}
    WNDCLASSW wc{};wc.lpfnWndProc=proc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"VeyraXessBoundary";wc.hCursor=nullptr;RegisterClassW(&wc);
    HWND window=CreateWindowExW(WS_EX_TOPMOST,wc.lpszClassName,L"XeSS boundary test",WS_POPUP,mi.rcMonitor.left,mi.rcMonitor.top,W,H,nullptr,nullptr,wc.hInstance,nullptr);
    if(!window)return 2;ShowWindow(window,SW_SHOW);SetForegroundWindow(window);UpdateWindow(window);
    auto pump=[]{MSG m;while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}};
    pump();
    const Scene scene=makeScene(run,speed);
    gfx::D3D12DeviceContext ctx;gfx::CommandSlotRing ring;Status st=Status::Ok;gfx::DeviceContextDesc dd;dd.enableDebugLayer=true;
    if(!ctx.initialize(dd,st)||!ring.initialize(ctx.device(),ctx.directQueue(),ctx.fence(),ctx.fenceEvent(),6,st))return 2;
    pipeline::EnhanceGraph graph(ctx,ring);pipeline::EnhanceGraphDesc gd;
    gd.sourceWidth=gd.workWidth=W;gd.sourceHeight=gd.workHeight=H;gd.flowWidth=FlowW;gd.flowHeight=FlowH;gd.rgbInput=true;
    gd.enableNr=false;gd.enableSr=false;gd.enableFg=true;gd.fgMultiplier=2;gd.frameGenerationBackend=engine::FrameGenerationBackend::XeSS;
    gd.fgMotion=mode==1||mode==5?engine::MotionSource::OpticalFlow:engine::MotionSource::Zero;
    auto knob=[](const wchar_t* name)->std::wstring{wchar_t v[16]{};return GetEnvironmentVariableW(name,v,16)?std::wstring(v):std::wstring();};
    if(knob(L"VEYRA_TEST_FLOW_FULL")==L"1"){gd.flowWidth=W;gd.flowHeight=H;}
    if(const auto q=knob(L"VEYRA_TEST_FLOW_QUALITY");!q.empty())gd.flowQuality=engine::FlowQuality(std::clamp(_wtoi(q.c_str()),0,2));
    if(knob(L"VEYRA_TEST_FLOW_VALIDATE")==L"0")gd.validateMotion=false;
    if(const auto b=knob(L"VEYRA_TEST_FLOW_BACKEND");b==L"dis")gd.opticalFlowBackend=engine::OpticalFlowBackend::GpuDis;else if(b==L"amd")gd.opticalFlowBackend=engine::OpticalFlowBackend::AmdFidelityFx;gd.runtimeAbsPath=runtime::localRuntimeDirectory().wstring();
    ComPtr<ID3D12Resource> motion,upload,depthUpload;const bool probe=mode>=2&&mode<=4,layered=mode>=4;
    if(layered&&!(depthUpload=pipeline::makeUploadBuffer(ctx.device(),uint64_t(W)*H*4)))return 2;
    if(probe){
        motion=pipeline::makeTexture(ctx.device(),W,H,DXGI_FORMAT_R16G16_FLOAT,false);upload=pipeline::makeUploadBuffer(ctx.device(),uint64_t(W)*H*4);
        if(!motion||!upload)return 2;gd.fgMotionProbe=motion.Get();
    }
    engine::VideoPresenter presenter;
    if(!graph.initialize(gd)||!presenter.open(ctx,window,graph)||!graph.createViews()){std::cerr<<"graph/presenter open failed"<<std::endl;return 2;}
    if(!presenter.xessActive()){std::cerr<<"XeSS presenter not active"<<std::endl;return 2;}
    {engine::PresentationSettings requested;std::wstring text;presenter.configurePresentation(ctx,requested,true,text);}
    // Desktop duplication of the primary output (the window covers it exactly).
    ComPtr<IDXGIFactory1> factory;if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))return 2;
    ComPtr<IDXGIAdapter1> adapter;ComPtr<IDXGIOutput> output;
    for(UINT a=0;!output&&factory->EnumAdapters1(a,&adapter)!=DXGI_ERROR_NOT_FOUND;++a)
        for(UINT o=0;adapter->EnumOutputs(o,&output)!=DXGI_ERROR_NOT_FOUND;++o){DXGI_OUTPUT_DESC od{};output->GetDesc(&od);if(od.Monitor==monitor)break;output.Reset();}
    if(!output)return 2;
    ComPtr<ID3D11Device> device11;ComPtr<ID3D11DeviceContext> context11;
    if(FAILED(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device11,nullptr,&context11)))return 2;
    // HDR desktop: duplicate as scRGB FP16 and undo the SDR white-level mapping (piecewise sRGB).
    ComPtr<IDXGIOutput6> output6;DXGI_OUTPUT_DESC1 desc1{};const bool hdrDesktop=SUCCEEDED(output.As(&output6))&&SUCCEEDED(output6->GetDesc1(&desc1))&&desc1.ColorSpace==DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    const float white=hdrDesktop?sdrWhiteScale(monitor):1.f;if(hdrDesktop&&white<=0){std::cerr<<"SDR white level unavailable"<<std::endl;return 2;}
    const DXGI_FORMAT captureFormat=hdrDesktop?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_B8G8R8A8_UNORM;
    ComPtr<IDXGIOutput5> output5;ComPtr<IDXGIOutputDuplication> duplication;const DXGI_FORMAT formats[]={captureFormat};
    if(FAILED(output.As(&output5))||FAILED(output5->DuplicateOutput1(device11.Get(),0,1,formats,&duplication))){std::cerr<<"DuplicateOutput1 failed"<<std::endl;return 2;}
    // Half-float bit pattern -> SDR 8-bit, white level folded in (a table keeps capture real-time).
    std::vector<uint8_t> decode(65536);for(uint32_t h=0;h<65536;++h)decode[h]=encodeSrgb(DirectX::PackedVector::XMConvertHalfToFloat(uint16_t(h))/white);
    D3D11_TEXTURE2D_DESC sd{};sd.Width=CW;sd.Height=H;sd.MipLevels=1;sd.ArraySize=1;sd.Format=captureFormat;sd.SampleDesc.Count=1;sd.Usage=D3D11_USAGE_STAGING;sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;if(FAILED(device11->CreateTexture2D(&sd,nullptr,&staging)))return 2;
    std::vector<Image> captures;std::mutex captureLock;std::atomic<bool> stop=false;std::atomic<unsigned> captureErrors=0;
    unsigned duplicateCaptures=0;
    std::jthread capture([&]{
        while(!stop){
            DXGI_OUTDUPL_FRAME_INFO fi{};ComPtr<IDXGIResource> resource;
            const HRESULT hr=duplication->AcquireNextFrame(20,&fi,&resource);
            if(hr==DXGI_ERROR_WAIT_TIMEOUT)continue;
            if(FAILED(hr)){++captureErrors;break;}
            if(fi.LastPresentTime.QuadPart&&captures.size()<128){
                ComPtr<ID3D11Texture2D> texture;
                if(SUCCEEDED(resource.As(&texture))){
                    D3D11_BOX box{UINT(X0),0,0,UINT(X0+CW),UINT(H),1};context11->CopySubresourceRegion(staging.Get(),0,0,0,0,texture.Get(),0,&box);
                    D3D11_MAPPED_SUBRESOURCE map{};
                    if(SUCCEEDED(context11->Map(staging.Get(),0,D3D11_MAP_READ,0,&map))){
                        Image im;im.rgb.resize(size_t(CW)*H);
                        for(int y=0;y<H;++y){const auto* row=static_cast<const uint8_t*>(map.pData)+size_t(y)*map.RowPitch;
                            if(hdrDesktop){const auto* h=reinterpret_cast<const uint16_t*>(row);
                                for(int x=0;x<CW;++x)im.rgb[size_t(y)*CW+x]={decode[h[x*4]],decode[h[x*4+1]],decode[h[x*4+2]]};}
                            else for(int x=0;x<CW;++x)im.rgb[size_t(y)*CW+x]={row[x*4+2],row[x*4+1],row[x*4]};}
                        context11->Unmap(staging.Get(),0);std::lock_guard lock(captureLock);
                        // DWM can report multiple desktop updates for an unchanged
                        // interpolated image (including the previous test window).
                        // Keep distinct pixels so those updates cannot consume the
                        // bounded capture budget before the later pairs are shown.
                        if(!captures.empty()&&im.rgb==captures.back().rgb)++duplicateCaptures;
                        else captures.push_back(std::move(im));
                    }
                }
            }
            duplication->ReleaseFrame();
        }
    });
    AVFrame* f=av_frame_alloc();if(!f)return 2;
    f->width=W;f->height=H;f->format=AV_PIX_FMT_RGBA;f->color_range=AVCOL_RANGE_JPEG;f->color_trc=AVCOL_TRC_IEC61966_2_1;f->colorspace=AVCOL_SPC_RGB;
    if(av_frame_get_buffer(f,32)<0)return 2;
    std::vector<uint16_t> halves(size_t(W)*H*2);std::vector<int> tops(size_t(W)*H);
    bool ok=true;const auto start=std::chrono::steady_clock::now();
    for(int k=0;k<N&&ok;++k){
        // 10 presents/s so every interpolated frame is captured; the provider still gets
        // 30 fps source timestamps (frameRenderTime), which is what its interpolation uses.
        std::this_thread::sleep_until(start+std::chrono::milliseconds(int64_t(k)*100));pump();
        ok=presenter.beginSourceInput();
        parallelRows(H,[&](int y){auto* row=f->data[0]+size_t(y)*f->linesize[0];for(int x=0;x<W;++x){int top=-1;const auto p=scene.pixel(x,y,k,&top);tops[size_t(y)*W+x]=top;row[x*4]=p[0];row[x*4+1]=p[1];row[x*4+2]=p[2];row[x*4+3]=255;}});
        if(ok&&layered){
            // Depth of this frame (non-inverted: smaller is nearer) into the graph's FG depth texture.
            constexpr float kDepth[]={.35f,.35f,.3f,.3f,.25f,.25f};auto* depth=graph.diagnosticDepthResource(true);
            void* data=nullptr;if(!depth||FAILED(depthUpload->Map(0,nullptr,&data))){ok=false;break;}
            auto* d=static_cast<float*>(data);for(size_t i=0;i<tops.size();++i)d[i]=tops[i]<0?.95f:kDepth[tops[i]];depthUpload->Unmap(0,nullptr);
            unsigned slot=0;auto* list=ring.acquireNext(slot,st);if(!list){ok=false;break;}
            D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={depth,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST};list->ResourceBarrier(1,&b);
            D3D12_TEXTURE_COPY_LOCATION dst{},src{};dst.pResource=depth;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;src.pResource=depthUpload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Footprint={DXGI_FORMAT_R32_FLOAT,UINT(W),UINT(H),1,UINT(W*4)};list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
            std::swap(b.Transition.StateBefore,b.Transition.StateAfter);list->ResourceBarrier(1,&b);
            ok=ring.submitAndSignal(slot)&&ring.waitIdle();
        }
        if(ok&&probe){
            parallelRows(H,[&](int y){for(int x=0;x<W;++x){auto v=k>0?scene.ideal(x,y,k,tops[size_t(y)*W+x]):std::array<float,2>{0,0};if(mode==3){v[0]=-v[0];v[1]=-v[1];}
                halves[(size_t(y)*W+x)*2]=DirectX::PackedVector::XMConvertFloatToHalf(v[0]);halves[(size_t(y)*W+x)*2+1]=DirectX::PackedVector::XMConvertFloatToHalf(v[1]);}});
            void* data=nullptr;if(FAILED(upload->Map(0,nullptr,&data))){ok=false;break;}
            std::memcpy(data,halves.data(),halves.size()*2);upload->Unmap(0,nullptr);
            unsigned slot=0;auto* list=ring.acquireNext(slot,st);if(!list){ok=false;break;}
            D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={motion.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST};list->ResourceBarrier(1,&b);
            D3D12_TEXTURE_COPY_LOCATION dst{},src{};dst.pResource=motion.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Footprint={DXGI_FORMAT_R16G16_FLOAT,UINT(W),UINT(H),1,UINT(W*4)};list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
            std::swap(b.Transition.StateBefore,b.Transition.StateAfter);list->ResourceBarrier(1,&b);
            ok=ring.submitAndSignal(slot)&&ring.waitIdle();
        }
        pipeline::EnhanceGraph::FrameOutputs out;
        ok=ok&&presenter.beginSourceProcessing()&&graph.process(f,k*(1000.0/30),k==0,out,uint64_t(k+1));
        if(!ok)break;
        presenter.sourceProcessed(out.batch.identity);
        ok=presenter.present(ctx,ring,graph,out.videoSlot,false,true,0,false,.5f,out.batch.identity,{},int64_t(k)*333333);
        ok=ok&&ring.waitIdle()&&graph.resolveGeneration(out);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));pump();
    stop=true;capture.join();
    const auto generatedBySdk=presenter.xessGeneratedCount();
    av_frame_free(&f);ok=ring.drainQueue()&&ok;presenter.close();graph.shutdown();
    // Ground truth (crop) for every evaluated pair midpoint and real frame, plus band masks.
    auto render=[&](double t,Image& im,std::vector<float>* edge,std::vector<int8_t>* top){
        im.rgb.resize(size_t(CW)*H);if(edge)edge->resize(size_t(CW)*H);if(top)top->resize(size_t(CW)*H);
        parallelRows(H,[&](int y){for(int x=0;x<CW;++x){float e=0;int id=-1;im.rgb[size_t(y)*CW+x]=scene.pixel(X0+x,y,t,&id,&e);if(edge)(*edge)[size_t(y)*CW+x]=e;if(top)(*top)[size_t(y)*CW+x]=int8_t(id);}});
    };
    std::vector<Image> mids(N),reals(N);std::vector<std::vector<float>> midEdge(N),realEdge(N);std::vector<std::vector<int8_t>> midTop(N);
    for(int k=0;k<N;++k){render(k,reals[k],&realEdge[k],nullptr);if(k>0)render(k-.5,mids[k],&midEdge[k],&midTop[k]);}
    auto distance=[&](const Image& a,const Image& b){double s=0;uint64_t n=0;for(int y=0;y<H;y+=8)for(int x=0;x<CW;x+=8){const auto& p=a.rgb[size_t(y)*CW+x];const auto& q=b.rgb[size_t(y)*CW+x];s+=std::abs(p[0]-q[0])+std::abs(p[1]-q[1])+std::abs(p[2]-q[2]);++n;}return s/(3.0*n);};
    std::vector<int> bestCapture(N,-1);std::vector<double> bestDistance(N,1e9);unsigned matchedReal=0,unmatched=0;std::string matches;
    for(size_t c=0;c<captures.size();++c){
        int pair=-1;double best=1e9;bool real=false;
        for(int k=1;k<N;++k){const double d=distance(captures[c],mids[k]);if(d<best){best=d;pair=k;real=false;}}
        for(int k=0;k<N;++k){const double d=distance(captures[c],reals[k]);if(d<best){best=d;pair=k;real=true;}}
        matches+=std::format("{}\"{}{}:{:.1f}\"",matches.empty()?"":",",real?"r":"m",pair,best);
        if(c==0||c==captures.size()/2||c+1==captures.size())save(dir/std::format("{}-capture{}.png",label,c),captures[c],1280,720,1400);
        if(best>12){++unmatched;continue;}
        if(real){++matchedReal;continue;}
        if(best<bestDistance[pair]){bestDistance[pair]=best;bestCapture[pair]=int(c);}
    }
    Accumulator band,outside,inner;unsigned evaluated=0;int last=-1;std::string distances;
    for(int k=Warmup+1;k<N;++k){
        distances+=std::format("{}{:.2f}",distances.empty()?"":",",bestCapture[k]<0?-1.0:bestDistance[k]);
        if(bestCapture[k]<0)continue;++evaluated;last=k;const auto& got=captures[size_t(bestCapture[k])];
        for(int y=0;y<H;++y)for(int x=0;x<CW;++x){
            const size_t i=size_t(y)*CW+x;const auto& p=got.rgb[i];const auto& q=mids[k].rgb[i];
            const int r=std::abs(p[0]-q[0]),g=std::abs(p[1]-q[1]),b=std::abs(p[2]-q[2]);
            const float e=std::min({midEdge[k][i],realEdge[k][i],realEdge[k-1][i]});
            if(e<=Band)band.add(r,g,b);else(midTop[k][i]>=0?inner:outside).add(r,g,b);
        }
    }
    if(last>0){
        const auto& got=captures[size_t(bestCapture[last])];
        const std::array<std::array<int,2>,4> centers{{{1080,900},{1480,900},{1280,1250},{1280,560}}};const char* names[]={"back","front","legs","arm"};
        for(size_t c=0;c<centers.size()&&ok;++c){
            const std::string stem=label+"-"+names[c];
            ok=save(dir/(stem+"-cap.png"),got,centers[c][0],centers[c][1],448)&&save(dir/(stem+"-err.png"),got,centers[c][0],centers[c][1],448,true,&mids[last]);
            if(ok&&mode==0)ok=save(dir/(std::format("xess-{}{}-{}-gt.png",run?"run":"orbit",speed,names[c])),mids[last],centers[c][0],centers[c][1],448);
        }
    }
    unsigned errors=0;ComPtr<ID3D12InfoQueue> iq;
    if(SUCCEEDED(ctx.device()->QueryInterface(IID_PPV_ARGS(&iq))))
        for(UINT64 i=0;i<iq->GetNumStoredMessages();++i){SIZE_T bytes=0;if(FAILED(iq->GetMessage(i,nullptr,&bytes)))continue;std::vector<uint8_t>b(bytes);auto* msg=reinterpret_cast<D3D12_MESSAGE*>(b.data());
            if(SUCCEEDED(iq->GetMessage(i,msg,&bytes))&&msg->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++errors;std::cerr<<msg->pDescription<<std::endl;}}
    const bool complete=evaluated==unsigned(N-Warmup-1)&&!captureErrors;
    std::cout<<std::format("FG_XESS_BOUNDARY {{\"label\":\"{}\",\"captures\":{},\"duplicatesSkipped\":{},\"matchedReal\":{},\"unmatched\":{},\"evaluated\":{},\"sdkGenerated\":{},\"bandMae\":{:.4f},\"bandBadPct\":{:.4f},"
        "\"outsideMae\":{:.4f},\"outsideBadPct\":{:.4f},\"innerMae\":{:.4f},\"pairDistances\":[{}],\"captureMatches\":[{}],\"hdrDesktop\":{},\"sdrWhite\":{:.3f},\"captureErrors\":{},\"errors\":{},\"pass\":{}}}",
        label,captures.size(),duplicateCaptures,matchedReal,unmatched,evaluated,generatedBySdk,band.mae(),band.badPercent(),outside.mae(),outside.badPercent(),inner.mae(),distances,matches,
        hdrDesktop,white,captureErrors.load(),errors,ok&&complete&&!errors?"true":"false")<<std::endl;
    ring.shutdown();ctx.shutdown();DestroyWindow(window);CoUninitialize();
    return ok&&complete&&!errors?0:1;
}
