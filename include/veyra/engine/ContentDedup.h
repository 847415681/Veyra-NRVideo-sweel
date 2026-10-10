#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace veyra::engine {
// Repeated-picture removal and content cadence for live sources (2026-10-11).
//
// A game locked to 30 or 40 fps behind a 60 Hz capture repeats pictures. The
// old "60->30" setting kept every second sample by timestamp, blind to the
// picture: whenever the game's frames did not land on that grid (a hitch, a
// 40 fps cap, a 58/59.94 Hz transport) it dropped real frames and kept
// repeats, and frame generation then interpolated across a doubled step
// (ghosting) or a still pair; "auto" only reported a rate.
//
// A sample is dropped here only when its picture repeats the last kept one,
// judged on the 64x36 luma points the capture callback already takes (~2.3k
// compares per frame, no GPU work). Kept frames get a content timestamp from a
// clamped phase-locked cadence: the picture changed after the previous sample
// and no later than this one, so the predicted time is clamped into that
// window. Regular content (30 in 58 Hz, 40 in 60 Hz) gets evenly spaced
// timestamps that are never later than the sample; hitches and variable rates
// keep their sample times.
class ContentDedup {
public:
    static constexpr unsigned kPoints = 64 * 36;
    struct Decision {
        bool keep = true;
        int64_t contentPts = 0;   // 100 ns, kept frames
        int64_t interval = 0;     // 100 ns from the previous kept frame (0 = unknown)
        double median = 0;        // median |luma difference| to the last kept frame (levels)
        unsigned changed = 0;     // points beyond the change gate
        const char* reason = "first";
    };
    struct Stats {
        uint64_t observed = 0, kept = 0, repeats = 0, capped = 0, held = 0, blind = 0;
        uint64_t regular = 0, irregular = 0;
        double maxShiftMs = 0;    // largest sample-to-content correction
    };

    void reset() {
        haveKept_ = haveContent_ = skipSpan_ = false; prevSample_ = 0; lastKeptSample_ = 0; lastContent_ = 0;
        timeCount_ = 0; timeHead_ = 0; period_ = 0; misses_ = 0;
    }
    // luma: kPoints 8-bit samples or nullptr (no CPU picture: keep everything).
    // capPeriod: minimum spacing of kept frames (the 60->30 setting), 0 = none.
    // hintPeriod: expected content period of the selected content rate, 0 = unknown.
    Decision observe(const uint8_t* luma, int64_t pts, int64_t transport, bool boundary,
                     int64_t capPeriod = 0, int64_t hintPeriod = 0) {
        Decision d;
        ++stats_.observed;
        if (transport <= 0) transport = 166667;
        const int64_t previous = prevSample_;
        const bool continuous = haveKept_ && !boundary && pts > previous && pts - previous <= transport * 6;
        prevSample_ = pts;
        if (!luma) {
            // No CPU picture (hardware-decoded streams): keep everything, except
            // that 60->30 still samples every second slot by timestamp as before.
            // A boundary (open, seek, settings change) keeps its first frame and
            // re-anchors, like the old sampler: transactions wait for that frame.
            ++stats_.blind; haveKept_ = false;
            if (boundary) { reset(); prevSample_ = pts; }
            else if (capPeriod > 0 && haveContent_ && pts > lastKeptSample_ && pts - lastKeptSample_ + transport / 4 < capPeriod && pts - lastKeptSample_ <= transport * 6) {
                ++stats_.capped; d.keep = false; d.reason = "rate-cap"; return d;
            }
            return keep(d, pts, previous, transport, nullptr, Timing::Sample, "no-picture");
        }
        if (!continuous) {
            reset(); prevSample_ = pts;
            return keep(d, pts, previous, transport, luma, Timing::Sample, boundary ? "boundary" : "first");
        }
        // Compare with the last KEPT picture: a slow fade accumulates until it
        // counts instead of being dropped one sub-threshold step at a time.
        std::array<uint16_t, 256> histogram{};
        const int gate = int(std::max(8.0, 3.0 * noise_ + 6.0));
        unsigned changed = 0;
        for (unsigned i = 0; i < kPoints; ++i) {
            const int diff = std::abs(int(luma[i]) - int(kept_[i]));
            ++histogram[diff];
            changed += diff > gate;
        }
        unsigned median = 0;
        for (unsigned seen = 0; median < 255; ++median) if ((seen += histogram[median]) * 2 >= kPoints) break;
        d.median = median; d.changed = changed;
        // New picture: a point moved clearly (a small object or the HUD in a still
        // scene counts; a clean capture repeats bit-exactly, so one point is enough
        // until noise has been seen) or most of the picture shifted (fades, pans).
        const double medianGate = std::max(1.5, 1.5 * noise_ + 0.75);
        const bool repeat = changed < (noise_ < 0.5 ? 1u : 3u) && median <= medianGate;
        const int64_t sinceKept = pts - lastKeptSample_;
        if (repeat) {
            // Never hold one picture for long: a still screen keeps a frame now
            // and then, and a change too slow to pass the gates still advances.
            const int64_t hold = std::clamp<int64_t>(period_ ? period_ * 4 : transport * 8, 1500000, 2500000);
            if (sinceKept < hold) {
                noise_ = noise_ * 0.9 + 0.1 * median;
                ++stats_.repeats; d.keep = false; d.reason = "repeat"; return d;
            }
            ++stats_.held;
            return keep(d, pts, previous, transport, luma, Timing::Sample, "hold-limit");
        }
        if (capPeriod > 0 && sinceKept + transport / 4 < capPeriod) {
            // 60->30: a new picture one slot after the last kept one (60 fps
            // content) is over the cap. Repeats never take its place.
            ++stats_.capped; d.keep = false; d.reason = "rate-cap"; return d;
        }
        return keep(d, pts, previous, transport, luma, Timing::Cadence, "new", hintPeriod);
    }
    // Estimated content rate of the kept frames (0 until established).
    double contentFps() const { return period_ > 0 ? 1e7 / double(period_) : 0.0; }
    double noiseLevel() const { return noise_; }
    const Stats& stats() const { return stats_; }

private:
    enum class Timing { Sample, Cadence };
    static constexpr unsigned kTimes = 48;
    Decision& keep(Decision& d, int64_t pts, int64_t previous, int64_t transport, const uint8_t* luma, Timing timing,
                   const char* reason, int64_t hint = 0) {
        d.keep = true; d.reason = reason; ++stats_.kept;
        int64_t content = pts;
        if (timing == Timing::Cadence) {
            // A span that started at a held (still-screen) frame is not a content period.
            if (skipSpan_) { timeCount_ = 0; timeHead_ = 0; }
            if (timeCount_ == 0) push(lastKeptSample_);
            push(pts);
            if (timeCount_ >= 8) {
                // Least-squares slope of sample time over kept-frame index: the
                // sample grid quantizes each time by up to one slot, and the
                // slope over n frames is off by about slot/n^1.5 (0.05 ms at 58 Hz).
                const unsigned n = timeCount_, first = (timeHead_ + kTimes - n) % kTimes;
                const double origin = double(times_[first]), mean = (n - 1) / 2.0;
                double sxx = 0, sxt = 0, st = 0;
                for (unsigned i = 0; i < n; ++i) st += double(times_[(first + i) % kTimes]) - origin;
                for (unsigned i = 0; i < n; ++i) {
                    const double x = i - mean, t = double(times_[(first + i) % kTimes]) - origin - st / n;
                    sxx += x * x; sxt += x * t;
                }
                period_ = std::max<int64_t>(int64_t(std::llround(sxt / sxx)), transport - transport / 8);
                if (hint > 0 && std::llabs(period_ - hint) * 100 < hint * 3) period_ = hint;
            }
            // The picture changed after the previous sample and by this one.
            const int64_t lo = std::max(lastKeptSample_, previous), hi = pts;
            if (period_ > 0) {
                const int64_t predicted = lastContent_ + period_, tolerance = transport / 2;
                if (predicted >= lo - tolerance && predicted <= hi + tolerance) {
                    content = std::clamp(predicted, lo + 1, hi); misses_ = 0; ++stats_.regular;
                } else {
                    ++stats_.irregular;
                    // Two misses in a row: the rate changed; relearn it from here.
                    if (++misses_ >= 2) { timeCount_ = 0; timeHead_ = 0; period_ = 0; misses_ = 0; push(pts); }
                }
            }
        }
        if (haveContent_) {
            content = std::max(content, lastContent_ + 10000);
            d.interval = content - lastContent_;
        }
        skipSpan_ = timing == Timing::Sample && haveContent_;
        haveContent_ = true;
        stats_.maxShiftMs = std::max(stats_.maxShiftMs, double(pts - content) / 1e4);
        d.contentPts = content;
        if (luma) { std::memcpy(kept_.data(), luma, kPoints); haveKept_ = true; }
        lastKeptSample_ = pts; lastContent_ = content;
        return d;
    }

    void push(int64_t time) { times_[timeHead_] = time; timeHead_ = (timeHead_ + 1) % kTimes; timeCount_ = std::min(timeCount_ + 1, kTimes); }

    std::array<uint8_t, kPoints> kept_{};
    std::array<int64_t, kTimes> times_{};
    unsigned timeCount_ = 0, timeHead_ = 0, misses_ = 0;
    bool haveKept_ = false, haveContent_ = false, skipSpan_ = false;
    int64_t prevSample_ = 0, lastKeptSample_ = 0, lastContent_ = 0, period_ = 0;
    double noise_ = 0;
    Stats stats_;
};
}
