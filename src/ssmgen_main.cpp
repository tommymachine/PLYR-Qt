// ssmgen_cli -- offline self-similarity-matrix generator (Layer 4b).
//
//   ssmgen_cli some.flac          : write some.flac.ssm
//   ssmgen_cli some.flac out.ssm  : write out.ssm
//   ssmgen_cli --selftest         : synthetic ABA structure unit test
//                                   (no FLAC needed). Confirms that:
//                                     * Diagonal blocks of similar
//                                       material light up bright.
//                                     * Off-diagonal stripes appear
//                                       where section A recurs after B.
//                                     * Sidecar round-trip preserves
//                                       T, hopSec, and all bytes.
//
// Eventually RipWorker calls into this same code path (via SsmGenerator
// directly, not the CLI) after FLAC encoding completes -- so every
// finished rip ends up with a .ssm sidecar next to the .flac. That
// integration is left for a follow-up; this binary is the canonical
// offline entry point in the meantime.

#include "SsmGenerator.h"
#include "FlacDecode.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>


namespace {

constexpr double kPi = 3.14159265358979323846;


// --- Synthetic test signal -------------------------------------------------
//
// 30 s ABA layout at 44.1 kHz: 10 s sine-A, 10 s noise-B, 10 s sine-A.
// The A region is a 220 + 880 Hz dyad (octave + perfect fifth would be
// boring); the B region is band-limited white noise. The SSM should
// show:
//   * 10x10 s bright block on the diagonal at A1.
//   * 10x10 s bright block on the diagonal at A2.
//   * 10x10 s bright off-diagonal block at A1xA2 (and symmetric A2xA1).
//   * 10x10 s bright block on the diagonal at B.
//   * Dark cross-bands between A and B (noise has very different MFCC
//     fingerprint from sine harmonics).

std::vector<float> buildAbaSignal(double sampleRate) {
    constexpr int kSecondsPerSection = 10;
    const int sectionSamples = int(sampleRate * kSecondsPerSection);
    const int total = sectionSamples * 3;

    std::vector<float> out(size_t(total), 0.0f);
    std::mt19937 rng(0xA1BA);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);

    auto sineA = [&](int n) {
        const double t = double(n) / sampleRate;
        return 0.3f * (float(std::sin(2.0 * kPi * 220.0 * t)) +
                       float(std::sin(2.0 * kPi * 880.0 * t)));
    };

    auto noiseB = [&]() {
        // CLT-ish gaussian via sum of 6 uniforms.
        float acc = 0.0f;
        for (int k = 0; k < 6; ++k) acc += u(rng);
        return 0.4f * acc / 6.0f;
    };

    for (int i = 0; i < sectionSamples; ++i)
        out[size_t(i)] = sineA(i);
    for (int i = 0; i < sectionSamples; ++i)
        out[size_t(sectionSamples + i)] = noiseB();
    for (int i = 0; i < sectionSamples; ++i)
        out[size_t(2 * sectionSamples + i)] = sineA(i);
    return out;
}


// --- Region statistics ----------------------------------------------------

struct RegionStats {
    int   T          = 0;
    float hopSec     = 0.0f;
    float a1xa1      = 0.0f;  // mean cell value inside A1 x A1
    float a1xb       = 0.0f;
    float a1xa2      = 0.0f;
    float bxb        = 0.0f;
    float a2xa2      = 0.0f;
};

// Average a rectangular region of the matrix (in [0, 255]).
float meanRegion(const std::vector<uint8_t>& mat, int T,
                 int rLo, int rHi, int cLo, int cHi) {
    rLo = std::max(0, rLo); rHi = std::min(T, rHi);
    cLo = std::max(0, cLo); cHi = std::min(T, cHi);
    const int n = (rHi - rLo) * (cHi - cLo);
    if (n <= 0) return 0.0f;
    double acc = 0.0;
    for (int r = rLo; r < rHi; ++r) {
        for (int c = cLo; c < cHi; ++c) {
            acc += double(mat[size_t(r) * size_t(T) + size_t(c)]);
        }
    }
    return float(acc / double(n));
}


// --- Self-test ------------------------------------------------------------
//
// Runs the in-memory ABA generator + region stats; runs the sidecar
// round-trip; prints results and returns 0 on success / non-zero on
// algorithmic failure.

int runSelfTest() {
    std::printf("== Layer 4b self-test (synthetic ABA structure) ==\n");

    const double sampleRate = 44100.0;
    auto sig = buildAbaSignal(sampleRate);
    std::printf("  built %.1f s mono signal at %.1f kHz (%zu samples)\n",
                double(sig.size()) / sampleRate, sampleRate / 1000.0, sig.size());

    std::vector<uint8_t> matrix;
    auto t0 = std::chrono::steady_clock::now();
    auto stats = SsmGenerator::generateFromMonoSamples(sig, sampleRate, matrix);
    auto t1 = std::chrono::steady_clock::now();
    const double wall = std::chrono::duration<double>(t1 - t0).count();
    if (!stats.success) {
        std::fprintf(stderr, "  FAILED to generate: %s\n",
                     stats.error.toLocal8Bit().constData());
        return 2;
    }
    std::printf("  T = %d, hopSec = %.4f\n", stats.T, double(stats.hopSec));
    std::printf("  wall: %.2f s  (mfcc=%.2fs sim=%.2fs)\n",
                wall, stats.mfccSec, stats.similaritySec);
    std::printf("  expected ~30/%.3f = %.1f frames; got T=%d\n",
                double(stats.hopSec), 30.0 / double(stats.hopSec), stats.T);

    // Region indices in frame coordinates. The hop is 0.5 s for our
    // 30-second test, so A1 = [0, 20), B = [20, 40), A2 = [40, 60).
    const int frames10s = int(std::round(10.0 / double(stats.hopSec)));
    // Trim a margin around the section boundaries so the MFCC window
    // (~46 ms) doesn't smear the boundary across the region edges.
    const int margin = 2;

    const int a1Lo = margin;
    const int a1Hi = frames10s - margin;
    const int bLo  = frames10s + margin;
    const int bHi  = 2 * frames10s - margin;
    const int a2Lo = 2 * frames10s + margin;
    const int a2Hi = 3 * frames10s - margin;

    RegionStats r;
    r.T      = stats.T;
    r.hopSec = stats.hopSec;
    r.a1xa1  = meanRegion(matrix, stats.T, a1Lo, a1Hi, a1Lo, a1Hi);
    r.a1xb   = meanRegion(matrix, stats.T, a1Lo, a1Hi, bLo,  bHi);
    r.a1xa2  = meanRegion(matrix, stats.T, a1Lo, a1Hi, a2Lo, a2Hi);
    r.bxb    = meanRegion(matrix, stats.T, bLo,  bHi,  bLo,  bHi);
    r.a2xa2  = meanRegion(matrix, stats.T, a2Lo, a2Hi, a2Lo, a2Hi);

    std::printf("\n  region means (in [0, 255]):\n");
    std::printf("    A1 x A1 = %6.1f (expected bright, ~210+)\n", r.a1xa1);
    std::printf("    A2 x A2 = %6.1f (expected bright, ~210+)\n", r.a2xa2);
    std::printf("    A1 x A2 = %6.1f (expected bright, ~200+)\n", r.a1xa2);
    std::printf("    B  x B  = %6.1f (expected bright-ish; noise self-sim)\n",
                r.bxb);
    std::printf("    A1 x B  = %6.1f (expected DARK, well below A1 x A2)\n",
                r.a1xb);

    int failures = 0;
    if (r.a1xa1 < 200.0f) {
        std::printf("  FAIL: A1xA1 too dark (%.1f < 200)\n", r.a1xa1); ++failures;
    }
    if (r.a2xa2 < 200.0f) {
        std::printf("  FAIL: A2xA2 too dark (%.1f < 200)\n", r.a2xa2); ++failures;
    }
    if (r.a1xa2 < 180.0f) {
        std::printf("  FAIL: A1xA2 too dark (%.1f < 180)\n", r.a1xa2); ++failures;
    }
    // The contrast that matters most: A1xA2 (recurrence) should be
    // dramatically brighter than A1xB (different segment).
    if (r.a1xa2 - r.a1xb < 40.0f) {
        std::printf("  FAIL: A1xA2 - A1xB contrast = %.1f, want >= 40\n",
                    r.a1xa2 - r.a1xb);
        ++failures;
    }
    if (r.a1xb >= r.a1xa2) {
        std::printf("  FAIL: A1xB (%.1f) >= A1xA2 (%.1f) -- segment vs "
                    "recurrence ordering reversed\n", r.a1xb, r.a1xa2);
        ++failures;
    }

    // --- Sidecar round-trip ------------------------------------------------
    const QString tmpPath = QStringLiteral("/tmp/ssmgen_selftest.ssm");
    QFile::remove(tmpPath);
    if (!SsmGenerator::writeSidecar(tmpPath, stats.T, stats.hopSec, matrix.data())) {
        std::printf("  FAIL: writeSidecar failed for %s\n",
                    tmpPath.toLocal8Bit().constData());
        ++failures;
    } else if (!SsmGenerator::isValidSidecar(tmpPath)) {
        std::printf("  FAIL: isValidSidecar returned false for %s\n",
                    tmpPath.toLocal8Bit().constData());
        ++failures;
    } else {
        SsmGenerator::SidecarHeader hdr;
        std::vector<uint8_t> readBack;
        if (!SsmGenerator::readSidecar(tmpPath, hdr, readBack)) {
            std::printf("  FAIL: readSidecar failed\n");
            ++failures;
        } else if (int(hdr.T) != stats.T) {
            std::printf("  FAIL: T mismatch on round-trip (%d -> %u)\n",
                        stats.T, hdr.T);
            ++failures;
        } else if (std::fabs(hdr.hopSec - stats.hopSec) > 1e-6f) {
            std::printf("  FAIL: hopSec mismatch on round-trip (%.6f -> %.6f)\n",
                        double(stats.hopSec), double(hdr.hopSec));
            ++failures;
        } else if (readBack != matrix) {
            std::printf("  FAIL: matrix bytes diverged on round-trip\n");
            ++failures;
        } else {
            const qint64 sz = QFileInfo(tmpPath).size();
            std::printf("\n  sidecar round-trip OK (%lld bytes on disk)\n",
                        sz);
        }
        QFile::remove(tmpPath);
    }

    if (failures == 0) {
        std::printf("\nAll Layer 4b self-tests passed.\n");
        return 0;
    }
    std::printf("\n%d failure(s).\n", failures);
    return 1;
}


// --- File mode ------------------------------------------------------------

int runFileMode(const QString& flacPath, const QString& outPath) {
    QFileInfo info(flacPath);
    if (!info.exists()) {
        std::fprintf(stderr, "ssmgen: %s: file not found\n",
                     flacPath.toLocal8Bit().constData());
        return 1;
    }
    std::printf("[ssmgen] %s (%lld bytes)\n",
                flacPath.toLocal8Bit().constData(), info.size());

    auto t0 = std::chrono::steady_clock::now();
    auto stats = SsmGenerator::generateForFile(flacPath, outPath);
    auto t1 = std::chrono::steady_clock::now();
    const double wall = std::chrono::duration<double>(t1 - t0).count();

    if (!stats.success) {
        std::fprintf(stderr, "ssmgen: FAILED: %s\n",
                     stats.error.toLocal8Bit().constData());
        return 1;
    }

    const QString finalOut = outPath.isEmpty()
                           ? (flacPath + QStringLiteral(".ssm"))
                           : outPath;
    const qint64 outSize = QFileInfo(finalOut).size();
    std::printf("[ssmgen] decoded in %.3fs\n", stats.decodeSec);
    std::printf("[ssmgen] T=%d, hopSec=%.3f, mfcc=%.3fs, similarity=%.3fs, "
                "write=%.3fs\n",
                stats.T, double(stats.hopSec),
                stats.mfccSec, stats.similaritySec, stats.writeSec);
    std::printf("[ssmgen] wrote %s (%.1f KB) in total %.3fs\n",
                finalOut.toLocal8Bit().constData(),
                double(outSize) / 1024.0, wall);
    return 0;
}

}  // namespace


int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    if (argc >= 2 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--selftest")) {
        return runSelfTest();
    }
    if (argc < 2) {
        std::fprintf(stderr,
            "usage: %s <input.flac> [output.ssm]\n"
            "       %s --selftest\n", argv[0], argv[0]);
        return 1;
    }
    const QString flacPath = QString::fromLocal8Bit(argv[1]);
    const QString outPath  = (argc >= 3) ? QString::fromLocal8Bit(argv[2])
                                          : QString();
    return runFileMode(flacPath, outPath);
}
