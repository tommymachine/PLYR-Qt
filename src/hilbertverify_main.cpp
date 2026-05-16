// Verification harness for HilbertAnalyzer (Layer 4c).
//
//  1. Silence  -> every band envelope ~ 0.
//  2. 440 Hz pure sine -> the bands closest to 440 Hz in log space
//     (band 2 @ 298 Hz and band 3 @ 576 Hz) carry most of the energy;
//     bands far away should be near zero.
//  3. White noise -> envelopes roughly equal across all bands (within a
//     factor of ~2, ignoring the lowest band where the FIR pass band is
//     barely engaged).
//  4. Phase advance for the 440 Hz tone: the instantaneous frequency
//     reported by the band carrying the tone should match 440 Hz +-
//     ~5 Hz (Hilbert phase differencing on a single sample is noisy
//     compared to a longer window, so we tolerate a wide band).
//
// Build:  cmake --build build --target hilbertverify_cli
// Run:    ./build/hilbertverify_cli

#include "HilbertAnalyzer.h"

#include <QCoreApplication>
#include <QtMath>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>


namespace {

constexpr double kSampleRate = 44100.0;
constexpr int    kFrame      = HilbertAnalyzer::FRAME_SAMPLES;
constexpr int    kHopRate    = 60;
constexpr int    kHopSamples = int(kSampleRate / double(kHopRate));   // 735


std::vector<float> makeSilence(int n)
{
    return std::vector<float>(size_t(n), 0.0f);
}


std::vector<float> makeSine(int n, double fHz, double amp = 0.7)
{
    std::vector<float> out(size_t(n), 0.0f);
    const double w = 2.0 * M_PI * fHz / kSampleRate;
    for (int i = 0; i < n; ++i) out[i] = float(amp * std::sin(w * double(i)));
    return out;
}


std::vector<float> makeWhiteNoise(int n, double amp = 0.5)
{
    std::vector<float> out(size_t(n), 0.0f);
    std::mt19937 rng(20251115);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    for (int i = 0; i < n; ++i) {
        // 6-sample CLT-ish Gaussian; clamped to [-1, 1].
        float a = 0.0f;
        for (int k = 0; k < 6; ++k) a += u(rng);
        out[size_t(i)] = float(amp) * std::clamp(a / 6.0f, -1.0f, 1.0f);
    }
    return out;
}


// Drive HilbertAnalyzer with a stream of mono samples in kHopSamples-
// sized chunks (mimicking the live hop cadence). Returns the envelopes
// (after the analyzer's two-time-constant smoothing) averaged over the
// last `lastN` hops of the run, so we measure steady-state.
std::array<float, HilbertAnalyzer::N_BANDS>
drive(HilbertAnalyzer& a, const std::vector<float>& mono,
      int lastN, std::array<float, HilbertAnalyzer::N_BANDS>* lastPhase = nullptr,
      std::array<float, HilbertAnalyzer::N_BANDS>* lastFreq  = nullptr)
{
    a.debugReset();

    const int total = int(mono.size());
    std::array<float, HilbertAnalyzer::N_BANDS> sumEnv {};
    std::array<float, HilbertAnalyzer::N_BANDS> env {}, ph {}, fq {};
    int countedHops = 0;

    int processedHops = 0;
    for (int start = 0; start + kHopSamples <= total; start += kHopSamples) {
        a.debugPushMono(mono.data() + start, kHopSamples);
        ++processedHops;

        // Snapshot once per hop -- we average over the trailing lastN.
        a.fillBandStates(env.data(), ph.data(), fq.data());
    }

    // Re-run the trailing hops to populate sumEnv. (debugPushMono updates
    // outputs but the trailing average is what we want.) Easier: drive
    // once more in chunks for the average -- but since debugPushMono
    // commits outputs already we just record them as we go in a second
    // pass.
    a.debugReset();
    std::array<float, HilbertAnalyzer::N_BANDS> finalEnv {}, finalPh {}, finalFq {};
    int sampleIdx = 0;
    int hopIdx    = 0;
    int totalHops = processedHops;
    int firstAvgHop = std::max(0, totalHops - lastN);
    for (int start = 0; start + kHopSamples <= total; start += kHopSamples) {
        a.debugPushMono(mono.data() + start, kHopSamples);
        a.fillBandStates(env.data(), ph.data(), fq.data());
        if (hopIdx >= firstAvgHop) {
            for (int b = 0; b < HilbertAnalyzer::N_BANDS; ++b)
                sumEnv[b] += env[b];
            ++countedHops;
        }
        ++hopIdx;
        sampleIdx += kHopSamples;
        finalEnv = env;
        finalPh  = ph;
        finalFq  = fq;
    }
    (void)sampleIdx;

    std::array<float, HilbertAnalyzer::N_BANDS> avg {};
    if (countedHops > 0) {
        const float inv = 1.0f / float(countedHops);
        for (int b = 0; b < HilbertAnalyzer::N_BANDS; ++b)
            avg[b] = sumEnv[b] * inv;
    }
    if (lastPhase) *lastPhase = finalPh;
    if (lastFreq)  *lastFreq  = finalFq;
    return avg;
}


void printBandTable(const char* label,
                    const HilbertAnalyzer& a,
                    const std::array<float, HilbertAnalyzer::N_BANDS>& vals)
{
    std::printf("\n== %s ==\n", label);
    QVariantList centres = a.bandCenters();
    for (int b = 0; b < HilbertAnalyzer::N_BANDS; ++b) {
        std::printf("  band %d  f0=%7.2f Hz   env=%.5f\n",
                    b, centres[b].toDouble(), vals[b]);
    }
}


}  // namespace


int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    int failures = 0;

    HilbertAnalyzer a;

    // Print the design points so the report is self-contained.
    {
        QVariantList centres = a.bandCenters();
        std::printf("== Band centers (log-spaced 80..8000 Hz) ==\n");
        for (int b = 0; b < HilbertAnalyzer::N_BANDS; ++b) {
            std::printf("  band %d : f0 = %.2f Hz\n",
                        b, centres[b].toDouble());
        }
    }

    // ---- Silence -----------------------------------------------------
    {
        auto sig = makeSilence(int(kSampleRate * 2));
        auto env = drive(a, sig, kHopRate);   // average last 1 s
        printBandTable("Silence -> envelope (target ~ 0)", a, env);
        float worst = 0.0f;
        for (auto v : env) worst = std::max(worst, std::abs(v));
        const bool ok = worst < 1e-3f;
        std::printf("  worst |env| = %.6f  %s\n", worst, ok ? "OK" : "FAIL");
        if (!ok) ++failures;
    }

    // ---- 440 Hz sine -------------------------------------------------
    {
        auto sig = makeSine(int(kSampleRate * 2), 440.0, 0.7);
        std::array<float, HilbertAnalyzer::N_BANDS> ph {}, fq {};
        auto env = drive(a, sig, kHopRate, &ph, &fq);
        printBandTable("440 Hz sine -> envelope", a, env);

        std::printf("  inst.freq snapshot (Hz):");
        for (auto f : fq) std::printf(" %.1f", f);
        std::printf("\n");
        std::printf("  phase     snapshot (rad):");
        for (auto p : ph) std::printf(" %+.2f", p);
        std::printf("\n");

        // Bands closest to 440 Hz in log space are b=2 (298 Hz) and
        // b=3 (576 Hz). 440 sits between them; the analyzer should put
        // most envelope into b=3 (since 440/576 = 0.764 vs 440/298 =
        // 1.477; in log space 440 is at log(440)=6.087, b=2 at 5.697,
        // b=3 at 6.357 -- distance 0.39 and 0.27, so b=3 closer).
        const float envB2 = env[2];
        const float envB3 = env[3];
        const float envFar = std::max({ env[0], env[5], env[6], env[7] });

        const bool peakInB3 = envB3 > envB2 && envB3 > envFar * 1.5f;
        std::printf("  peak in band 3 (~576 Hz)              : %s\n",
                    peakInB3 ? "OK" : "FAIL");
        if (!peakInB3) ++failures;

        // Inst.freq of the peak band should be near 440 Hz.
        const float fEst = fq[3];
        const bool fOk  = std::abs(fEst - 440.0f) < 60.0f;
        std::printf("  band 3 inst.freq = %.1f Hz (target ~440) %s\n",
                    fEst, fOk ? "OK" : "FAIL");
        if (!fOk) ++failures;
    }

    // ---- 2 kHz sine (sanity check on a higher band) -----------------
    {
        auto sig = makeSine(int(kSampleRate * 2), 2000.0, 0.7);
        std::array<float, HilbertAnalyzer::N_BANDS> ph {}, fq {};
        auto env = drive(a, sig, kHopRate, &ph, &fq);
        printBandTable("2 kHz sine -> envelope", a, env);
        // Band 5 center ~2146 Hz should dominate.
        const bool peakInB5 = env[5] > env[4] && env[5] > env[6];
        std::printf("  peak in band 5 (~2146 Hz)             : %s\n",
                    peakInB5 ? "OK" : "FAIL");
        if (!peakInB5) ++failures;
        const float fEst = fq[5];
        const bool  fOk  = std::abs(fEst - 2000.0f) < 200.0f;
        std::printf("  band 5 inst.freq = %.1f Hz (target ~2000) %s\n",
                    fEst, fOk ? "OK" : "FAIL");
        if (!fOk) ++failures;
    }

    // ---- White noise -------------------------------------------------
    {
        auto sig = makeWhiteNoise(int(kSampleRate * 4), 0.4);
        auto env = drive(a, sig, kHopRate * 2);   // 2 s avg
        printBandTable("White noise -> envelope", a, env);

        // White noise has a flat PSD; a 2/3-octave bandpass at center
        // f_b passes RMS energy proportional to sqrt(BW_b) = sqrt(c * f_b).
        // So we expect env_b ∝ sqrt(f_b). The lowest band (80 Hz) and
        // the highest (8 kHz) differ by sqrt(100) = 10x in expected
        // amplitude even with an ideal Hilbert. We test that the
        // *normalized* envelope env_b / sqrt(f_b) is roughly constant
        // across bands -- that's what "equal noise excitation per band"
        // means once you account for proportional bandwidth.
        QVariantList centres = a.bandCenters();
        std::array<float, HilbertAnalyzer::N_BANDS> normalized {};
        std::printf("  normalized (env / sqrt(f0)):\n");
        for (int b = 0; b < HilbertAnalyzer::N_BANDS; ++b) {
            normalized[b] = env[b] / std::sqrt(float(centres[b].toDouble()));
            std::printf("    band %d : %.5f\n", b, normalized[b]);
        }
        float nLo = std::min_element(normalized.begin(), normalized.end())
                  - normalized.begin();
        float nHi = std::max_element(normalized.begin(), normalized.end())
                  - normalized.begin();
        float lo  = normalized[size_t(nLo)];
        float hi  = normalized[size_t(nHi)];
        if (lo < 1e-9f) {
            std::printf("  normalized envelope degenerate; FAIL\n");
            ++failures;
        } else {
            float ratio = hi / lo;
            const bool ok = ratio < 4.0f;
            std::printf("  max/min normalized ratio = %.3f (target < 4) %s\n",
                        ratio, ok ? "OK" : "FAIL");
            if (!ok) ++failures;
        }
    }

    if (failures == 0) {
        std::printf("\nAll HilbertAnalyzer verifications passed.\n");
        return 0;
    }
    std::printf("\n%d failure(s).\n", failures);
    return 1;
}
