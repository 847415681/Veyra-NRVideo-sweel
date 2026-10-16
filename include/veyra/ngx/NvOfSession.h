#pragma once

// Reusable NVOF D3D12 session (extracted from the Phase 5 probe).
// Produces current->previous optical flow as raw SHORT2 (S10.5) at the
// hardware-grid extent, plus SDK cost. Vectors use source-input pixels; densify
// expands the grid, and any working-extent scaling happens downstream.
// Optional paired reverse outputs also produce previous->current from the
// same A/B frames. Synchronization is fence-based:
// the caller signals `inFence`/`inValue` when both input textures are ready;
// NVOF signals `outFence`/nextOutValue when the flow texture is complete.
// The caller must make its D3D12 queue wait on outFence before consuming the
// flow (ID3D12CommandQueue::Wait) - no CPU waits are needed on this path.

#include <d3d12.h>

#include <cstdint>
#include <vector>

#include "veyra/Result.h"

// Opaque NVOF SDK handles (defined by the SDK headers; kept void here so
// this header stays light like the FFmpeg media headers).
using NvOfOpaqueHandle = void*;

namespace veyra::ngx {

class NvOfSession {
public:
    NvOfSession() = default;
    ~NvOfSession();

    NvOfSession(const NvOfSession&) = delete;
    NvOfSession& operator=(const NvOfSession&) = delete;

    struct Desc {
        uint32_t width = 0;              // working extent (input frames)
        uint32_t height = 0;
        ID3D12Fence* inFence = nullptr;   // signaled when inputs are GPU-ready
        ID3D12Fence* outFence = nullptr;  // signaled when flow is GPU-ready
        // P0.4: raw output lives at hardware-grid extent in SHORT2 (S10.5).
        uint32_t quality=1; // 0 performance / 1 balanced / 2 quality; mapped to SDK symbols
        uint32_t gridSize = 4;            // requested grid (validated vs caps)
        // Optional diagnostic/candidate path: BOTH produces previous->current
        // as well, using the same A/B pair. Caller owns both grid textures and
        // follows the same fence and unregister-before-release contract.
        ID3D12Resource* reverseFlow = nullptr; // R16G16_SINT, paired with reverseCost
        ID3D12Resource* reverseCost = nullptr; // R8_UINT
    };

    struct Caps {
        uint32_t requestedQuality=1,appliedQuality=1,actualPerfLevel=0;
        bool queried = false;
        std::vector<uint32_t> supportedGrids;  // raw capability list (two-call protocol)
        uint32_t minWidth = 0, maxWidth = 0, minHeight = 0, maxHeight = 0;
        bool supportsHint = false;
        uint32_t selectedGrid = 0;         // what init actually used
    };
    const Caps& caps() const { return caps_; }

    uint32_t gridWidth() const { return gridW_; }
    uint32_t gridHeight() const { return gridH_; }

    // Loads nvofapi64.dll (System32, absolute), creates the D3D12 instance,
    // initializes the session, and registers the given resources. Contract
    // (user directive 2026-09-04 s6/s8):
    //   - inputA = PREVIOUS frame, inputB = CURRENT frame; both MUST be
    //     DXGI_FORMAT_B8G8R8A8_UNORM (maps to NV_OF_BUFFER_FORMAT_ABGR8 per
    //     the SDK's NvOFD3DCommon.cpp) at desc.width x desc.height.
    //   - Execute output vectors point CURRENT -> PREVIOUS (proven by the
    //     displacement sign matrix in tools/nvof_probe).
    //   - flowOut MUST be R16G16_SINT (SHORT2/S10.5) at
    //     ceil(w/grid) x ceil(h/grid); costOut MUST be R8_UINT at the SAME
    //     grid extent. costOut is REQUIRED (confidence gating is part of the
    //     V1 contract) and is passed ONLY here - there is no second entry.
    // All four resources are validated via GetDesc before registration; any
    // registration failure unregisters everything already registered in
    // reverse order and returns false.
    // Registration supplies explicit fence points and completes on a separate
    // initialization-only fence before success or partial-failure rollback.
    // It does not publish a flow result or add waits to execute().
    bool initialize(ID3D12Device* device,
                    ID3D12Resource* inputA,
                    ID3D12Resource* inputB,
                    ID3D12Resource* flowOut,
                    ID3D12Resource* costOut,
                    const Desc& desc,
                    Status& status);

    // Computes flow with inputFrame = inputB (current) and referenceFrame =
    // inputA (previous); the SDK's vectors point current -> previous. The
    // caller must have scheduled a GPU signal of inFence at `inValue` after
    // both inputs are written; when this call returns, GPU completion of the
    // flow is observable as outFence == nextOutValue().
    bool execute(uint64_t inValue, Status& status);

    uint64_t nextOutValue() const { return nextOutValue_; }
    uint64_t executeCount() const { return executeCount_; }
    bool initialized() const { return initialized_; }

    // Phase 1 of teardown: reverse-order unregister (cost/flow/B/A) while the
    // caller's registered D3D12 resources are STILL ALIVE. The caller must
    // Release those resources AFTER this returns and BEFORE shutdown() -
    // releasing an NVOF-registered resource after nvOFDestroy +
    // FreeLibrary(nvofapi64.dll) segfaults (observed t10-L0-r2, 2026-09-04).
    // Correct ownership order: unregisterAll() -> release textures -> shutdown().
    bool unregisterAll(Status& status);

    // Phase 2: nvOFDestroy -> free function table -> FreeLibrary. Unregisters
    // any still-registered resources first (no-op after unregisterAll()).
    void shutdown();

private:
    struct FnList;                    // NvOFD3D12FunctionList (SDK types)
    void* dll_ = nullptr;             // nvofapi64.dll
    FnList* fn_ = nullptr;
    NvOfOpaqueHandle ofHandle_ = nullptr; // NvOFHandle
    NvOfOpaqueHandle hInputA_ = nullptr;  // NvOFGPUBufferHandle
    NvOfOpaqueHandle hInputB_ = nullptr;
    NvOfOpaqueHandle hFlow_ = nullptr;
    ID3D12Fence* inFence_ = nullptr;
    ID3D12Fence* outFence_ = nullptr;
    uint64_t nextOutValue_ = 1;
    uint64_t executeCount_ = 0;
    bool initialized_ = false;
    Caps caps_;
    uint32_t gridW_ = 0, gridH_ = 0;
    NvOfOpaqueHandle hCost_ = nullptr;
    NvOfOpaqueHandle hReverseFlow_ = nullptr, hReverseCost_ = nullptr;
};

} // namespace veyra::ngx
