#pragma once
// Monitoring overlays that inject into this process and hook presentation.
//
// Field crashes 2026-10-01: with RivaTuner Statistics Server (MSI Afterburner's on-screen
// display, RTSSHooks64.dll) injected, every session that turned XeSS frame generation on
// crashed inside d3d11.dll a second or two later: RTSS draws its OSD with D3D11On12 into
// the XeSS proxy swapchain. GamePP (游加加) held swapchain buffers so every resize failed
// and the driver crashed on a window change. The same tools are fine with the ordinary
// DXGI swapchain and with DLSS frame generation.
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <iterator>
#include <string>

namespace veyra::gfx {

struct PresentationHook {
    const wchar_t* module;
    const char* product;
};

inline constexpr PresentationHook kPresentationHooks[] = {
    {L"RTSSHooks64.dll", "RivaTuner Statistics Server (MSI Afterburner)"},
    {L"GPP64.dll", "GamePP"},
    {L"shade64.dll", "GamePP"},
    {L"GameTracker64.dll", "GamePP"},
};

// RTSS needs a different UI renderer. NVIDIA App module presence is not a failure
// diagnosis and must not trigger a warning or a persistent failure marker.
inline constexpr PresentationHook kRiskyInjections[] = {
    {L"RTSSHooks64.dll", "RivaTuner Statistics Server"},
};

// The compatibility components loaded now (empty when none).
inline std::wstring riskyInjections() {
    std::wstring out;
    for (const auto& hook : kRiskyInjections)
        if (GetModuleHandleW(hook.module)) { if (!out.empty()) out += L','; out += hook.module; }
    return out;
}

// RivaTuner Statistics Server (MSI Afterburner's OSD, also used by HWiNFO, CapFrameX and
// others) is running: its shared memory exists and is live. True before it has injected
// RTSSHooks64.dll here, which it does to every process that creates windows.
// An orphan RTSSHooksLoader64 or an already-injected DLL can outlive the server;
// neither module presence nor the loader process alone means the OSD is active.
// A force-terminated server can also leave a valid shared-memory signature in
// clients that kept the mapping open. Require the server process as well.
//
// Why it matters (reproduced 2026-10-03, RTSS 7.3.5.28314, RTX 5070; version
// corrected against the executable on 2026-10-04): RTSS draws on a single
// Direct3D 12 swapchain at a time and rebuilds its renderer whenever a different one
// presents. With the interface on D3D12 next to the video's own swapchain, the GPU device
// was removed (DXGI_ERROR_ACCESS_DENIED) about half a second after a video started once its
// OSD had something to draw. With the interface drawn without a swapchain, RTSS only sees
// the video and draws its OSD there.
inline bool rivaTunerRunning() {
    const HANDLE raw = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (raw == INVALID_HANDLE_VALUE) return false;
    struct Snapshot { HANDLE handle; ~Snapshot() { CloseHandle(handle); } } snapshot{raw};
    PROCESSENTRY32W process{};
    process.dwSize = sizeof(process);
    bool server = false;
    for (BOOL found = Process32FirstW(snapshot.handle, &process); found;
         found = Process32NextW(snapshot.handle, &process)) {
        if (_wcsicmp(process.szExeFile, L"RTSS.exe") == 0) { server = true; break; }
    }
    if (!server) return false;
    HANDLE map = OpenFileMappingW(FILE_MAP_READ, FALSE, L"RTSSSharedMemoryV2");
    if (!map) return false;
    bool live = false;
    if (const auto* header = static_cast<const DWORD*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, sizeof(DWORD)))) {
        live = header[0] == 0x52545353;   // 'RTSS'; 0xDEAD while the server shuts down
        UnmapViewOfFile(header);
    }
    CloseHandle(map);
    return live;
}

// A running process whose image name satisfies match.
template <class Match> inline bool processRunning(Match match) {
    const HANDLE raw = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (raw == INVALID_HANDLE_VALUE) return false;
    struct Snapshot { HANDLE handle; ~Snapshot() { CloseHandle(handle); } } snapshot{raw};
    PROCESSENTRY32W process{};
    process.dwSize = sizeof(process);
    for (BOOL found = Process32FirstW(snapshot.handle, &process); found;
         found = Process32NextW(snapshot.handle, &process))
        if (match(process.szExeFile)) return true;
    return false;
}

// NVIDIA's in-game overlay is switched on: NVIDIA App's "NVIDIA 信息浮窗" runs
// NVIDIA Overlay.exe, GeForce Experience's in-game overlay runs NVIDIA Share.exe; both
// exit when the overlay is switched off. nvspcap64.dll alone says nothing: the driver
// loads it into every Direct3D process whether or not the overlay is on.
//
// Why it matters (field logs(15), 2026-10-10, RTX 5060 Ti, driver 617.42): during
// capture the engine's video thread and the interface thread stopped at the same moment
// in 10 of 12 freezes (a deadlock between the two presenting threads), and users report
// that 2.0+ freezes within seconds of starting capture with the overlay on and never
// with it off; 1.4.4, whose interface had no swapchain, did not. Like RTSS, the overlay
// hooks every swapchain in the process; with the interface drawn in software only the
// video's swapchain remains. Not reproduced on an RTX 5070 / 616.56 (overlay on, D3D12
// interface, DLSS frame generation, HDR output, 45 s).
inline bool nvidiaOverlayRunning() {
    return processRunning([](const wchar_t* name) {
        return _wcsicmp(name, L"NVIDIA Overlay.exe") == 0 || _wcsicmp(name, L"NVIDIA Share.exe") == 0;
    });
}

// GamePP (游戏加加, GamePP.exe and its GamePP* helpers) injects GPP64.dll (overlay),
// GameTracker64.dll and shade64.dll (filters) into programs it treats as games. Field
// dump 2026-10-01: with them injected, the video's and the interface's swapchains both
// failed every ResizeBuffers and the driver crashed on a window change; the same
// two-swapchain hooking as RTSS and NVIDIA's overlay. Detected by process at start,
// since the modules only arrive after the interface's window exists.
inline bool gamePPRunning() {
    return processRunning([](const wchar_t* name) { return _wcsnicmp(name, L"GamePP", 6) == 0; });
}
inline bool gamePPInjected() {
    return GetModuleHandleW(L"GPP64.dll") || GetModuleHandleW(L"GameTracker64.dll") || GetModuleHandleW(L"shade64.dll");
}

// RTSS reads this variable from the process it hooks whenever it (re)loads its profile for
// it ("Name,value[,Name,value…]", RTSSHooks64.dll); EnableOSD is one of the properties it
// accepts. With EnableOSD,0 it still counts frames but never draws in this process.
inline constexpr wchar_t kRivaTunerProfileOverride[] = L"RTSSHooksProfileOverride";
inline constexpr wchar_t kRivaTunerOsdOff[] = L"EnableOSD,0";

// The first injected overlay found, or nullptr.
inline const PresentationHook* injectedPresentationHook() {
    for (const auto& hook : kPresentationHooks)
        if (GetModuleHandleW(hook.module)) return &hook;
    return nullptr;
}

// Every loaded module that is neither part of Windows nor shipped next to the executable,
// as "name; name; …" for the log. Overlays, recorders and monitoring tools show up here;
// a field log with a mystery slowdown can then say which of them was in the process.
inline std::string thirdPartyModules() {
    HMODULE modules[1024]; DWORD needed = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) return "unavailable";
    wchar_t windowsDir[MAX_PATH]{}, exeDir[MAX_PATH]{};
    GetWindowsDirectoryW(windowsDir, MAX_PATH);
    GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
    if (auto* slash = wcsrchr(exeDir, L'\\')) slash[1] = 0;
    const auto startsWith = [](const wchar_t* path, const wchar_t* prefix) {
        const size_t n = wcslen(prefix); return n && _wcsnicmp(path, prefix, n) == 0;
    };
    std::string out;
    const DWORD count = std::min<DWORD>(needed / sizeof(HMODULE), DWORD(std::size(modules)));
    for (DWORD i = 0; i < count; ++i) {
        wchar_t path[MAX_PATH]{};
        if (!GetModuleFileNameW(modules[i], path, MAX_PATH)) continue;
        // Driver components live under Windows\System32\DriverStore; keep those, they carry
        // the driver's own overlay and filter modules.
        if ((startsWith(path, windowsDir) && !wcsstr(path, L"\\DriverStore\\")) || startsWith(path, exeDir)) continue;
        char utf8[MAX_PATH * 3]{};
        WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, int(sizeof(utf8)), nullptr, nullptr);
        if (!out.empty()) out += "; ";
        out += utf8;
    }
    return out.empty() ? "none" : out;
}

} // namespace veyra::gfx
