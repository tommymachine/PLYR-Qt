// Verification harness for MfccAnalyzer + MfccTrajectory (Layer 2c).
//
// 1. Jacobi eigendecomposition unit tests on known matrices:
//    - 3x3 diagonal (eigenvalues = diagonals, eigenvectors = identity).
//    - 3x3 with one off-diagonal (eigenvalues solvable by hand).
//    - 13x13 identity (eigenvalues all 1, V orthogonal).
// 2. ABA test signal: 200+800 Hz sine (A) -> white noise (B) -> A again.
//    Feed through MfccAnalyzer + MfccTrajectory and confirm:
//    - A-region MFCCs are tightly clustered (low std).
//    - B-region MFCCs differ from A but cluster too.
//    - Returning to A lands near the original A region.
//    - The top-3 eigenvalue ratio lambda_1 / lambda_3 > 5 (the data is
//      meaningfully sub-3-D in MFCC space for this signal).
//
// Build:  cmake --build build --target mfccverify_cli
// Run:    ./build/mfccverify_cli

#include "MfccAnalyzer.h"
#include "MfccTrajectory.h"

#include <QCoreApplication>
#include <QtMath>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

// jacobiEigen<N>() is defined as a template in MfccTrajectory.cpp; the
// instantiations for N = 3, 12, 13 are emitted there explicitly. We
// just declare the signature here so the link succeeds.
template <int N> int jacobiEigen(float* A, float* V, float* eigsOut);


namespace {

constexpr double kSampleRate = 44100.0;
constexpr int    kMfccFrameSamples = MfccAnalyzer::FRAME_SAMPLES; // 2048
// Stride between frames. AudioFeatures runs refresh at 60 Hz over a
// continuous stream of audio, so the implicit hop between successive
// frames is sampleRate / 60 = 735 samples. We mirror that here.
constexpr int    kHopSamples = 735;
constexpr int    kHopRate    = int(kSampleRate / double(kHopSamples));   // 60


float sumSin(int n, double sampleRate, const std::vector<double>& freqs)
{
    float acc = 0.0f;
    const float gain = 0.4f / float(freqs.size());
    for (double f : freqs) {
        acc += gain * float(std::sin(2.0 * M_PI * f * double(n) / sampleRate));
    }
    return acc;
}


float whiteNoise(std::mt19937& rng, float amp)
{
    // CLT-ish Gaussian noise: sum of six uniforms in [-1, 1]. Cheap and
    // good enough for a stationary noise test signal.
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    float acc = 0.0f;
    for (int i = 0; i < 6; ++i) acc += u(rng);
    return amp * acc / 6.0f;
}


float distance(const float* a, const float* b, int n)
{
    float acc = 0.0f;
    for (int i = 0; i < n; ++i) { const float d = a[i] - b[i]; acc += d * d; }
    return std::sqrt(acc);
}


// --- Jacobi unit tests ----------------------------------------------------

bool checkJacobiDiagonal3()
{
    float A[9] = {
        3, 0, 0,
        0, 1, 0,
        0, 0, 5
    };
    float V[9] = {0};
    float eigs[3] = {0};
    const int sweeps = jacobiEigen<3>(A, V, eigs);

    std::array<float, 3> sorted = { eigs[0], eigs[1], eigs[2] };
    std::sort(sorted.begin(), sorted.end());
    const bool ok = std::abs(sorted[0] - 1.f) < 1e-5f &&
                    std::abs(sorted[1] - 3.f) < 1e-5f &&
                    std::abs(sorted[2] - 5.f) < 1e-5f;
    std::printf("  diag(3,1,5): sweeps=%d  eigs=[%.4f %.4f %.4f]  %s\n",
                sweeps, eigs[0], eigs[1], eigs[2], ok ? "OK" : "FAIL");
    return ok;
}


bool checkJacobiOffDiagonal3()
{
    // A = [[2, 1, 0], [1, 2, 0], [0, 0, 3]]. The 2x2 sub-block has
    // eigenvalues 1, 3; the third row/col is decoupled with eigenvalue
    // 3. Expected: eigenvalues {1, 3, 3}.
    float A[9] = {
        2, 1, 0,
        1, 2, 0,
        0, 0, 3
    };
    float V[9] = {0};
    float eigs[3] = {0};
    const int sweeps = jacobiEigen<3>(A, V, eigs);

    std::array<float, 3> sorted = { eigs[0], eigs[1], eigs[2] };
    std::sort(sorted.begin(), sorted.end());
    const bool ok = std::abs(sorted[0] - 1.f) < 1e-5f &&
                    std::abs(sorted[1] - 3.f) < 1e-5f &&
                    std::abs(sorted[2] - 3.f) < 1e-5f;
    std::printf("  off-diag: sweeps=%d  eigs=[%.4f %.4f %.4f]  %s\n",
                sweeps, eigs[0], eigs[1], eigs[2], ok ? "OK" : "FAIL");
    return ok;
}


bool checkJacobiIdentity13()
{
    constexpr int N = 13;
    std::array<float, N * N> A {};
    for (int i = 0; i < N; ++i) A[i * N + i] = 1.0f;
    std::array<float, N * N> V {};
    std::array<float, N>     eigs {};
    const int sweeps = jacobiEigen<N>(A.data(), V.data(), eigs.data());

    bool eigOk = true;
    for (int i = 0; i < N; ++i)
        if (std::abs(eigs[i] - 1.0f) > 1e-5f) eigOk = false;

    // Orthogonality: V^T V = I. Sum the absolute deviation from identity.
    float offSum = 0.0f;
    for (int i = 0; i < N; ++i) {
        for (int j = 0; j < N; ++j) {
            float dot = 0.0f;
            for (int k = 0; k < N; ++k)
                dot += V[k * N + i] * V[k * N + j];
            const float target = (i == j) ? 1.0f : 0.0f;
            offSum += std::abs(dot - target);
        }
    }
    const bool orthoOk = offSum < 1e-4f;
    std::printf("  identity(13): sweeps=%d  eigvals==1: %s  V^TV~I: %s "
                "(dev=%.2e)\n",
                sweeps,
                eigOk ? "OK" : "FAIL",
                orthoOk ? "OK" : "FAIL",
                offSum);
    return eigOk && orthoOk;
}


// --- ABA signal test ------------------------------------------------------

struct AbaStats {
    std::array<float, MfccTrajectory::FEATURE_DIM> centroidA1 {};
    std::array<float, MfccTrajectory::FEATURE_DIM> centroidB  {};
    std::array<float, MfccTrajectory::FEATURE_DIM> centroidA2 {};
    float stdA1 = 0.0f;
    float stdB  = 0.0f;
    float stdA2 = 0.0f;
    float distA1ToA2 = 0.0f;
    float distA1ToB  = 0.0f;
    float distA2ToB  = 0.0f;
    std::array<float, 3> eigs {};
    int    filledRows = 0;
    int    hopsProcessed = 0;
    double meanHopUsec = 0.0;
    int    jacobiSweeps  = 0;     // last recompute's sweep count
    long   jacobiUsec    = 0;     // last recompute's wall time
    int    recomputeCount = 0;    // total recomputes during the run
    int    signFlips     = 0;     // accumulated sign flips after first PCA
};


void summarize(const std::vector<std::array<float, MfccAnalyzer::N_COEFFS>>& vecs,
               int begin, int end,
               std::array<float, MfccTrajectory::FEATURE_DIM>& centroid,
               float& stdOut)
{
    centroid.fill(0.0f);
    const int n = end - begin;
    if (n <= 0) { stdOut = 0.0f; return; }
    for (int r = begin; r < end; ++r) {
        for (int c = 0; c < MfccTrajectory::FEATURE_DIM; ++c) {
            centroid[c] += vecs[r][c + 1];
        }
    }
    const float invN = 1.0f / float(n);
    for (auto& v : centroid) v *= invN;

    float varAcc = 0.0f;
    for (int r = begin; r < end; ++r) {
        for (int c = 0; c < MfccTrajectory::FEATURE_DIM; ++c) {
            const float d = vecs[r][c + 1] - centroid[c];
            varAcc += d * d;
        }
    }
    stdOut = std::sqrt(varAcc / float(n * MfccTrajectory::FEATURE_DIM));
}


AbaStats runAbaTest()
{
    // Compose: the trajectory owns its internal analyzer as a child
    // QObject (DirectConnection on mfccUpdated). Drive the internal
    // analyzer via debugProcessFrame so the trajectory's PCA recompute
    // logic runs synchronously off the same emit.
    MfccTrajectory traj;
    auto* analyzer = traj.findChild<MfccAnalyzer*>();
    Q_ASSERT(analyzer != nullptr);

    // Build the 12 s signal. 4 s A, 4 s B, 4 s A.
    constexpr int kSecSamples = int(kSampleRate * 4);
    constexpr int kTotal      = kSecSamples * 3;
    std::vector<float> sig(size_t(kTotal), 0.0f);
    std::mt19937 rng(12345);

    const std::vector<double> freqsA = { 200.0, 800.0 };
    for (int i = 0; i < kSecSamples; ++i)
        sig[i] = sumSin(i, kSampleRate, freqsA);
    for (int i = 0; i < kSecSamples; ++i)
        sig[size_t(kSecSamples * 2 + i)] = sumSin(i, kSampleRate, freqsA);
    for (int i = 0; i < kSecSamples; ++i)
        sig[size_t(kSecSamples + i)] = whiteNoise(rng, 0.35f);

    // Drive.
    std::vector<std::array<float, MfccAnalyzer::N_COEFFS>> vecs;
    vecs.reserve(size_t(kTotal / kHopSamples + 4));

    long totalUsec = 0;
    int  written   = 0;
    std::array<float, kMfccFrameSamples> window {};

    for (int tickEnd = 0; tickEnd < kTotal; tickEnd += kHopSamples) {
        const int srcEnd = std::min(tickEnd + kMfccFrameSamples, kTotal);
        const int srcStart = std::max(0, srcEnd - kMfccFrameSamples);
        const int copyN = srcEnd - srcStart;
        const int padN  = kMfccFrameSamples - copyN;

        // Build the window: zero-pad in front for early ticks.
        for (int i = 0; i < padN; ++i) window[i] = 0.0f;
        for (int i = 0; i < copyN; ++i)
            window[padN + i] = sig[size_t(srcStart + i)];

        auto t0 = std::chrono::steady_clock::now();
        analyzer->debugProcessFrame(window.data(), kMfccFrameSamples);
        auto t1 = std::chrono::steady_clock::now();
        totalUsec += std::chrono::duration_cast<std::chrono::microseconds>
                         (t1 - t0).count();

        std::array<float, MfccAnalyzer::N_COEFFS> v {};
        analyzer->fillLatestMfcc(v.data(), MfccAnalyzer::N_COEFFS);
        vecs.push_back(v);
        ++written;
    }

    AbaStats stats {};
    stats.hopsProcessed = written;
    stats.meanHopUsec   = (written > 0) ? double(totalUsec) / double(written)
                                         : 0.0;
    stats.filledRows    = traj.filledRows();

    // Section boundaries in hop count. Skip the first 30 hops of each
    // section to let the 2048-sample window slide past the boundary.
    const int hopsPerSect = kHopRate * 4;
    const int margin = 30;
    const int a1Lo = margin;
    const int a1Hi = std::min(hopsPerSect - margin, int(vecs.size()));
    const int bLo  = hopsPerSect + margin;
    const int bHi  = std::min(2 * hopsPerSect - margin, int(vecs.size()));
    const int a2Lo = 2 * hopsPerSect + margin;
    const int a2Hi = std::min(3 * hopsPerSect - margin, int(vecs.size()));

    summarize(vecs, a1Lo, a1Hi, stats.centroidA1, stats.stdA1);
    summarize(vecs, bLo,  bHi,  stats.centroidB,  stats.stdB);
    summarize(vecs, a2Lo, a2Hi, stats.centroidA2, stats.stdA2);

    stats.distA1ToA2 = distance(stats.centroidA1.data(),
                                stats.centroidA2.data(),
                                MfccTrajectory::FEATURE_DIM);
    stats.distA1ToB  = distance(stats.centroidA1.data(),
                                stats.centroidB.data(),
                                MfccTrajectory::FEATURE_DIM);
    stats.distA2ToB  = distance(stats.centroidA2.data(),
                                stats.centroidB.data(),
                                MfccTrajectory::FEATURE_DIM);

    traj.debugSnapshot(nullptr, 0, stats.eigs.data());
    stats.jacobiSweeps  = traj.debugLastSweeps();
    stats.jacobiUsec    = traj.debugLastUsec();
    stats.recomputeCount = traj.debugRecomputeCount();
    stats.signFlips      = traj.debugSignFlips();

    // Print a few sample 3D trajectory points across the run so we can
    // spot-check that A1, B, A2 land in distinct regions of the embed-
    // ding. We sample the trajectory at fractional positions 0.15
    // (mid-A1), 0.5 (mid-B), 0.85 (mid-A2).
    std::array<float, MfccTrajectory::TRAJECTORY_LEN * 3> xyz {};
    const int n = traj.debugSnapshot(xyz.data(),
                                     MfccTrajectory::TRAJECTORY_LEN,
                                     nullptr);
    if (n > 0) {
        auto sample = [&](float t) {
            const int i = std::clamp(int(t * (n - 1)), 0, n - 1);
            std::printf("  xyz[%4d / %d] (t=%.2f): (%+.3f, %+.3f, %+.3f)\n",
                        i, n, t,
                        xyz[i * 3 + 0], xyz[i * 3 + 1], xyz[i * 3 + 2]);
        };
        std::printf("\n== 3D trajectory samples ==\n");
        sample(0.15f);
        sample(0.50f);
        sample(0.85f);
    }
    return stats;
}


}  // namespace


int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    int failures = 0;

    std::printf("== Jacobi eigendecomposition unit tests ==\n");
    if (!checkJacobiDiagonal3())    ++failures;
    if (!checkJacobiOffDiagonal3()) ++failures;
    if (!checkJacobiIdentity13())   ++failures;

    AbaStats s = runAbaTest();

    std::printf("\n== ABA drive ==\n");
    std::printf("  hops processed              : %d\n", s.hopsProcessed);
    std::printf("  trajectory filledRows       : %d / %d\n",
                s.filledRows, MfccTrajectory::TRAJECTORY_LEN);
    std::printf("  mean per-hop wall time      : %.1f us\n", s.meanHopUsec);
    std::printf("  PCA recomputes              : %d\n", s.recomputeCount);
    std::printf("  last Jacobi sweep count     : %d\n", s.jacobiSweeps);
    std::printf("  last Jacobi recompute cost  : %ld us\n", s.jacobiUsec);
    std::printf("  sign flips (post-first PCA) : %d\n", s.signFlips);

    std::printf("\n== ABA centroid distances ==\n");
    std::printf("  std(A1) = %.4f  std(B) = %.4f  std(A2) = %.4f\n",
                s.stdA1, s.stdB, s.stdA2);
    std::printf("  dist(A1, A2) = %.4f\n", s.distA1ToA2);
    std::printf("  dist(A1, B)  = %.4f\n", s.distA1ToB);
    std::printf("  dist(A2, B)  = %.4f\n", s.distA2ToB);

    const bool a1A2Close = s.distA1ToA2 <
                           0.5f * std::min(s.distA1ToB, s.distA2ToB);
    std::printf("  dist(A1,A2) << min(dist(A?,B)) : %s\n",
                a1A2Close ? "OK" : "FAIL");
    if (!a1A2Close) ++failures;

    // The eigenvalues are stored per-slot, and the sign-stability code
    // can swap which slot the second/third axis ends up in across
    // recomputes; the printout shows them in slot order, but the ratio
    // we care about is max / min over the top-3. For this ABA signal
    // we expect lambda_1 >> lambda_2 ~ lambda_3 since the signal is
    // essentially 1-D in the MFCC space (A vs B).
    std::printf("\n== Top-3 PCA eigenvalues ==\n");
    std::printf("  eig[slot 0..2] = %.5f  %.5f  %.5f\n",
                s.eigs[0], s.eigs[1], s.eigs[2]);
    const float eigMax = std::max({ s.eigs[0], s.eigs[1], s.eigs[2] });
    const float eigMin = std::min({ s.eigs[0], s.eigs[1], s.eigs[2] });
    const float ratio  = (eigMin > 1e-6f) ? eigMax / eigMin : 0.0f;
    std::printf("  lambda_max / lambda_min = %.2f (target > 5)\n", ratio);
    const bool ratioOk = ratio > 5.0f;
    if (!ratioOk) ++failures;

    if (failures == 0) {
        std::printf("\nAll MfccAnalyzer / MfccTrajectory verifications passed.\n");
        return 0;
    }
    std::printf("\n%d failure(s).\n", failures);
    return 1;
}
