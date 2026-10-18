#pragma once

// AVerMedia multichannel capture audio, driven in this process.
//
// The card only forwards the console's compressed (Dolby) stream when
// AVerMedia's own audio graph is running; a plain DirectShow/WASAPI capture of
// the card's audio function keeps handing out linear PCM. The OBS component
// solves this by driving `AVerMedia::AudioDevice` (avt_dshow_core.dll) inside
// the OBS process; this class does the same for Veyra and turns the resulting
// IEC 61937 carrier into interleaved float PCM for the existing capture audio
// session.
//
// The call sequence and the AudioDealer ABI were measured on 2026-10-18 and are
// documented in docs/AVERMEDIA_5P1_DRIVE_HANDOFF_2026-10-18.md. Everything here
// is read-only with respect to the vendor binaries: they are loaded by absolute
// path from the user's own installation, never copied, patched or signed.
//
// Behaviour:
//   * The carrier arrives as 16-bit stereo at 48 kHz. When IEC 61937 bursts are
//     present the payload is unwrapped, byte-swapped per 16-bit word and sent to
//     the FFmpeg AC-3 decoder already used by the bitstream capture path.
//   * When no burst appears within the probe window the carrier is treated as
//     ordinary linear PCM and passed through unchanged, so a console that is
//     set to Linear PCM still produces sound on this path.
//   * After the graph has delivered a stream, a burst silence longer than the
//     loss window rebuilds the vendor graph. This is what recovers the capture
//     after the console changes its audio output format.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace veyra::source {

struct AverMediaVendorAudioState {
    bool started = false;
    bool running = false;
    bool bitstream = false;
    std::wstring device;
    std::string carrier;      // "AC-3" / "linear PCM 2ch" once decided
    unsigned channels = 0;
    uint32_t sampleRate = 0;
    uint64_t callbacks = 0;
    uint64_t carrierBytes = 0;
    uint64_t bursts = 0;
    uint64_t decodedFrames = 0;
    uint64_t pcmFrames = 0;
    uint64_t restarts = 0;
    uint64_t losses = 0;
    std::wstring error;
};

class AverMediaVendorAudio {
public:
    // Implementation handle. Public only so the module's own translation unit
    // can forward-declare it for its callback object.
    struct Impl;

    // Called on the vendor streaming thread with interleaved float PCM.
    // `frames` counts sample frames, not floats.
    using PcmCallback =
        std::function<void(const float* interleaved, size_t frames, unsigned channels, uint32_t sampleRate)>;

    AverMediaVendorAudio();
    ~AverMediaVendorAudio();
    AverMediaVendorAudio(const AverMediaVendorAudio&) = delete;
    AverMediaVendorAudio& operator=(const AverMediaVendorAudio&) = delete;

    // Resolves the installed component, starts the vendor graph on a dedicated
    // STA thread and begins delivering PCM. Returns false (and fills state())
    // when the component is missing or the graph refuses to bind.
    // `preferName` is the capture card's own name ("Live Gamer Ultra 2.1") and
    // `preferPath` its DirectShow device path. The vendor device list is matched
    // on the USB instance first (the audio function of the same card shares it),
    // then on the name, so a second AVerMedia card in the same machine cannot
    // donate its audio to this capture.
    bool start(PcmCallback onPcm, const std::wstring& preferName = std::wstring(),
               const std::wstring& preferPath = std::wstring());

    // Stops the graph and joins the worker. Safe to call more than once.
    void stop();

    bool running() const;
    AverMediaVendorAudioState state() const;

private:
    std::unique_ptr<Impl> p_;
};

} // namespace veyra::source
