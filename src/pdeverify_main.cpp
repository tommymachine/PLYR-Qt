// pdeverify_cli -- Layer 4d Gray-Scott solver verification.
//
// Runs a CPU reference implementation of the Gray-Scott PDE on a
// 64x64 grid using the same forward-Euler integration the shader
// uses, with the same default parameters:
//     Du = 0.16, Dv = 0.08, F = 0.030, k = 0.062, dt = 1.0
// After 1000 substeps, computes the variance of `v` across the grid
// and asserts that it has grown meaningfully from the (near-zero
// because the seed is a single tiny disc) initial state.
//
// Reference: Pearson 1993, "Complex Patterns in a Simple System",
// Science 261. Gray-Scott PDE math is uncopyrightable.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

constexpr int kN = 64;
constexpr int kSteps = 1000;

inline int wrap(int i, int n)
{
    // Toroidal addressing; same wrap the shader's REPEAT sampler does.
    if (i < 0)   return i + n;
    if (i >= n)  return i - n;
    return i;
}

}  // namespace

int main(int /*argc*/, char* /*argv*/[])
{
    std::vector<float> u(kN * kN, 1.0f);
    std::vector<float> v(kN * kN, 0.0f);
    std::vector<float> uNext(kN * kN);
    std::vector<float> vNext(kN * kN);

    // Centered seed disc, same shape buildGsSeed paints.
    const int cx = kN / 2;
    const int cy = kN / 2;
    const int seedR = 4;
    for (int y = 0; y < kN; ++y) {
        for (int x = 0; x < kN; ++x) {
            const int dx = x - cx;
            const int dy = y - cy;
            if (dx * dx + dy * dy <= seedR * seedR) {
                u[y * kN + x] = 0.5f;
                v[y * kN + x] = 0.5f;
            }
        }
    }

    const float Du = 0.16f;
    const float Dv = 0.08f;
    const float F  = 0.030f;
    const float k  = 0.062f;
    const float dt = 1.0f;

    for (int step = 0; step < kSteps; ++step) {
        for (int y = 0; y < kN; ++y) {
            const int ym = wrap(y - 1, kN);
            const int yp = wrap(y + 1, kN);
            for (int x = 0; x < kN; ++x) {
                const int xm = wrap(x - 1, kN);
                const int xp = wrap(x + 1, kN);
                const int i  = y * kN + x;
                const float uu = u[i];
                const float vv = v[i];
                const float lapU = u[y * kN + xm] + u[y * kN + xp]
                                 + u[ym * kN + x] + u[yp * kN + x]
                                 - 4.0f * uu;
                const float lapV = v[y * kN + xm] + v[y * kN + xp]
                                 + v[ym * kN + x] + v[yp * kN + x]
                                 - 4.0f * vv;
                const float uv2  = uu * vv * vv;
                const float du   = Du * lapU - uv2 + F * (1.0f - uu);
                const float dv   = Dv * lapV + uv2 - (F + k) * vv;
                uNext[i] = std::clamp(uu + dt * du, 0.0f, 1.0f);
                vNext[i] = std::clamp(vv + dt * dv, 0.0f, 1.0f);
            }
        }
        std::swap(u, uNext);
        std::swap(v, vNext);
    }

    // Variance + simple cell-count stats.
    double mean = 0.0;
    for (float x : v) mean += x;
    mean /= double(kN * kN);

    double var = 0.0;
    double vMin = 1e9, vMax = -1e9;
    int    cells_high = 0;       // v > 0.3 -> centre of a stable spot
    int    cells_low  = 0;       // v < 0.05 -> empty
    for (float x : v) {
        const double d = double(x) - mean;
        var += d * d;
        vMin = std::min(vMin, double(x));
        vMax = std::max(vMax, double(x));
        if (x > 0.3f)  ++cells_high;
        if (x < 0.05f) ++cells_low;
    }
    var /= double(kN * kN);

    std::printf("Gray-Scott CPU reference: %d steps on %dx%d grid\n",
                kSteps, kN, kN);
    std::printf("  F = %.3f  k = %.3f  Du = %.3f  Dv = %.3f  dt = %.2f\n",
                double(F), double(k), double(Du), double(Dv), double(dt));
    std::printf("  v mean    = %.6f\n", mean);
    std::printf("  v variance= %.6f\n", var);
    std::printf("  v min/max = %.6f / %.6f\n", vMin, vMax);
    std::printf("  cells v>0.3:  %d / %d (%.1f%%)\n",
                cells_high, kN * kN, 100.0 * cells_high / (kN * kN));
    std::printf("  cells v<0.05: %d / %d (%.1f%%)\n",
                cells_low,  kN * kN, 100.0 * cells_low  / (kN * kN));

    // PASS criteria (per the layer plan):
    //   * variance grew meaningfully (> 0.005) -- the initial state
    //     has variance ~0.0005 from the small seed disc, so we want
    //     at least an order of magnitude growth.
    //   * pattern formed: at least 10 cells have v > 0.3 (i.e. spots
    //     have crystallised somewhere) AND a meaningful chunk of the
    //     grid is still empty (the "spots on empty background"
    //     signature, not "uniform high-v collapse").
    bool pass = true;
    if (var < 0.005) {
        std::printf("FAIL: variance %.6f is too low (expected >0.005)\n", var);
        pass = false;
    }
    if (cells_high < 10) {
        std::printf("FAIL: too few high-v cells (%d) -- no spots formed\n",
                    cells_high);
        pass = false;
    }
    if (cells_low < (kN * kN) / 4) {
        std::printf("FAIL: pattern collapsed -- only %d empty cells\n",
                    cells_low);
        pass = false;
    }
    if (!std::isfinite(vMin) || !std::isfinite(vMax)) {
        std::printf("FAIL: NaN or infinity in solution\n");
        pass = false;
    }

    std::printf("\n%s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
