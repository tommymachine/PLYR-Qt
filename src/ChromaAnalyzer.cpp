#include "ChromaAnalyzer.h"
#include "CqtAnalyzer.h"

#include <QMutexLocker>
#include <algorithm>
#include <cmath>
#include <cstring>


ChromaAnalyzer::ChromaAnalyzer(QObject* parent)
    : QObject(parent)
{
    recomputeAlphas();
}


ChromaAnalyzer::~ChromaAnalyzer() = default;


void ChromaAnalyzer::setCqtSource(CqtAnalyzer* s)
{
    if (m_cqtSource == s) return;
    if (m_cqtSource) {
        disconnect(m_cqtSource, &CqtAnalyzer::hopComplete,
                   this, &ChromaAnalyzer::onHopComplete);
    }
    m_cqtSource = s;
    if (m_cqtSource) {
        connect(m_cqtSource, &CqtAnalyzer::hopComplete,
                this, &ChromaAnalyzer::onHopComplete,
                Qt::DirectConnection);
    }
    emit cqtSourceChanged();
}


void ChromaAnalyzer::setAttackMs(double ms)
{
    if (ms <= 0.0) return;
    m_tauAttackSec = ms * 0.001;
    recomputeAlphas();
}


void ChromaAnalyzer::setReleaseMs(double ms)
{
    if (ms <= 0.0) return;
    m_tauReleaseSec = ms * 0.001;
    recomputeAlphas();
}


void ChromaAnalyzer::setHopRate(double hz)
{
    if (hz <= 0.0) return;
    m_hopRateHz = hz;
    recomputeAlphas();
}


void ChromaAnalyzer::recomputeAlphas()
{
    // alpha = exp(-1 / (tau * refresh_rate))   -- same shape AudioFeatures
    // uses for its band envelopes. The envelope tracks the target value;
    // when the new sample exceeds the current value we use alphaAttack
    // (small alpha => fast update), otherwise alphaRelease.
    m_alphaAttack  = float(std::exp(-1.0 / (m_tauAttackSec  * m_hopRateHz)));
    m_alphaRelease = float(std::exp(-1.0 / (m_tauReleaseSec * m_hopRateHz)));
}


void ChromaAnalyzer::onHopComplete()
{
    if (!m_cqtSource) return;

    const int outputBins    = m_cqtSource->outputBins();
    const int binsPerOctave = m_cqtSource->binsPerOctave();
    if (outputBins <= 0 || binsPerOctave <= 0) return;
    if (outputBins > int(m_cqtScratch.size())) return;

    // 1. Pull raw linear magnitudes.
    if (!m_cqtSource->fillRowFloat(m_cqtScratch.data(), outputBins))
        return;

    // 2. Fold into 12 pitch classes. With B=24 each pitch class owns
    //    `binsPerSemi = 2` consecutive bins per octave: the equal-tempered
    //    bin and the quarter-tone-sharp neighbour. The PC's logical center
    //    sits between them.
    //
    //    Two effects combine to push us toward a Gaussian-weighted sum
    //    centered on the PC's mid-point rather than its equal-tempered
    //    bin:
    //      (a) The octave-recursive CQT has a systematic ~+1 bin upward
    //          bias in the lower octaves (verified against cqtverify_cli's
    //          220 Hz test: peak at bin 67, not 66). A Gaussian centered
    //          on the equal-tempered bin alone would assign that
    //          mistuned-up energy to the *next* PC.
    //      (b) Equal-tempered tones still leak into the adjacent CQT bins
    //          via the kernel's main lobe (Q ~ 34 with B=24 doesn't
    //          completely isolate bins). Including both within-PC bins
    //          recovers that leaked energy.
    //
    //    For each CQT bin we accumulate into its unique PC with a
    //    Gaussian weight on the offset from the PC center. With the
    //    center at (binsPerSemi - 1)/2 and sigma = 1 bin, the two bins
    //    of each PC get the same weight (so a pure tone at either offset
    //    reads the same PC magnitude). Tunable via kGaussianSigmaBins.
    const int   binsPerSemi = binsPerOctave / 12;       // 2 for B=24
    const int   nOctaves    = outputBins / binsPerOctave;
    const float invTwoSig2  = 1.0f / (2.0f * kGaussianSigmaBins *
                                              kGaussianSigmaBins);
    const float pcCenter    = float(binsPerSemi - 1) * 0.5f;

    std::array<float, PITCH_CLASSES> accum {};
    for (int p = 0; p < PITCH_CLASSES; ++p) accum[p] = 0.0f;

    for (int oct = 0; oct < nOctaves; ++oct) {
        const int octBase = oct * binsPerOctave;
        for (int b = 0; b < binsPerOctave; ++b) {
            const int   pc     = (b / binsPerSemi) % PITCH_CLASSES;
            const float offset = float(b % binsPerSemi) - pcCenter;
            const float w      = std::exp(-offset * offset * invTwoSig2);
            accum[pc] += w * m_cqtScratch[octBase + b];
        }
    }

    // 3. Log compression so a soft-but-clear tone still reads close to
    //    a loud one (the chromagram should look ~the same whether the
    //    chord is mezzo-piano or fortissimo).
    float maxLog = 0.0f;
    for (int p = 0; p < PITCH_CLASSES; ++p) {
        accum[p] = std::log1p(accum[p]);
        maxLog   = std::max(maxLog, accum[p]);
    }

    // 4. Peak normalization so the brightest PC reads 1.0. If the input
    //    is silent we leave accum at zero (the smoother will release).
    if (maxLog > 1e-9f) {
        const float inv = 1.0f / maxLog;
        for (int p = 0; p < PITCH_CLASSES; ++p) accum[p] *= inv;
    } else {
        for (int p = 0; p < PITCH_CLASSES; ++p) accum[p] = 0.0f;
    }

    // 4b. Contrast curve. The CQT kernel mainlobe at B=24 is slightly
    //     wider than the semitone spacing in the lower octaves -- a pure
    //     tone at C3 puts ~95% of its energy into the C bin and ~90%
    //     into the C# bin via mainlobe leakage. The Tonnetz lattice still
    //     picks the correct triad (the chord-vertex bins are all near
    //     1.0), but the margin between C and C# m gets thin. Raising the
    //     normalized chromagram to power=1.5 pulls the brightest PC
    //     further ahead of its semitone-neighbour smear without losing
    //     dynamic range (since all values are in [0,1], pow keeps them
    //     there). Same trick the spectrum analyzer uses for its display
    //     gamma.
    constexpr float kPcContrastPower = 1.5f;
    for (int p = 0; p < PITCH_CLASSES; ++p) {
        accum[p] = std::pow(accum[p], kPcContrastPower);
    }

    // 5. Two-time-constant envelope per PC + publish.
    {
        QMutexLocker lk(&m_outMutex);
        for (int p = 0; p < PITCH_CLASSES; ++p) {
            m_chroma[p] = accum[p];
            const float prev   = m_chromaSmoothed[p];
            const float target = accum[p];
            const float a = (target > prev) ? m_alphaAttack : m_alphaRelease;
            m_chromaSmoothed[p] = a * prev + (1.0f - a) * target;
        }
        m_haveChroma.store(true, std::memory_order_release);
    }

    emit chromaUpdated();
}


QVariantList ChromaAnalyzer::chromaList() const
{
    QVariantList out;
    out.reserve(PITCH_CLASSES);
    // Snapshot under the lock so we don't tear across a hop update.
    QMutexLocker lk(const_cast<QMutex*>(&m_outMutex));
    for (int p = 0; p < PITCH_CLASSES; ++p) out.append(m_chroma[p]);
    return out;
}


QVariantList ChromaAnalyzer::chromaSmoothedList() const
{
    QVariantList out;
    out.reserve(PITCH_CLASSES);
    QMutexLocker lk(const_cast<QMutex*>(&m_outMutex));
    for (int p = 0; p < PITCH_CLASSES; ++p) out.append(m_chromaSmoothed[p]);
    return out;
}


void ChromaAnalyzer::fillChroma(float* out12)
{
    if (!out12) return;
    QMutexLocker lk(&m_outMutex);
    std::memcpy(out12, m_chroma.data(), sizeof(float) * PITCH_CLASSES);
}


void ChromaAnalyzer::fillChromaSmoothed(float* out12)
{
    if (!out12) return;
    QMutexLocker lk(&m_outMutex);
    std::memcpy(out12, m_chromaSmoothed.data(), sizeof(float) * PITCH_CLASSES);
}
