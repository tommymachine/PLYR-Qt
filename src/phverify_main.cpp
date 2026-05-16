// phverify_cli -- Layer 4a correctness harness.
//
// Drives PersistentHomologyAnalyzer's synchronous computation path
// with a small set of synthetic point clouds and asserts the resulting
// Vietoris-Rips persistent homology matches the textbook answer:
//
//   * 64-point circle in R^2 (radius 1):
//       - H0: exactly 1 unkillable bar (the eternal component)
//       - H1: at least 1 long bar with (death - birth) > 0.5
//         (the topological loop the points trace)
//
//   * 64-point 2D random walk with a deliberate self-intersection:
//       - H1: at least 1 noticeable bar (more noise than the clean
//         circle, but the loop should still register)
//
//   * 128-point trefoil knot in R^3:
//       - H1: at least 1 long bar (the knot is topologically a circle
//         when viewed as a 1-manifold, so its Betti_1 is 1)
//
// Also exercises the per-compute wall-clock so the layer report can
// quote N=64 / N=128 cost numbers without re-instrumenting the live UI.
//
// References (math, no copied code):
//   - Ulrich Bauer, "Ripser: efficient computation of Vietoris-Rips
//     persistence barcodes" (arXiv:1908.02518).
//   - Tralie & Perea 2017, "Sliding windows and persistence"
//     (arXiv:1703.04127).

#include "PersistentHomologyAnalyzer.h"

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>


namespace {

using Pair = PersistentHomologyAnalyzer::PersistencePair;

constexpr float kPi = 3.14159265358979323846f;


// Build n points sampled uniformly on a circle of given radius in R^2.
// Output: row-major (n x 2) floats.
std::vector<float> sampleCircle(int n, float radius)
{
    std::vector<float> out(size_t(n) * 2);
    for (int i = 0; i < n; ++i) {
        const float t = 2.0f * kPi * float(i) / float(n);
        out[size_t(i) * 2 + 0] = radius * std::cos(t);
        out[size_t(i) * 2 + 1] = radius * std::sin(t);
    }
    return out;
}


// Build an approximation of a noisy 2D random walk that loops on
// itself. We use a Lissajous figure (well-known to trace a closed
// curve) with a controllable amount of additive noise so the homology
// is non-trivial but the loop is still recoverable.
std::vector<float> sampleNoisyLissajous(int n, float noise)
{
    std::vector<float> out(size_t(n) * 2);
    // Use a fixed LCG so the test is reproducible.
    uint32_t s = 0x9E3779B9u;
    auto rand01 = [&]() -> float {
        s = s * 1664525u + 1013904223u;
        return float(s & 0xFFFFFFu) / float(0x1000000);
    };
    for (int i = 0; i < n; ++i) {
        const float t = 2.0f * kPi * float(i) / float(n);
        const float x = std::cos(3.0f * t);
        const float y = std::sin(2.0f * t);
        out[size_t(i) * 2 + 0] = x + noise * (rand01() - 0.5f);
        out[size_t(i) * 2 + 1] = y + noise * (rand01() - 0.5f);
    }
    return out;
}


// Build n points on a trefoil knot in R^3. Parametric form (standard):
//   x = sin(t) + 2 sin(2t)
//   y = cos(t) - 2 cos(2t)
//   z = -sin(3t)
// The knot is topologically a circle (Betti_1 = 1) so Ripser should
// recover one long-lived H1 class.
std::vector<float> sampleTrefoil(int n)
{
    std::vector<float> out(size_t(n) * 3);
    for (int i = 0; i < n; ++i) {
        const float t = 2.0f * kPi * float(i) / float(n);
        out[size_t(i) * 3 + 0] = std::sin(t) + 2.0f * std::sin(2.0f * t);
        out[size_t(i) * 3 + 1] = std::cos(t) - 2.0f * std::cos(2.0f * t);
        out[size_t(i) * 3 + 2] = -std::sin(3.0f * t);
    }
    return out;
}


struct Summary {
    int   nH0Killed   = 0;
    int   nH0Eternal  = 0;
    int   nH1         = 0;
    int   nH1Long     = 0;
    float maxH1Length = 0.0f;
    float maxH1Birth  = 0.0f;
    float maxH1Death  = 0.0f;
};

Summary summarize(const QVector<Pair>& pairs, float longThreshold)
{
    Summary s;
    for (const Pair& p : pairs) {
        const bool eternal = !std::isfinite(p.death);
        if (p.dim == 0) {
            if (eternal) s.nH0Eternal++;
            else         s.nH0Killed++;
        } else if (p.dim == 1) {
            s.nH1++;
            const float len = eternal ? std::numeric_limits<float>::infinity()
                                       : (p.death - p.birth);
            if (len > longThreshold) s.nH1Long++;
            if (std::isfinite(len) && len > s.maxH1Length) {
                s.maxH1Length = len;
                s.maxH1Birth  = p.birth;
                s.maxH1Death  = p.death;
            }
        }
    }
    return s;
}


// Run a single test with the given name, point cloud, and threshold.
// Returns true on PASS.
bool runTest(const char* name,
             PersistentHomologyAnalyzer& ph,
             const std::vector<float>& points,
             int dim,
             float threshold,
             int  minH1Long,
             float minH1Length)
{
    const int n = int(points.size()) / dim;
    QElapsedTimer timer;
    timer.start();
    const QVector<Pair> pairs = ph.computeFromPointCloudSync(
        points.data(), n, dim, threshold, /*dimMax=*/1);
    const qint64 elapsedUsec = timer.nsecsElapsed() / 1000;

    const Summary s = summarize(pairs, /*longThreshold=*/0.1f);

    std::printf("\n=== %s ===\n", name);
    std::printf("  N points : %d   dim : %d   threshold : %.4f\n",
                n, dim, double(threshold));
    std::printf("  compute  : %lld us (%.2f ms)\n",
                static_cast<long long>(elapsedUsec),
                double(elapsedUsec) / 1000.0);
    std::printf("  H0 pairs : %d killed + %d eternal\n",
                s.nH0Killed, s.nH0Eternal);
    std::printf("  H1 pairs : %d (long >= 0.1 : %d)\n",
                s.nH1, s.nH1Long);
    if (s.maxH1Length > 0) {
        std::printf("  longest H1 : [%.4f, %.4f)  length = %.4f\n",
                    double(s.maxH1Birth),
                    double(s.maxH1Death),
                    double(s.maxH1Length));
    }

    bool pass = true;
    if (s.nH0Eternal != 1) {
        std::printf("  FAIL: expected exactly 1 eternal H0 bar, got %d\n",
                    s.nH0Eternal);
        pass = false;
    }
    if (s.nH1Long < minH1Long) {
        std::printf("  FAIL: expected at least %d long H1 bars (>%.2f), got %d\n",
                    minH1Long, 0.1, s.nH1Long);
        pass = false;
    }
    if (s.maxH1Length < minH1Length) {
        std::printf("  FAIL: longest H1 bar = %.4f, expected >= %.4f\n",
                    double(s.maxH1Length), double(minH1Length));
        pass = false;
    }

    std::printf("  %s\n", pass ? "PASS" : "FAIL");
    return pass;
}


// Time a couple of N values so we have ground-truth perf numbers for
// the layer report.
void timingBench(PersistentHomologyAnalyzer& ph)
{
    std::printf("\n=== timing bench ===\n");
    for (int n : {32, 64, 96, 128}) {
        const auto pts = sampleCircle(n, 1.0f);
        QElapsedTimer timer;
        timer.start();
        const int kRuns = (n <= 64) ? 8 : 4;
        for (int r = 0; r < kRuns; ++r) {
            (void)ph.computeFromPointCloudSync(pts.data(), n, 2,
                                               /*threshold=*/2.5f,
                                               /*dimMax=*/1);
        }
        const qint64 totalUs = timer.nsecsElapsed() / 1000;
        std::printf("  N = %3d : %lld us total / %d runs = %.2f ms / run\n",
                    n, static_cast<long long>(totalUs), kRuns,
                    (double(totalUs) / kRuns) / 1000.0);
    }
}


}  // namespace


int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    PersistentHomologyAnalyzer ph;

    bool allPass = true;

    // Test 1: clean 64-point unit circle. Expected H1 length ~= 1.732
    // (the radius of the loop -- birth at the side length sqrt(2 - 2
    // cos(2pi/64)) ~ 0.098, death at the diameter 2.0).
    {
        const auto pts = sampleCircle(64, 1.0f);
        if (!runTest("circle / N=64 / R=1.0", ph, pts, /*dim=*/2,
                     /*threshold=*/3.0f,
                     /*minH1Long=*/1, /*minH1Length=*/0.5f))
            allPass = false;
    }

    // Test 2: smaller-scale circle (radius 0.5) -- catches scale
    // dependence in the threshold heuristic.
    {
        const auto pts = sampleCircle(64, 0.5f);
        if (!runTest("circle / N=64 / R=0.5", ph, pts, /*dim=*/2,
                     /*threshold=*/2.0f,
                     /*minH1Long=*/1, /*minH1Length=*/0.25f))
            allPass = false;
    }

    // Test 3: noisy Lissajous (k=3,k=2) in R^2 -- still a closed
    // curve, but with self-crossings and additive noise. We expect
    // at least one noticeable H1 bar.
    {
        const auto pts = sampleNoisyLissajous(96, /*noise=*/0.15f);
        if (!runTest("noisy lissajous (3,2) / N=96 / noise=0.15", ph, pts, /*dim=*/2,
                     /*threshold=*/2.5f,
                     /*minH1Long=*/1, /*minH1Length=*/0.3f))
            allPass = false;
    }

    // Test 4: trefoil knot in R^3. Topologically a circle: one long H1.
    {
        const auto pts = sampleTrefoil(96);
        if (!runTest("trefoil knot / N=96 / R^3", ph, pts, /*dim=*/3,
                     /*threshold=*/3.5f,
                     /*minH1Long=*/1, /*minH1Length=*/0.5f))
            allPass = false;
    }

    // Test 5: tiny degenerate -- 4 points on a square. Sanity check
    // for very small N.
    {
        std::vector<float> sq = {
            -1.0f, -1.0f,
             1.0f, -1.0f,
             1.0f,  1.0f,
            -1.0f,  1.0f
        };
        if (!runTest("square / N=4", ph, sq, /*dim=*/2,
                     /*threshold=*/3.0f,
                     /*minH1Long=*/1, /*minH1Length=*/0.4f))
            allPass = false;
    }

    timingBench(ph);

    std::printf("\n%s\n", allPass ? "ALL PASS" : "SOME FAIL");
    return allPass ? 0 : 1;
}
