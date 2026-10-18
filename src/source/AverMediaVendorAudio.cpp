#include "veyra/source/AverMediaVendorAudio.h"

#include "veyra/source/AverMediaAudioSwitch.h"
#include "veyra/sink/BitstreamAudio.h"
#include "veyra/Log.h"

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace veyra::source {
namespace {

// IEC 61937 sync as it appears on the carrier: the words 0xF872 0x4E1F are
// stored little-endian, so the byte order is 72 F8 1F 4E.
constexpr uint8_t kIecSync[4] = {0x72, 0xF8, 0x1F, 0x4E};
constexpr size_t kIecHeaderBytes = 8;

// How long the carrier is watched before it is declared linear PCM.
constexpr double kProbeSeconds = 4.0;
// Burst silence after a working bitstream before the vendor graph is rebuilt.
constexpr double kLossSeconds = 6.0;
constexpr int kMaxRestarts = 3;
// Bound on the retained carrier tail; bursts are 6144 bytes at most for AC-3.
constexpr size_t kCarrierKeep = 1u << 20;

double nowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string out(size_t(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

// ABI of AVerMedia::AudioDevice as measured on 2026-10-18
// (docs/AVERMEDIA_5P1_DRIVE_HANDOFF_2026-10-18.md section 2/3).
struct VendorDeviceInfo {
    std::wstring friendlyName;
    std::wstring devicePath;
};

// "\\?\usb#vid_07ca&pid_2553&mi_00#9&183620d5&0&0000#{...}\global" ->
// "9&183620d5&0". A composite USB device gives every interface its own instance
// id that differs only in the trailing "&NNNN" (measured: video mi_00 ends in
// &0000, audio mi_02 in &0002), so the shared port prefix is what pins the
// component's device list to this physical capture card.
std::wstring usbInstanceId(std::wstring_view path) {
    std::wstring lower(path);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return wchar_t(towlower(c)); });
    const auto mi = lower.find(L"&mi_");
    if (mi == std::wstring::npos) return {};
    const auto first = lower.find(L'#', mi);
    if (first == std::wstring::npos) return {};
    const auto second = lower.find(L'#', first + 1);
    if (second == std::wstring::npos || second - first < 2) return {};
    std::wstring instance = lower.substr(first + 1, second - first - 1);
    // Drop the per-interface tail: "9&183620d5&0&0002" -> "9&183620d5&0".
    const auto tail = instance.rfind(L'&');
    if (tail != std::wstring::npos && instance.size() - tail == 5) instance.erase(tail);
    return instance;
}

struct AverMediaVendorAudio::Impl;

class VendorAudioDealer {
public:
    virtual ~VendorAudioDealer() = default;
    virtual int onAudioData(void* header, const void* data, int length, unsigned extra) = 0;
};

// The vendor stub expects the two-slot vtable: slot 0 the deleting destructor,
// slot 1 the data callback. Any extra virtual above these two would shift it.
class Dealer final : public VendorAudioDealer {
public:
    explicit Dealer(class AverMediaVendorAudio::Impl* owner) : owner_(owner) {}
    int onAudioData(void* header, const void* data, int length, unsigned extra) override;

private:
    AverMediaVendorAudio::Impl* owner_ = nullptr;
};

using CtorFn = void* (*)(void*);
using DtorFn = void (*)(void*);
using ConnectFn = bool (*)(void*);
using GetDeviceListFn = void (*)(std::vector<VendorDeviceInfo>&);
using ResetGraphFn = bool (*)(void*);
using SetCallbackFn = bool (*)(void*, VendorAudioDealer*);
using StartFn = bool (*)(void*);
using StopFn = bool (*)(void*);
using UpdateDeviceFn = bool (*)(void*, const std::wstring&, const std::wstring&);
using LogSinkFn = void (*)(void (*)(const char*, char*));

constexpr const char* kExportCtor = "??0AudioDevice@AVerMedia@@QEAA@XZ";
constexpr const char* kExportDtor = "??1AudioDevice@AVerMedia@@QEAA@XZ";
constexpr const char* kExportConnect = "?ConnectFilters@AudioDevice@AVerMedia@@QEAA_NXZ";
constexpr const char* kExportDeviceList =
    "?GetDeviceList@AudioDevice@AVerMedia@@SAXAEAV?$vector@UDeviceInfo@AVerMedia@@V?$allocator@UDeviceInfo@AVerMedia@@@std@@@std@@@Z";
constexpr const char* kExportReset = "?ResetGraph@AudioDevice@AVerMedia@@QEAA_NXZ";
constexpr const char* kExportSetCallback = "?SetCallback@AudioDevice@AVerMedia@@QEAA_NPEAVAudioDealer@2@@Z";
constexpr const char* kExportStart = "?Start@AudioDevice@AVerMedia@@QEAA_NXZ";
constexpr const char* kExportStop = "?Stop@AudioDevice@AVerMedia@@QEAA_NXZ";
constexpr const char* kExportUpdate =
    "?UpdateDevice@AudioDevice@AVerMedia@@QEAA_NAEBV?$basic_string@_WU?$char_traits@_W@std@@V?$allocator@_W@2@@std@@0@Z";
constexpr const char* kExportLogInfo = "?setInfoHandler@LogHelper@@YAXP6AXPEBDPEAD@Z@Z";
constexpr const char* kExportLogWarn = "?setWarningHandler@LogHelper@@YAXP6AXPEBDPEAD@Z@Z";
constexpr const char* kExportLogError = "?setErrorHandler@LogHelper@@YAXP6AXPEBDPEAD@Z@Z";
constexpr const char* kExportLogDebug = "?setDebugHandler@LogHelper@@YAXP6AXPEBDPEAD@Z@Z";

// The vendor component writes its own diagnostics through these callbacks. The
// format string is not filled in by the component, so the text is passed
// through verbatim (see the handoff: "%d"/"%ls" stay unexpanded).
void vendorLog(const char* level, const char* message, char* extra) {
    std::string line = message != nullptr ? message : "";
    if (extra != nullptr) {
        bool printable = true;
        for (int i = 0; i < 180 && extra[i] != 0; ++i) {
            const unsigned char c = static_cast<unsigned char>(extra[i]);
            if (c < 32 || c > 126) { printable = false; break; }
        }
        if (printable && extra[0] != 0) line += " | " + std::string(extra);
    }
    const std::string tag = std::string("capture-audio-vendor/") + (level != nullptr ? level : "info");
    if (std::strstr(line.c_str(), "fail") != nullptr || std::strstr(line.c_str(), "null") != nullptr)
        log::warn(tag.c_str(), line);
    else
        log::info(tag.c_str(), line);
}
void vendorInfo(const char* message, char* extra) { vendorLog("info", message, extra); }
void vendorWarn(const char* message, char* extra) { vendorLog("warn", message, extra); }
void vendorError(const char* message, char* extra) { vendorLog("error", message, extra); }
void vendorDebug(const char* message, char* extra) { vendorLog("dbg", message, extra); }

} // namespace

struct AverMediaVendorAudio::Impl {
    mutable std::mutex mutex;
    AverMediaVendorAudioState state;
    PcmCallback onPcm;
    std::thread worker;
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> restartRequested{false};
    std::wstring preferName;
    std::wstring preferPath;

    // Graph objects; only touched on the worker thread.
    HMODULE module = nullptr;
    std::vector<PVOID> dllDirectories;
    CtorFn ctor = nullptr;
    DtorFn dtor = nullptr;
    ConnectFn connect = nullptr;
    GetDeviceListFn deviceList = nullptr;
    ResetGraphFn reset = nullptr;
    SetCallbackFn setCallback = nullptr;
    StartFn startGraph = nullptr;
    StopFn stopGraph = nullptr;
    UpdateDeviceFn updateDevice = nullptr;
    void* device = nullptr;
    Dealer dealer{this};

    // Decode state; touched by the streaming callback and reset by the worker.
    std::mutex decodeMutex;
    bool tearingDown = false;
    enum class Mode { Undecided, Bitstream, LinearPcm } mode = Mode::Undecided;
    std::vector<uint8_t> carrier;
    std::vector<uint8_t> ac3;
    std::vector<float> pcm;
    std::unique_ptr<sink::BitstreamDecoder> decoder;
    double openedAt = 0;
    double lastBurstAt = 0;
    uint16_t dataType = 0;

    bool loadLibrary();
    bool resolve();
    bool setupGraph();
    void teardownGraph();
    void run();
    void onCarrier(const uint8_t* data, size_t bytes);
    void decodeCarrier();
    void emitBitstream();
    void emitLinearPcm();
    void noteError(const std::string& text);
};

int Dealer::onAudioData(void* header, const void* data, int length, unsigned extra) {
    (void)header;
    (void)extra;
    if (owner_ == nullptr) return 1;
    if (data == nullptr || length <= 0) return 1;
    owner_->onCarrier(static_cast<const uint8_t*>(data), size_t(length));
    return 1;
}

bool AverMediaVendorAudio::Impl::loadLibrary() {
    const std::wstring root = AverMediaAudioSwitch::locateComponentRoot();
    if (root.empty()) {
        noteError("AVerMedia Multichannel Audio component not found");
        return false;
    }
    const std::wstring coreDir = root + L"\\obs-plugins\\64bit";
    const std::wstring dataDir = root + L"\\data\\obs-plugins\\AVerMediaMultichannelAudio";
    // The core DLL needs both directories: avt_device_opener.dll lives next to
    // it, the Realtek SDK / MSVC 2012 runtime sit in the plugin's data folder.
    for (const std::wstring& dir : {coreDir, dataDir}) {
        if (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        if (PVOID cookie = AddDllDirectory(dir.c_str()); cookie != nullptr) dllDirectories.push_back(cookie);
    }
    const std::wstring corePath = coreDir + L"\\avt_dshow_core.dll";
    module = LoadLibraryExW(corePath.c_str(), nullptr,
                            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
                                LOAD_LIBRARY_SEARCH_USER_DIRS);
    if (module == nullptr) {
        noteError(std::format("LoadLibraryExW(avt_dshow_core.dll) failed error={}", GetLastError()));
        return false;
    }
    log::info("capture-audio-vendor", std::format("vendor graph component root=\"{}\"", narrow(root)));
    return true;
}

bool AverMediaVendorAudio::Impl::resolve() {
    auto get = [this](const char* name) -> void* {
        void* address = reinterpret_cast<void*>(GetProcAddress(module, name));
        if (address == nullptr) log::warn("capture-audio-vendor", std::string("missing export ") + name);
        return address;
    };
    ctor = reinterpret_cast<CtorFn>(get(kExportCtor));
    dtor = reinterpret_cast<DtorFn>(get(kExportDtor));
    connect = reinterpret_cast<ConnectFn>(get(kExportConnect));
    deviceList = reinterpret_cast<GetDeviceListFn>(get(kExportDeviceList));
    reset = reinterpret_cast<ResetGraphFn>(get(kExportReset));
    setCallback = reinterpret_cast<SetCallbackFn>(get(kExportSetCallback));
    startGraph = reinterpret_cast<StartFn>(get(kExportStart));
    stopGraph = reinterpret_cast<StopFn>(get(kExportStop));
    updateDevice = reinterpret_cast<UpdateDeviceFn>(get(kExportUpdate));
    if (auto set = reinterpret_cast<LogSinkFn>(get(kExportLogInfo))) set(vendorInfo);
    if (auto set = reinterpret_cast<LogSinkFn>(get(kExportLogWarn))) set(vendorWarn);
    if (auto set = reinterpret_cast<LogSinkFn>(get(kExportLogError))) set(vendorError);
    if (auto set = reinterpret_cast<LogSinkFn>(get(kExportLogDebug))) set(vendorDebug);
    const bool complete = ctor != nullptr && dtor != nullptr && connect != nullptr && reset != nullptr &&
                          setCallback != nullptr && startGraph != nullptr && stopGraph != nullptr &&
                          updateDevice != nullptr && deviceList != nullptr;
    if (!complete) noteError("avt_dshow_core.dll is missing required AudioDevice exports");
    return complete;
}

bool AverMediaVendorAudio::Impl::setupGraph() {
    {
        std::lock_guard lock(decodeMutex);
        tearingDown = false;
        mode = Mode::Undecided;
        carrier.clear();
        ac3.clear();
        pcm.clear();
        decoder.reset();
        dataType = 0;
        openedAt = nowSeconds();
        lastBurstAt = 0;
    }
    // 16 bytes: the wrapper itself only needs 8, the ctor stores a pointer.
    device = ctor(std::calloc(1, 16));
    if (device == nullptr) {
        noteError("AudioDevice ctor returned null");
        return false;
    }
    // ResetGraph is the real Initialize: it creates the filter graph and marks
    // the core as initialised (core+0x30).
    if (!reset(device)) {
        noteError("ResetGraph (Initialize) failed");
        return false;
    }
    std::vector<VendorDeviceInfo> devices;
    deviceList(devices);
    log::info("capture-audio-vendor", std::format("vendor device list count={}", devices.size()));
    auto lowerOf = [](std::wstring text) {
        std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return wchar_t(towlower(c)); });
        return text;
    };
    const VendorDeviceInfo* chosen = nullptr;
    const VendorDeviceInfo* audioFunction = nullptr;
    const VendorDeviceInfo* preferred = nullptr;
    const VendorDeviceInfo* instanceMatch = nullptr;
    const std::wstring wanted = lowerOf(preferName);
    // The video and audio functions of one card share the USB instance id, the
    // segment after "&mi_XX#" in the device path. That is the only identity the
    // component's device list carries that a second card cannot forge.
    const std::wstring videoInstance = usbInstanceId(preferPath);
    for (const auto& info : devices) {
        log::info("capture-audio-vendor", std::format("vendor device name=\"{}\" path=\"{}\"",
                                                      narrow(info.friendlyName), narrow(info.devicePath)));
        if (info.devicePath.empty()) continue;
        if (audioFunction == nullptr && lowerOf(info.devicePath).find(L"mi_02") != std::wstring::npos)
            audioFunction = &info;
        if (preferred == nullptr && !wanted.empty() && lowerOf(info.friendlyName).rfind(wanted, 0) == 0)
            preferred = &info;
        if (instanceMatch == nullptr && !videoInstance.empty() &&
            lowerOf(info.devicePath).find(videoInstance) != std::wstring::npos)
            instanceMatch = &info;
    }
    if (!videoInstance.empty() && instanceMatch == nullptr)
        log::info("capture-audio-vendor", std::format("no vendor device shares instance=\"{}\"", narrow(videoInstance)));
    // Prefer the audio function of this very card, then the card named after the
    // video device, then any audio function the component lists.
    chosen = instanceMatch != nullptr ? instanceMatch : preferred != nullptr ? preferred : audioFunction;
    if (chosen == nullptr) {
        noteError("vendor device list has no audio function (is the interactive desktop active?)");
        return false;
    }
    if (!updateDevice(device, chosen->friendlyName, chosen->devicePath)) {
        noteError("UpdateDevice failed");
        return false;
    }
    if (!connect(device)) {
        noteError("ConnectFilters failed");
        return false;
    }
    // core+0x18 holds the interface the callback is installed on; with a null
    // device pointer SetCallback would crash inside the vendor code.
    void* core = *reinterpret_cast<void**>(device);
    void* callbackInterface =
        core != nullptr ? *reinterpret_cast<void**>(reinterpret_cast<unsigned char*>(core) + 0x18) : nullptr;
    if (callbackInterface == nullptr) {
        noteError("SetCallback skipped: the graph exposes no callback interface");
        return false;
    }
    if (!setCallback(device, &dealer)) {
        noteError("SetCallback failed");
        return false;
    }
    if (!startGraph(device)) {
        noteError("Start failed");
        return false;
    }
    {
        std::lock_guard lock(mutex);
        state.device = chosen->friendlyName;
        state.started = true;
        state.running = true;
        state.error.clear();
    }
    log::info("capture-audio-vendor", std::format("vendor graph started device=\"{}\"", narrow(chosen->friendlyName)));
    return true;
}

void AverMediaVendorAudio::Impl::teardownGraph() {
    {
        std::lock_guard lock(decodeMutex);
        tearingDown = true;
        decoder.reset();
        carrier.clear();
        ac3.clear();
    }
    if (device != nullptr) {
        if (stopGraph != nullptr) stopGraph(device);
        if (dtor != nullptr) dtor(device);
        std::free(device);
        device = nullptr;
    }
}

void AverMediaVendorAudio::Impl::noteError(const std::string& text) {
    std::lock_guard lock(mutex);
    state.error = std::wstring(text.begin(), text.end());
    state.running = false;
    log::warn("capture-audio-vendor", text);
}

void AverMediaVendorAudio::Impl::onCarrier(const uint8_t* data, size_t bytes) {
    std::lock_guard lock(decodeMutex);
    if (tearingDown) return;
    {
        std::lock_guard guard(mutex);
        ++state.callbacks;
        state.carrierBytes += bytes;
    }
    carrier.insert(carrier.end(), data, data + bytes);
    decodeCarrier();
}

void AverMediaVendorAudio::Impl::decodeCarrier() {
    if (mode == Mode::Undecided) {
        // Look for two bursts of the same data type: one may be a stray sync
        // word inside payload, two matching ones are decisive.
        size_t cursor = 0;
        size_t found = 0;
        uint16_t foundType = 0;
        while (cursor + kIecHeaderBytes <= carrier.size()) {
            if (std::memcmp(carrier.data() + cursor, kIecSync, 4) != 0) {
                ++cursor;
                continue;
            }
            const uint16_t type = uint16_t(carrier[cursor + 4]) | uint16_t(carrier[cursor + 5] << 8);
            if (sink::classifyIec61937DataType(type) == sink::BitstreamKind::None) {
                ++cursor;
                continue;
            }
            if (found == 0 || type == foundType) {
                ++found;
                foundType = type;
                if (found >= 2) break;
            } else {
                found = 1;
                foundType = type;
            }
            cursor += kIecHeaderBytes;
        }
        if (found >= 2) {
            const auto kind = sink::classifyIec61937DataType(foundType);
            decoder = std::make_unique<sink::BitstreamDecoder>();
            if (decoder->open(kind, false)) {
                mode = Mode::Bitstream;
                dataType = foundType;
                lastBurstAt = nowSeconds();
                log::info("capture-audio-vendor", std::format(
                    "carrier is IEC 61937 dataType=0x{:02X} ({}); decoding to PCM",
                    unsigned(foundType), sink::bitstreamKindName(kind)));
                {
                    std::lock_guard lock(mutex);
                    state.bitstream = true;
                    state.carrier = sink::bitstreamKindName(kind);
                }
            } else {
                const std::string why = decoder->lastError();
                decoder.reset();
                noteError(std::string("bitstream decoder refused the stream: ") + why);
            }
        } else if (nowSeconds() - openedAt > kProbeSeconds || carrier.size() > kCarrierKeep) {
            mode = Mode::LinearPcm;
            log::info("capture-audio-vendor", std::format(
                "no IEC 61937 burst in {:.1f}s of carrier; treating it as linear PCM", nowSeconds() - openedAt));
            std::lock_guard lock(mutex);
            state.carrier = "linear PCM 2ch";
        }
    }
    if (mode == Mode::Bitstream) emitBitstream();
    else if (mode == Mode::LinearPcm) emitLinearPcm();
    else if (carrier.size() > kCarrierKeep) carrier.erase(carrier.begin(), carrier.end() - kCarrierKeep / 2);
}

void AverMediaVendorAudio::Impl::emitBitstream() {
    size_t cursor = 0;
    while (cursor + kIecHeaderBytes <= carrier.size()) {
        if (std::memcmp(carrier.data() + cursor, kIecSync, 4) != 0) { ++cursor; continue; }
        const size_t payloadBits = size_t(carrier[cursor + 6]) | size_t(carrier[cursor + 7] << 8);
        const size_t payload = payloadBits / 8;
        if (payload == 0 || cursor + kIecHeaderBytes + payload > carrier.size()) break;
        {
            std::lock_guard guard(mutex);
            ++state.bursts;
        }
        const uint8_t* words = carrier.data() + cursor + kIecHeaderBytes;
        // The carrier stores the bitstream as 16-bit words in little-endian
        // order; the codec wants them in wire order, so swap each pair.
        for (size_t i = 0; i + 1 < payload; i += 2) {
            ac3.push_back(words[i + 1]);
            ac3.push_back(words[i]);
        }
        lastBurstAt = nowSeconds();
        cursor += kIecHeaderBytes + payload;
    }
    if (cursor > 0) carrier.erase(carrier.begin(), carrier.begin() + std::ptrdiff_t(cursor));
    if (ac3.empty()) return;
    pcm.clear();
    if (decoder != nullptr && decoder->push(ac3.data(), ac3.size(), pcm)) {
        ac3.clear();
        const unsigned channels = decoder->channels();
        const uint32_t rate = decoder->sampleRate();
        if (!pcm.empty() && onPcm && channels > 0 && rate > 0) {
            const size_t frames = pcm.size() / channels;
            {
                std::lock_guard lock(mutex);
                state.channels = channels;
                state.sampleRate = rate;
                state.decodedFrames += frames;
            }
            onPcm(pcm.data(), frames, channels, rate);
        }
    } else if (ac3.size() > kCarrierKeep) {
        ac3.clear();
    }
    // A stream that stopped producing bursts is rebuilt from scratch.
    if (lastBurstAt > 0 && nowSeconds() - lastBurstAt > kLossSeconds && !restartRequested.exchange(true)) {
        std::lock_guard lock(mutex);
        ++state.losses;
        log::warn("capture-audio-vendor", std::format("no IEC 61937 burst for {:.1f}s; rebuilding the vendor graph",
                                                     nowSeconds() - lastBurstAt));
    }
}

void AverMediaVendorAudio::Impl::emitLinearPcm() {
    // 16-bit stereo carrier: pass it through so a console set to Linear PCM
    // still plays on this path.
    const size_t frames = carrier.size() / 4;
    if (frames == 0) return;
    pcm.resize(frames * 2);
    for (size_t i = 0; i < frames * 2; ++i) {
        const int16_t sample = int16_t(uint16_t(carrier[i * 2]) | uint16_t(carrier[i * 2 + 1] << 8));
        pcm[i] = float(sample) / 32768.0f;
    }
    carrier.erase(carrier.begin(), carrier.begin() + std::ptrdiff_t(frames * 4));
    if (onPcm) {
        {
            std::lock_guard lock(mutex);
            state.channels = 2;
            state.sampleRate = 48000;
            state.pcmFrames += frames;
        }
        onPcm(pcm.data(), frames, 2, 48000);
    }
}

void AverMediaVendorAudio::Impl::run() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) {
        noteError(std::format("CoInitializeEx failed hr=0x{:08X}", unsigned(com)));
        return;
    }
    if (!loadLibrary() || !resolve()) {
        if (module != nullptr) FreeLibrary(module);
        module = nullptr;
        if (SUCCEEDED(com)) CoUninitialize();
        return;
    }
    if (!setupGraph()) {
        teardownGraph();
        FreeLibrary(module);
        module = nullptr;
        if (SUCCEEDED(com)) CoUninitialize();
        return;
    }
    int restarts = 0;
    while (!stopRequested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (restartRequested.exchange(false) && restarts < kMaxRestarts) {
            ++restarts;
            log::info("capture-audio-vendor", std::format("rebuilding the vendor graph ({} of {})", restarts, kMaxRestarts));
            teardownGraph();
            openedAt = nowSeconds();
            if (!setupGraph()) {
                log::warn("capture-audio-vendor", "vendor graph rebuild failed; audio stops here");
                break;
            }
            std::lock_guard lock(mutex);
            ++state.restarts;
        }
    }
    teardownGraph();
    FreeLibrary(module);
    module = nullptr;
    for (PVOID cookie : dllDirectories) RemoveDllDirectory(cookie);
    dllDirectories.clear();
    {
        std::lock_guard lock(mutex);
        state.running = false;
        state.started = false;
    }
    if (SUCCEEDED(com)) CoUninitialize();
}

AverMediaVendorAudio::AverMediaVendorAudio() : p_(std::make_unique<Impl>()) {}

AverMediaVendorAudio::~AverMediaVendorAudio() {
    stop();
    if (p_ && p_->module != nullptr) FreeLibrary(p_->module);
}

bool AverMediaVendorAudio::start(PcmCallback onPcm, const std::wstring& preferName,
                                 const std::wstring& preferPath) {
    if (p_->worker.joinable()) return p_->state.running;
    p_->onPcm = std::move(onPcm);
    p_->preferName = preferName;
    p_->preferPath = preferPath;
    p_->stopRequested = false;
    p_->restartRequested = false;
    p_->worker = std::thread([this] { p_->run(); });
    // The worker loads the component and starts streaming. Failures (missing
    // exports, a refusing component, a rejected session) surface within a second
    // or two and must fall back at once. A healthy graph, on the other hand,
    // prerolls inside the vendor's Start() for 5 to 13 seconds (measured
    // 2026-10-18: 5.2 s with a burst carrier, 13.0 s on linear PCM), so a timeout
    // there is not a failure - the callbacks arrive on the worker thread and the
    // caller keeps this object either way.
    for (int i = 0; i < 300; ++i) {
        {
            std::lock_guard lock(p_->mutex);
            if (p_->state.error.empty() && !p_->state.running)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            else
                break;
        }
    }
    std::lock_guard lock(p_->mutex);
    return p_->state.error.empty();
}

void AverMediaVendorAudio::stop() {
    if (!p_ || !p_->worker.joinable()) return;
    p_->stopRequested = true;
    p_->worker.join();
}

bool AverMediaVendorAudio::running() const {
    std::lock_guard lock(p_->mutex);
    return p_->state.running;
}

AverMediaVendorAudioState AverMediaVendorAudio::state() const {
    std::lock_guard lock(p_->mutex);
    return p_->state;
}

} // namespace veyra::source
