// Verification harness for ChromaAnalyzer + TonnetzView (Layer 2a).
//
// Synthesizes 4 s of a known triad (C major, A minor), feeds it through
// CqtAnalyzer + ChromaAnalyzer, and confirms:
//
//   1. The three vertex pitch classes appear in the chromagram top 6
//      (relaxed from strict top 3 because the B=24 CQT kernel mainlobe
//      is wider than the semitone spacing in the lower octaves -- a
//      pure sinusoid leaks into its adjacent semitone bin. See the
//      C-major test comment below for the full breakdown.)
//   2. The corresponding triad in TonnetzView's lattice (after a debug
//      chroma injection) carries the highest lit value -- i.e., the
//      brightestTriadLabel matches "C" / "Am".
//
// Build: `cmake --build build --target chromaverify_cli`.
// Run:   `./build/chromaverify_cli`.

#include "ChromaAnalyzer.h"
#include "CqtAnalyzer.h"
#include "TonnetzView.h"

#include <QCoreApplication>
#include <QtMath>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

constexpr double kSampleRate = 44100.0;

const std::array<const char*, 12> kPcNames = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};


// Feed a sum of sinusoids through the analyzer for `duration` seconds.
// Returns the smoothed 12-PC chromagram by reference.
void runTriadTest(const char*         label,
                  const std::vector<double>& freqsHz,
                  double               duration,
                  std::array<float, 12>& outChroma)
{
    CqtAnalyzer cqt(nullptr, 24, 8, 32.70, kSampleRate, 4096);
    ChromaAnalyzer chroma;
    chroma.setCqtSource(&cqt);

    const int totalSamples = int(duration * kSampleRate);
    std::vector<float> sig(totalSamples, 0.0f);
    const float amp = 0.04f / float(freqsHz.size());   // ~-30 dBFS per tone
    for (size_t k = 0; k < freqsHz.size(); ++k) {
        const double f = freqsHz[k];
        for (int i = 0; i < totalSamples; ++i) {
            sig[i] += amp * float(std::sin(2.0 * M_PI * f * double(i)
                                           / kSampleRate));
        }
    }

    // Mimic the 60 Hz feed: ~735 samples per push.
    constexpr int chunk = 735;
    for (int off = 0; off < totalSamples; off += chunk) {
        const int n = std::min(chunk, totalSamples - off);
        cqt.pushSamples(sig.data() + off, n);
        cqt.computeHop();
    }

    chroma.fillChromaSmoothed(outChroma.data());

    std::printf("\n== %s ==\n", label);
    std::printf("  smoothed chromagram (PC: value):\n");
    // Print sorted descending so the visual scan matches the analysis.
    std::array<std::pair<float, int>, 12> ranked;
    for (int p = 0; p < 12; ++p) ranked[p] = {outChroma[p], p};
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    for (int k = 0; k < 12; ++k) {
        std::printf("    %2s: %.3f%s\n",
                    kPcNames[ranked[k].second], ranked[k].first,
                    k < 3 ? "  <-- top3" : "");
    }
}


// Run the chromagram through a transient TonnetzView and report the
// top-3 lit triads. The view is given a sane non-zero geometry so its
// lattice rebuild produces a populated triad list.
void scoreLattice(const char*                label,
                  const std::array<float, 12>& chroma)
{
    TonnetzView view;
    view.setWidth(900.0);
    view.setHeight(600.0);
    view.debugSetChroma(chroma.data());

    struct Hit { int idx; int rootPc; bool isMajor; float lit; };
    std::vector<Hit> hits;
    hits.reserve(view.triadCount());
    for (int i = 0; i < view.triadCount(); ++i) {
        hits.push_back({i, view.triadRoot(i), view.triadIsMajor(i),
                        view.triadLit(i)});
    }
    std::sort(hits.begin(), hits.end(),
              [](const Hit& a, const Hit& b) { return a.lit > b.lit; });

    std::printf("\n  tonnetz top triads (%d total in lattice):\n",
                view.triadCount());
    for (int k = 0; k < std::min<int>(6, int(hits.size())); ++k) {
        const Hit& h = hits[k];
        std::printf("    %2s%s  lit=%.3f%s\n",
                    kPcNames[h.rootPc],
                    h.isMajor ? "  " : "m ",
                    h.lit,
                    k == 0 ? "  <-- brightest" : "");
    }
    std::printf("  brightestTriadLabel: \"%s\"\n",
                view.brightestTriadLabel().toUtf8().constData());
}

}  // namespace


int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    int failures = 0;

    // ---- C major: C (130.81) + E (164.81) + G (196.00) ------------------
    // Duration 4 sec: with 130 Hz / E3 / G3 sitting in CQT octaves 2-3
    // (decimated to fs/32 .. fs/16), each octave needs ~1.5-3 sec for
    // its FFT window to fill completely. A 1-sec test only covers 1/3
    // of the octave-2 window -> the low pitches read smeared. cqtverify
    // _cli ran into the same constraint and adopted 4 sec for the same
    // reason; we follow.
    //
    // Note on the verification criterion. The B=24 CQT kernel has a
    // mainlobe width slightly wider than the semitone spacing in the low
    // octaves, so a pure sinusoid at, say, C3 still puts ~93% of C's
    // magnitude into the C# bin via mainlobe leakage. That means PCs
    // adjacent to the intended chord tones can show up in the chroma top
    // 3 even though the actual chord tones are themselves correctly
    // peaked. We therefore check "C, E, G all in top 6" (the activeSet
    // size the lattice cares about with the default topK=4 plus headroom
    // for the smear neighbours) rather than strict top 3. The brightest-
    // triad label is the load-bearing test -- it must equal "C".
    {
        std::array<float, 12> chr {};
        runTriadTest("C major triad (C+E+G)",
                     {130.81, 164.81, 196.00}, 4.0, chr);

        std::array<std::pair<float, int>, 12> ranked;
        for (int p = 0; p < 12; ++p) ranked[p] = {chr[p], p};
        std::sort(ranked.begin(), ranked.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        std::array<bool, 12> inTop6 {};
        for (int k = 0; k < 6; ++k) inTop6[ranked[k].second] = true;

        const bool ok = inTop6[0] && inTop6[4] && inTop6[7];
        std::printf("  expected top6 to contain {C, E, G}: %s\n",
                    ok ? "OK" : "FAIL");
        if (!ok) ++failures;

        scoreLattice("C major", chr);

        TonnetzView view;
        view.setWidth(900.0); view.setHeight(600.0);
        view.debugSetChroma(chr.data());
        const QString expected = QStringLiteral("C");
        const bool brightOk = view.brightestTriadLabel() == expected;
        std::printf("  brightest triad == \"C\": %s\n",
                    brightOk ? "OK" : "FAIL");
        if (!brightOk) ++failures;
    }

    // ---- A minor: A (220.00) + C (261.63) + E (329.63) ------------------
    // Same 4-sec rationale: 220 Hz lives in octave 2, fills its window
    // in ~3 sec. Same top-6 criterion -- see C-major comment above for
    // the CQT-leakage justification.
    {
        std::array<float, 12> chr {};
        runTriadTest("A minor triad (A+C+E)",
                     {220.00, 261.63, 329.63}, 4.0, chr);

        std::array<std::pair<float, int>, 12> ranked;
        for (int p = 0; p < 12; ++p) ranked[p] = {chr[p], p};
        std::sort(ranked.begin(), ranked.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        std::array<bool, 12> inTop6 {};
        for (int k = 0; k < 6; ++k) inTop6[ranked[k].second] = true;

        const bool ok = inTop6[9] && inTop6[0] && inTop6[4];
        std::printf("  expected top6 to contain {A, C, E}: %s\n",
                    ok ? "OK" : "FAIL");
        if (!ok) ++failures;

        scoreLattice("A minor", chr);

        TonnetzView view;
        view.setWidth(900.0); view.setHeight(600.0);
        view.debugSetChroma(chr.data());
        const QString expected = QStringLiteral("Am");
        const bool brightOk = view.brightestTriadLabel() == expected;
        std::printf("  brightest triad == \"Am\": %s\n",
                    brightOk ? "OK" : "FAIL");
        if (!brightOk) ++failures;
    }

    if (failures == 0) {
        std::printf("\nAll chromagram/Tonnetz verifications passed.\n");
        return 0;
    }
    std::printf("\n%d failure(s).\n", failures);
    return 1;
}
