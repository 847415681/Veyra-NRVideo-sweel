// Scene/Cadence analyzer tests
#include <cstdio>
#include <cmath>
#include <vector>

#include "veyra/core/SceneCadenceAnalyzer.h"

namespace {
int g_failures = 0;
int g_checks = 0;
void check(const char* name, bool ok, const char* detail = "") {
    ++g_checks;
    std::printf("[%s] %s :: %s\n", ok ? "PASS" : "FAIL", name, detail);
    if (!ok) ++g_failures;
}
std::vector<double> makeHistogram(double mean) {
    // Simple Gaussian-like histogram centered at `mean` (0-255 bin index).
    std::vector<double> h(256, 0.0);
    int center = static_cast<int>(mean * 255);
    for (int i = 0; i < 256; ++i) {
        double d = (i - center) / 32.0;
        h[i] = std::exp(-d * d);
    }
    // Normalize.
    double sum = 0;
    for (double v : h) sum += v;
    for (double& v : h) v /= sum;
    return h;
}
}

int main() {
    using namespace veyra::core;

    // Baseline: first frame establishes reference.
    {
        SceneCadenceAnalyzer a;
        auto r = a.analyze(1, makeHistogram(0.5), 0.0, 0);
        check("scene:first-no-cut", !r.isSceneCut && !r.isFlash && !r.isDuplicate, "first frame is baseline");
    }

    // Scene cut: very different histogram + high SAD.
    {
        SceneCadenceAnalyzer a;
        (void)a.analyze(1, makeHistogram(0.2), 0.0, 0);
        auto r = a.analyze(2, makeHistogram(0.8), 0.6, 33333);
        check("scene:cut-detected", r.isSceneCut, "different histogram + high SAD = cut");
        check("scene:cut-counted", a.sceneCutCount() == 1, "count=1");
    }

    // Moderate-luma cuts were previously missed by the mandatory SAD > .3.
    // Disjoint point histograms are deliberately independent of the generator.
    {
        std::vector<double> dark(256,0), nearby(256,0), partial(256,0);
        dark[100]=1; nearby[130]=1; partial[100]=.2;partial[130]=.8;
        SceneCadenceAnalyzer a;
        (void)a.analyze(0,dark,0,0);
        auto r=a.analyze(1,nearby,30.0/255,16667);
        check("scene:moderate-luma-cut",r.isSceneCut,"histogram replacement and SAD below old .3 threshold");
        a.reset();(void)a.analyze(0,dark,0,0);
        r=a.analyze(1,nearby,.079,16667);
        check("scene:small-change-not-cut",!r.isSceneCut,"disjoint histogram alone is insufficient");
        a.reset();(void)a.analyze(0,dark,0,0);
        r=a.analyze(1,partial,.15,16667);
        check("scene:partial-replacement-not-cut",!r.isSceneCut,"moderate SAD needs near-total replacement");
        a.reset();(void)a.analyze(0,dark,0,0);
        r=a.analyze(1,dark,.25,16667);
        check("scene:moving-texture-not-cut",!r.isSceneCut,"high motion with stable distribution is not a cut");
    }

    // Same scene: similar histogram + low SAD.
    {
        SceneCadenceAnalyzer a;
        (void)a.analyze(1, makeHistogram(0.5), 0.0, 0);
        auto r = a.analyze(2, makeHistogram(0.51), 0.02, 33333);
        check("scene:same-no-cut", !r.isSceneCut, "similar = no cut");
    }

    // Flash: high SAD but similar histogram.
    {
        SceneCadenceAnalyzer a;
        (void)a.analyze(1, makeHistogram(0.5), 0.0, 0);
        // Flash: brightness changes but content structure (histogram shape) is similar.
        // We simulate this with slightly shifted histogram + very high SAD.
        auto r = a.analyze(2, makeHistogram(0.505), 0.7, 33333);
        check("scene:flash-not-cut", !r.isSceneCut, "flash should not be cut");
        // The histogram distance for 0.5->0.55 should be moderate.
        // If it's below the flash threshold, it's a flash.
        if (r.histogramDistance < 0.5) {
            check("scene:flash-detected", r.isFlash, "flash detected");
        } else {
            // Histogram distance too high for flash threshold  might be classified as cut.
            // This is a known limitation of the simple Gaussian model.
            check("scene:flash-or-cut", r.isFlash || r.isSceneCut, "either flash or cut (threshold dependent)");
        }
    }

    // Duplicate: near-zero SAD.
    {
        SceneCadenceAnalyzer a;
        (void)a.analyze(1, makeHistogram(0.5), 0.0, 0);
        auto r = a.analyze(2, makeHistogram(0.5), 0.0001, 0);
        check("scene:duplicate-detected", r.isDuplicate, "zero SAD + zero PTS delta = duplicate");
        check("scene:dup-counted", a.duplicateCount() == 1, "dup count=1");
    }

    // Cadence break: PTS gap 3x normal.
    {
        SceneCadenceAnalyzer a;
        (void)a.analyze(1, makeHistogram(0.5), 0.0, 0);
        (void)a.analyze(2, makeHistogram(0.5), 0.01, 33333); // normal cadence
        auto r = a.analyze(3, makeHistogram(0.5), 0.01, 33333 + 150000); // 150ms gap
        check("scene:cadence-break", r.isCadenceBreak, "large PTS gap = cadence break");
    }

    // Frames the player skipped (capture mailbox drop / preview skip) are not a
    // source cadence break: a 100 ms gap over 6 source frames is 16.7 ms each.
    // Field log 2026-10-04: treating it as a break reset every ~100 ms and held
    // live capture at ~22 fps after the window lost focus.
    {
        SceneCadenceAnalyzer a;
        (void)a.analyze(1, makeHistogram(0.5), 0.0, 0, 1);
        (void)a.analyze(2, makeHistogram(0.5), 0.01, 16667, 1);
        auto skipped = a.analyze(3, makeHistogram(0.5), 0.01, 16667 + 100000, 6);
        check("scene:skip-not-break", !skipped.isCadenceBreak, "100 ms over 6 source frames keeps cadence");
        auto next = a.analyze(4, makeHistogram(0.5), 0.01, 16667 + 100000 + 16667, 1);
        check("scene:after-skip-normal", !next.isCadenceBreak, "normal frame after a skip keeps cadence");
        auto jump = a.analyze(5, makeHistogram(0.5), 0.01, 16667 + 100000 + 16667 + 400000, 1);
        check("scene:real-gap-still-breaks", jump.isCadenceBreak, "a real 400 ms source gap still breaks cadence");
    }
    // A duplicate (sub-millisecond PTS delta) must not make the next normal
    // interval look like a 3x jump.
    {
        SceneCadenceAnalyzer a;
        (void)a.analyze(1, makeHistogram(0.5), 0.0, 0);
        (void)a.analyze(2, makeHistogram(0.5), 0.01, 16667);
        (void)a.analyze(3, makeHistogram(0.5), 0.0001, 16667 + 500);
        auto r = a.analyze(4, makeHistogram(0.5), 0.01, 33334);
        check("scene:duplicate-no-false-break", !r.isCadenceBreak, "interval after a duplicate compares with the last real interval");
    }

    // Log50: Elgato4KPro delivers a positive 1ms PTS increment periodically,
    // surrounded by ordinary 20.833ms (or 16.683ms) intervals. It is not a
    // duplicate at the 1ms boundary, even for a static picture. Returning to
    // normal must not reset the entire temporal graph.
    for (uint64_t interval : {20833u, 16683u}) {
        for (double sad : {0.0, 0.01}) {
            SceneCadenceAnalyzer a;
            const auto histogram = makeHistogram(0.5);
            uint64_t pts = 0;
            unsigned breaks = 0;
            (void)a.analyze(0, histogram, sad, pts);
            for (uint64_t frame = 1; frame <= 6060; ++frame) {
                pts += frame % 101 == 0 ? 1000 : interval;
                const auto r = a.analyze(frame, histogram, sad, pts);
                breaks += r.isCadenceBreak;
            }
            check("scene:log50-short-pts-no-reset", breaks == 0,
                  "periodic 1ms timestamp cannot poison the next normal interval");
            const auto gap = a.analyze(6061, histogram, sad, pts + 200000);
            check("scene:log50-real-stall-still-breaks", gap.isCadenceBreak,
                  "robust baseline still detects a real 200ms source stall");
        }
    }
    {
        SceneCadenceAnalyzer a;
        const auto histogram = makeHistogram(0.5);
        uint64_t pts = 0;
        (void)a.analyze(0, histogram, 0.01, pts);
        for (uint64_t frame = 1; frame <= 8; ++frame)
            (void)a.analyze(frame, histogram, 0.01, pts += 16683);
        (void)a.analyze(9, histogram, 0.01, pts += 1000);
        (void)a.analyze(10, histogram, 0.01, pts += 2000);
        check("scene:short-pts-burst-no-reset",
              !a.analyze(11, histogram, 0.01, pts += 16683).isCadenceBreak,
              "two timestamp outliers still cannot establish a new cadence");
        check("scene:short-pts-mailbox-skip",
              !a.analyze(12, histogram, 0.01, pts += 16683 * 6, 6).isCadenceBreak,
              "sourceStep normalization remains effective after timestamp jitter");
    }
    {
        SceneCadenceAnalyzer a;
        const auto histogram = makeHistogram(0.5);
        uint64_t pts = 0;
        bool continuous = true;
        (void)a.analyze(0, histogram, 0.01, pts);
        for (uint64_t frame = 1; frame <= 2400; ++frame) {
            // 48/120/60/90Hz VRR is a real variable cadence, not a 60Hz stream.
            const uint64_t interval[] = {20833, 8333, 16667, 11111};
            continuous &= !a.analyze(frame, histogram, 0.01,
                                    pts += interval[frame % 4]).isCadenceBreak;
        }
        check("scene:vrr-cadence-preserved", continuous,
              "variable 48-120Hz input remains continuous");
        a.reset();
        (void)a.analyze(0, histogram, 0.01, 0);
        // Establish a genuinely faster source after reconnect, then stall.
        (void)a.analyze(1, histogram, 0.01, 2000);
        check("scene:reset-clears-robust-cadence",
              a.analyze(2, histogram, 0.01, 22000).isCadenceBreak,
              "no old slow cadence can mask a break in a fresh fast stream");
    }

    // Reset clears baseline.
    {
        SceneCadenceAnalyzer a;
        (void)a.analyze(1, makeHistogram(0.2), 0.0, 0);
        a.reset();
        auto r = a.analyze(2, makeHistogram(0.8), 0.6, 33333);
        check("scene:reset-no-cut", !r.isSceneCut, "after reset, first frame is new baseline");
    }

    std::printf("SCENE-TESTS: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
