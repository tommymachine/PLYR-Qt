// Verification harness for CqtAnalyzer.
//
// Feeds known-frequency sine tones through the analyzer and prints the
// peak-bin index + magnitudes around it. We expect:
//
//   440 Hz (A4): peak near bin 24*log2(440/32.7) = 89.7 -> ~89 or 90.
//   1000 Hz    : peak near bin 24*log2(1000/32.7) = 118.5 -> ~118 or 119.
//
// Anything more than ~2 bins off means the kernel construction or
// octave alignment is wrong.
//
// Build target produced by CMakeLists; run with:
//   ./build/cqtverify_cli

#include "CqtAnalyzer.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QtMath>
#include <array>
#include <cstdio>
#include <vector>

static int sineTest(double freqHz, double durationSec)
{
    constexpr double sampleRate = 44100.0;
    CqtAnalyzer analyzer(nullptr, /*B=*/24, /*nOct=*/8,
                         /*fMin=*/32.70, sampleRate, /*fft=*/4096);

    const int totalSamples = int(durationSec * sampleRate);
    std::vector<float> sig(totalSamples);
    for (int i = 0; i < totalSamples; ++i) {
        // Quieter amplitude (~ -34 dBFS) keeps the byte readout below
        // saturation so neighboring-bin magnitudes are visible.
        sig[i] = float(0.02 * std::sin(2.0 * M_PI * freqHz * double(i) / sampleRate));
    }

    // Push the signal in chunks (mimics the 60 Hz feed: ~735 samples
    // per push). Run a hop after each chunk; the analyzer only outputs
    // after enough samples have accumulated on every octave.
    constexpr int chunk = 735;
    QElapsedTimer timer;
    qint64 hopMicroAccum = 0;
    int    hopsRun = 0;
    for (int off = 0; off < totalSamples; off += chunk) {
        const int n = std::min(chunk, totalSamples - off);
        analyzer.pushSamples(sig.data() + off, n);
        timer.start();
        if (analyzer.computeHop()) {
            hopMicroAccum += timer.nsecsElapsed() / 1000;
            ++hopsRun;
        }
    }

    // Read the final magnitudes. fillRow returns dB-encoded bytes; for
    // a precise peak location we also expose the linear magnitudes via
    // a small back door: a friend test would be cleaner, but since this
    // CLI lives next to the analyzer and is dev-only, we re-derive by
    // looking at the largest byte. For sub-bin precision, we'd need
    // linear magnitudes; for the kernel correctness check we want, the
    // byte resolution is sufficient (peak should be EXACTLY at the
    // predicted bin +/- 1, not anywhere in a 16-bin window).
    std::array<uint8_t, CqtAnalyzer::MAX_BINS> bytes {};
    analyzer.fillRow(bytes.data(), analyzer.outputBins());

    int peakIdx = 0; int peakByte = 0;
    for (int i = 0; i < analyzer.outputBins(); ++i) {
        if (bytes[i] > peakByte) { peakByte = bytes[i]; peakIdx = i; }
    }
    const double expectedBin = 24.0 * std::log2(freqHz / 32.70);

    std::printf("\n== %d Hz sine (%.3fs) ==\n", int(freqHz), durationSec);
    std::printf("  expected peak bin: %.2f\n", expectedBin);
    std::printf("  actual   peak bin: %d  (byte=%d)\n", peakIdx, peakByte);
    std::printf("  offset           : %+.2f bins\n",
                double(peakIdx) - expectedBin);

    // Dump every bin so we can see which octaves are responding.
    std::printf("  full output (24 per row, one row per octave):\n");
    for (int oct = 0; oct < 8; ++oct) {
        std::printf("    oct %d [bin %3d-%3d]: ", oct, oct*24, oct*24+23);
        for (int k = 0; k < 24; ++k) {
            const int b = bytes[oct * 24 + k];
            if (b > 30) std::printf("%3d ", b);
            else        std::printf("  . ");
        }
        std::printf("\n");
    }

    if (hopsRun > 0) {
        std::printf("  per-hop cost     : %lld us avg over %d hops\n",
                    hopMicroAccum / hopsRun, hopsRun);
    }

    // Verdict.
    const double err = std::fabs(double(peakIdx) - expectedBin);
    if (err > 2.0) {
        std::printf("  FAIL: peak is >2 bins off the predicted location.\n");
        return 1;
    }
    std::printf("  OK\n");
    return 0;
}


int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // Building an analyzer above the test loop logs the sparsity once
    // via CqtAnalyzer::buildKernel's qInfo. Each sineTest below also
    // constructs an analyzer and emits the same line, but having one
    // here keeps the report self-contained even if sineTest is later
    // refactored to reuse a shared instance.
    {
        CqtAnalyzer probe(nullptr, 24, 8, 32.70, 44100.0, 4096);
        Q_UNUSED(probe);
    }

    int failures = 0;
    failures += sineTest(440.0,  1.0);
    failures += sineTest(1000.0, 1.0);
    // Lower pitches need longer test signals: their octave runs more
    // heavily decimated, so the analysis window covers more wall time.
    // C2 (~130 Hz) needs ~4096/(fs/32) = ~3 sec to fully populate the
    // octave-2 window. We use 4 sec to give the kernel margin.
    failures += sineTest(220.0,  4.0);
    failures += sineTest(2000.0, 1.0);

    if (failures == 0) {
        std::printf("\nAll sinusoid verifications passed.\n");
        return 0;
    }
    std::printf("\n%d failure(s).\n", failures);
    return 1;
}
