// Log50 timestamps replayed through the actual graph and native presenter.
// Test-only synchronous GPU waits/readback are not playback code or FPS data.
#include "veyra/engine/VideoPresenter.h"
#include "veyra/pipeline/EnhanceGraph.h"
#include "veyra/gfx/D3D12DeviceContext.h"
#include "veyra/gfx/CommandSlotRing.h"
#include "veyra/sink/ImageExportSink.h"
#include "veyra/RuntimePaths.h"
#include "veyra/Log.h"
#include <d3d12sdklayers.h>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

int wmain(int argc, wchar_t** argv) {
    using namespace veyra;
    if (argc != 2 || FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 2;
    const std::filesystem::path output = argv[1];
    std::filesystem::create_directories(output);
    Logger::instance().openFile((output / L"engine.log").wstring());
    Logger::instance().setConsoleEnabled(false);
    gfx::D3D12DeviceContext ctx;
    gfx::CommandSlotRing ring;
    gfx::DeviceContextDesc device;
    device.enableDebugLayer = true;
    Status status;
    if (!ctx.initialize(device, status) ||
        !ring.initialize(ctx.device(), ctx.directQueue(), ctx.fence(), ctx.fenceEvent(), 6, status)) return 2;
    gfx::ComPtr<ID3D12InfoQueue> messages;
    ctx.device()->QueryInterface(IID_PPV_ARGS(&messages));
    if (!messages) return 2;
    messages->ClearStoredMessages();
    HWND window = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Capture cadence replay",
        WS_POPUP, 30, 30, 640, 360, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!window) return 2;
    engine::VideoPresenter presenter;
    gfx::ComPtr<IDXGISwapChain3> held;
    AVFrame* frame = av_frame_alloc();
    frame->format = AV_PIX_FMT_P010LE;
    frame->width = 640; frame->height = 360;
    frame->color_range = AVCOL_RANGE_MPEG;
    frame->colorspace = AVCOL_SPC_BT709;
    frame->color_primaries = AVCOL_PRI_BT709;
    frame->color_trc = AVCOL_TRC_BT709;
    bool ok = av_frame_get_buffer(frame, 32) >= 0;
    for (unsigned round = 0; round < 4 && ok; ++round) {
        ok = presenter.prepareGraphChange() && ring.drainQueue();
        pipeline::EnhanceGraph graph(ctx, ring);
        pipeline::EnhanceGraphDesc desc;
        desc.sourceWidth = desc.workWidth = 640;
        desc.sourceHeight = desc.workHeight = 360;
        desc.enableNr = desc.enableSr = false; desc.enableFg = true;
        desc.captureBitDepth = 10; desc.fgMultiplier = 2;
        desc.frameGenerationBackend = round % 2 ? engine::FrameGenerationBackend::Vfg : engine::FrameGenerationBackend::Dlss;
        desc.runtimeAbsPath = runtime::localRuntimeDirectory().wstring();
        ok = ok && graph.initialize(desc) && graph.createViews() && presenter.open(ctx, window, graph);
        if (!held) held = presenter.swapChainForTest();
        const bool retained = held && held.Get() == presenter.swapChainForTest().Get();
        ok = ok && retained;
        const uint64_t intervalUs = round < 2 ? 20833 : 16683;
        uint64_t ptsUs = 0;
        unsigned cadenceBreaks = 0, generated = 0, anomalyPairs = 0;
        bool originalPts = true, orderedSubframes = true;
        unsigned lastSlot = 0; pipeline::FrameIdentity lastIdentity;
        for (unsigned i = 0; i < 224 && ok; ++i) {
            if (i) ptsUs += i % 101 == 0 ? 1000 : intervalUs;
            for (int y = 0; y < 360; ++y) for (int x = 0; x < 640; ++x) {
                const auto luma = 40 + (((unsigned(x) + i * 2) / 8 * 7 + unsigned(y) / 8 * 11) % 160);
                reinterpret_cast<uint16_t*>(frame->data[0] + size_t(y) * frame->linesize[0])[x] = uint16_t((64 + luma * 3) << 6);
            }
            for (int y = 0; y < 180; ++y) for (int x = 0; x < 640; ++x)
                reinterpret_cast<uint16_t*>(frame->data[1] + size_t(y) * frame->linesize[1])[x] = 512 << 6;
            pipeline::EnhanceGraph::FrameOutputs out;
            ok = graph.process(frame, double(ptsUs) / 1000, i == 0, out, i + 1) &&
                 ring.waitIdle() && graph.resolveGeneration(out);
            cadenceBreaks += out.detectedReset == pipeline::ResetReason::CadenceBreak;
            originalPts &= out.batch.b100ns == int64_t(ptsUs * 10);
            if (out.hasGenerated) {
                ++generated;
                if (i > 1 && (i % 101 == 0 || i % 101 == 1)) ++anomalyPairs;
            }
            for (unsigned n = 0; n < out.batch.count; ++n) {
                const auto& item = out.batch.frames[n];
                if (item.kind == pipeline::FrameKind::Generated && item.validity == pipeline::GenerationValidity::Valid)
                    orderedSubframes &= item.pts100ns > out.batch.a100ns && item.pts100ns < out.batch.b100ns;
            }
            ok = ok && presenter.present(ctx, ring, graph, out.videoSlot, false, false, 0, false, .5f, out.batch.identity);
            lastSlot = out.videoSlot; lastIdentity = out.batch.identity;
        }
        sink::RgbaImage image;
        ok = ok && presenter.readPresentedFrameForTest(ctx, ring, image);
        bool visible = false;
        if (ok) for (size_t p = 0; p < image.pixels.size(); p += 4) visible |= image.pixels[p] > 30;
        if (round == 0 && ok) {
            // Force the field ResizeBuffers ownership error using an actual
            // external backbuffer reference, then release it and recover.
            gfx::ComPtr<ID3D12Resource> externalBuffer;
            ok = SUCCEEDED(held->GetBuffer(0, IID_PPV_ARGS(&externalBuffer)));
            SetWindowPos(window, nullptr, 0, 0, 800, 450, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            ok = ok && presenter.present(ctx, ring, graph, lastSlot, false, false, 0, false, .5f, lastIdentity);
            DXGI_SWAP_CHAIN_DESC1 size{}; held->GetDesc1(&size);
            const bool keptOldBuffers = size.Width == 640 && size.Height == 360;
            externalBuffer.Reset();
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            ok = ok && presenter.present(ctx, ring, graph, lastSlot, false, false, 0, false, .5f, lastIdentity);
            held->GetDesc1(&size);
            const bool resized = size.Width == 800 && size.Height == 450;
            ok = ok && keptOldBuffers && resized && presenter.readPresentedFrameForTest(ctx, ring, image);
            std::cout << "CAPTURE_RESIZE keptOldBuffers=" << keptOldBuffers << " recovered=" << resized
                      << " passed=" << ok << std::endl;
        }
        // A real 200ms gap must still reset even after the anomaly pattern.
        pipeline::EnhanceGraph::FrameOutputs gap;
        ok = ok && graph.process(frame, double(ptsUs + 200000) / 1000, false, gap, 225) && ring.waitIdle();
        const bool realGap = gap.detectedReset == pipeline::ResetReason::CadenceBreak && gap.historyReset;
        gap = {};
        ok = ok && cadenceBreaks == 0 && generated >= 218 && anomalyPairs == 4 &&
             originalPts && orderedSubframes && visible && realGap;
        std::cout << "CAPTURE_CADENCE backend=" << (round % 2 ? "VFG" : "DLSS")
            << " intervalUs=" << intervalUs << " frames=224 cadenceBreaks=" << cadenceBreaks
            << " generated=" << generated << " anomalyPairs=" << anomalyPairs
            << " originalPts=" << originalPts << " orderedSubframes=" << orderedSubframes
            << " retainedExternalChain=" << retained << " visible=" << visible
            << " realGap=" << realGap << " passed=" << ok << std::endl;
        ok = presenter.prepareGraphChange() && ring.drainQueue() && ok;
        graph.shutdown();
    }
    held.Reset(); presenter.close(); av_frame_free(&frame);
    unsigned errors = 0;
    for (UINT64 i = 0; i < messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size = 0; messages->GetMessage(i, nullptr, &size);
        std::vector<char> buffer(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
        messages->GetMessage(i, message, &size);
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) { ++errors; std::cerr << message->pDescription << std::endl; }
    }
    ok = ok && errors == 0 && SUCCEEDED(ctx.device()->GetDeviceRemovedReason());
    messages.Reset(); ring.shutdown(); ctx.shutdown(); DestroyWindow(window); CoUninitialize();
    std::cout << "CAPTURE_CADENCE_RESULT passed=" << ok << " debugErrors=" << errors
              << " physicalCapture=unmeasured displayFps=unmeasured" << std::endl;
    return ok ? 0 : 1;
}
