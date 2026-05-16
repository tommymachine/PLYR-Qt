#include "KeyEstimator.h"
#include "ChromaAnalyzer.h"

#include <QVariantMap>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>


namespace {

// Krumhansl-Kessler probe-tone profiles, both rooted on C. Each value is
// the mean probe-tone rating the K-K listeners gave to that pitch class
// when the key was C major / C minor. Source: Krumhansl & Kessler 1982.
constexpr std::array<float, 12> kKKMajorC = {
    6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f,
    2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f
};
constexpr std::array<float, 12> kKKMinorC = {
    6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f,
    2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f
};

// 12 pitch-class names. Sharps only, matching TonnetzView.
constexpr std::array<const char*, 12> kPcNames = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

// Build the circle-of-fifths u coordinate for a key whose major root is
// `root`. (root * 7) mod 12 gives the position 0..11 in the fifth cycle
// (C=0, G=1, D=2, A=3, E=4, B=5, F#=6, C#=7, G#=8, D#=9, A#=10, F=11).
// Divide by 12 to land in [0, 1).
inline float fifthAxisU(int root) {
    return float((root * 7) % 12) / 12.0f;
}

// Zero-mean / unit-norm normalization on a 12-element vector. Reduces
// Pearson correlation to a plain dot product against a similarly
// normalized vector. Returns false on a degenerate (zero-variance)
// input so callers can short-circuit. `out` is written in place.
bool normalizeForPearson(std::array<float, 12>& out) {
    float mean = 0.0f;
    for (int p = 0; p < 12; ++p) mean += out[p];
    mean /= 12.0f;
    float sumSq = 0.0f;
    for (int p = 0; p < 12; ++p) {
        out[p] -= mean;
        sumSq  += out[p] * out[p];
    }
    if (sumSq < 1e-12f) return false;
    const float invNorm = 1.0f / std::sqrt(sumSq);
    for (int p = 0; p < 12; ++p) out[p] *= invNorm;
    return true;
}

}  // namespace


KeyEstimator::KeyEstimator(QObject* parent) : QObject(parent)
{
    buildTemplates();
    recomputeAlpha();
}


KeyEstimator::~KeyEstimator() = default;


void KeyEstimator::buildTemplates()
{
    // Templates 0..11 = major, rooted on PC = template index.
    // Templates 12..23 = minor, rooted on PC = template index - 12.
    //
    // To get the profile rooted on r from the C-rooted profile, take
    // C_template[(p - r + 12) % 12] for each pitch class p. (When p=r
    // we're at the root, which maps to C_template[0] -- the largest
    // weight.) Both templates are then normalized in place so the
    // online Pearson collapses to a dot product.
    for (int r = 0; r < 12; ++r) {
        std::array<float, 12> majK {};
        std::array<float, 12> minK {};
        for (int p = 0; p < 12; ++p) {
            const int sh = ((p - r) % 12 + 12) % 12;
            majK[p] = kKKMajorC[sh];
            minK[p] = kKKMinorC[sh];
        }
        normalizeForPearson(majK);
        normalizeForPearson(minK);
        m_templates[r]      = majK;
        m_templates[12 + r] = minK;

        // Toroidal coordinates -- circle of fifths on u, mode on v.
        m_keyU[r]      = fifthAxisU(r);
        m_keyU[12 + r] = fifthAxisU(r);
        m_keyV[r]      = 0.0f;       // major hemisphere
        m_keyV[12 + r] = 0.5f;       // minor hemisphere
    }
}


void KeyEstimator::setChromaSource(ChromaAnalyzer* s)
{
    if (m_chromaSource == s) return;
    if (m_chromaSource) {
        disconnect(m_chromaSource, &ChromaAnalyzer::chromaUpdated,
                   this, &KeyEstimator::onChromaUpdated);
    }
    m_chromaSource = s;
    if (m_chromaSource) {
        connect(m_chromaSource, &ChromaAnalyzer::chromaUpdated,
                this, &KeyEstimator::onChromaUpdated,
                Qt::DirectConnection);
    }
    emit chromaSourceChanged();
}


void KeyEstimator::setSoftmaxTemperature(float t)
{
    if (t <= 0.0f || std::isnan(t)) return;
    m_softmaxTemp = t;
}


void KeyEstimator::setSmoothingMs(double ms)
{
    if (ms <= 0.0) return;
    m_smoothingSec = ms * 0.001;
    recomputeAlpha();
}


void KeyEstimator::setHopRate(double hz)
{
    if (hz <= 0.0) return;
    m_hopRateHz = hz;
    recomputeAlpha();
}


void KeyEstimator::recomputeAlpha()
{
    // alpha = exp(-1 / (tau * refresh_rate)). Larger alpha => slower
    // tracking. With tau = 200 ms and 60 Hz hops, alpha ~= 0.92, i.e.
    // 92% of the previous smoothed value is retained per hop. Same shape
    // ChromaAnalyzer + AudioFeatures use for their envelope followers.
    m_alpha = float(std::exp(-1.0 / (m_smoothingSec * m_hopRateHz)));
}


float KeyEstimator::correlate(const float* a12, const float* tmpl12)
{
    // Standard Pearson correlation. The template is already zero-mean /
    // unit-norm; we still need to demean + renorm the live chroma each
    // call because it shifts magnitude/baseline frame to frame.
    float mean = 0.0f;
    for (int p = 0; p < 12; ++p) mean += a12[p];
    mean /= 12.0f;
    float sumSq = 0.0f;
    std::array<float, 12> centered;
    for (int p = 0; p < 12; ++p) {
        centered[p] = a12[p] - mean;
        sumSq      += centered[p] * centered[p];
    }
    if (sumSq < 1e-12f) return 0.0f;
    const float invNorm = 1.0f / std::sqrt(sumSq);
    float dot = 0.0f;
    for (int p = 0; p < 12; ++p) dot += centered[p] * invNorm * tmpl12[p];
    return dot;
}


void KeyEstimator::onChromaUpdated()
{
    if (!m_chromaSource) return;
    std::array<float, PITCH_CLASSES> chr {};
    m_chromaSource->fillChromaSmoothed(chr.data());
    runEstimate(chr.data(), /*smoothCentroid*/ true);
}


void KeyEstimator::debugEstimate(const float* chroma12)
{
    if (!chroma12) return;
    runEstimate(chroma12, /*smoothCentroid*/ false);
}


void KeyEstimator::runEstimate(const float* chroma12, bool smoothCentroid)
{
    // 1. Detect a silent / degenerate chroma input. If everything is at
    //    or near zero, leave the published centroid untouched so the
    //    glow stays where the music last left it instead of drifting to
    //    (0, 0). We still publish zero weights so the QML readout shows
    //    no confident key.
    float chrSumAbs = 0.0f;
    for (int p = 0; p < 12; ++p) chrSumAbs += std::fabs(chroma12[p]);
    if (chrSumAbs < 1e-6f) {
        for (auto& w : m_weights) w = 0.0f;
        m_argmax        = -1;
        m_keyName       = QString();
        m_keyConfidence.store(0.0f, std::memory_order_release);
        emit estimateUpdated();
        return;
    }

    // 2. De-smear the chromagram before correlation. The B=24 CQT kernel
    //    has a known mainlobe wider than the semitone spacing in the
    //    lower octaves -- a pure tone at PC p puts ~95% of its energy
    //    in bin p and ~90% in bin p+1. ChromaAnalyzer's pow(1.5)
    //    contrast curve does not get rid of this; for chord detection
    //    that worked (the Tonnetz triangle just needs three lit
    //    vertices), but for KEY detection it is catastrophic: the
    //    C-major chord's smear pattern [C, C#, E, F, G, G#] looks more
    //    like F minor's K-K profile (F+G#+C peaks) than C major's
    //    (C+E+G peaks).
    //
    //    Subtract a fraction alpha of each PC from its upward neighbour
    //    -- the smear direction -- then clamp to zero. This is a one-
    //    pole IIR inverse-filter step; alpha = 0.85 cancels ~90% of the
    //    leakage measured empirically by chromaverify_cli. Iterating
    //    twice (de-smear, then de-smear the residual) catches the small
    //    two-step leakage from PC p to p+2 as well.
    std::array<float, PITCH_CLASSES> chr {};
    for (int p = 0; p < PITCH_CLASSES; ++p) chr[p] = chroma12[p];
    constexpr float kSmearAlpha = 0.85f;
    for (int pass = 0; pass < 2; ++pass) {
        std::array<float, PITCH_CLASSES> next {};
        for (int p = 0; p < PITCH_CLASSES; ++p) {
            const int  prev = (p + PITCH_CLASSES - 1) % PITCH_CLASSES;
            const float val = chr[p] - kSmearAlpha * chr[prev];
            next[p] = std::max(val, 0.0f);
        }
        chr = next;
    }
    // Re-normalize so the brightest PC reads 1.0 -- correlations are
    // scale-invariant so this is just for readability of the weights.
    float chMax = 0.0f;
    for (int p = 0; p < PITCH_CLASSES; ++p) chMax = std::max(chMax, chr[p]);
    if (chMax > 1e-6f) {
        const float inv = 1.0f / chMax;
        for (int p = 0; p < PITCH_CLASSES; ++p) chr[p] *= inv;
    }

    // 3. Pearson correlation against each of the 24 templates.
    std::array<float, N_KEYS> r {};
    for (int k = 0; k < N_KEYS; ++k) {
        r[k] = correlate(chr.data(), m_templates[k].data());
    }

    // 3. Softmax with temperature. Subtract the max correlation first
    //    for numerical stability (exp of large values overflows). With
    //    temp=8 a single key with correlation 0.1 ahead of its nearest
    //    rival gets ~70% of the mass, which is what we want for a
    //    "clear glow at one position" display.
    float rMax = r[0];
    for (int k = 1; k < N_KEYS; ++k) rMax = std::max(rMax, r[k]);

    std::array<float, N_KEYS> w {};
    float sumW = 0.0f;
    for (int k = 0; k < N_KEYS; ++k) {
        w[k]  = std::exp(m_softmaxTemp * (r[k] - rMax));
        sumW += w[k];
    }
    if (sumW < 1e-12f) {
        // Should not happen after the silent-input guard above, but be
        // defensive: leave the previous centroid alone and bail.
        emit estimateUpdated();
        return;
    }
    const float inv = 1.0f / sumW;
    for (int k = 0; k < N_KEYS; ++k) w[k] *= inv;
    m_weights = w;

    // 4. Argmax + label. We always publish the most-likely key by
    //    softmax weight, equivalent here to the argmax of the underlying
    //    correlations (monotonic).
    int bestIdx = 0;
    for (int k = 1; k < N_KEYS; ++k) {
        if (w[k] > w[bestIdx]) bestIdx = k;
    }
    m_argmax = bestIdx;
    const int  root    = bestIdx % 12;
    const bool isMajor = bestIdx < 12;
    m_keyName = QString::fromUtf8(kPcNames[root])
              + (isMajor ? QStringLiteral(" major")
                         : QStringLiteral(" minor"));
    m_keyConfidence.store(w[bestIdx], std::memory_order_release);

    // 5. Circular-mean centroid over the 24 key positions weighted by
    //    softmax. Convert each (u_k, v_k) to (sin, cos) before
    //    averaging, then recover the angle via atan2 -- the standard
    //    way to handle the seam where u/v wrap from ~1 back to 0.
    constexpr float TAU = 6.2831853071795864769f;
    float sU = 0.0f, cU = 0.0f, sV = 0.0f, cV = 0.0f;
    for (int k = 0; k < N_KEYS; ++k) {
        const float uAng = TAU * m_keyU[k];
        const float vAng = TAU * m_keyV[k];
        sU += w[k] * std::sin(uAng);
        cU += w[k] * std::cos(uAng);
        sV += w[k] * std::sin(vAng);
        cV += w[k] * std::cos(vAng);
    }

    // 6. Smoothing: one-pole IIR on the trig accumulators. On the first
    //    hop replace; thereafter blend. Smoothing in trig space (rather
    //    than directly on u/v) is what makes the wraparound continuous.
    if (smoothCentroid && m_havePrior) {
        const float a = m_alpha;
        m_smSinU = a * m_smSinU + (1.0f - a) * sU;
        m_smCosU = a * m_smCosU + (1.0f - a) * cU;
        m_smSinV = a * m_smSinV + (1.0f - a) * sV;
        m_smCosV = a * m_smCosV + (1.0f - a) * cV;
    } else {
        m_smSinU = sU; m_smCosU = cU;
        m_smSinV = sV; m_smCosV = cV;
        m_havePrior = true;
    }

    // 7. Recover smoothed u, v via atan2. atan2 returns [-pi, pi]; map
    //    back to [0, 1) via /TAU and fract. We clamp the result to
    //    [0, 1 - eps] so float rounding of (1 - tiny) -> 1.0 can't
    //    produce a literal 1.0 that downstream consumers might treat
    //    as "off the surface".
    auto wrap01 = [](float angle) {
        float t = angle / TAU;
        t = t - std::floor(t);                 // fract in [0, 1]
        if (t >= 1.0f) t = 0.0f;               // edge case: rounded up to 1
        return t;
    };
    const float uOut = wrap01(std::atan2(m_smSinU, m_smCosU));
    const float vOut = wrap01(std::atan2(m_smSinV, m_smCosV));
    m_torusU.store(uOut, std::memory_order_release);
    m_torusV.store(vOut, std::memory_order_release);

    emit estimateUpdated();
}


QVariantList KeyEstimator::topKeys() const
{
    // Build a Top-N list sorted by softmax weight descending. We do the
    // work here (not on the per-hop path) because this is invoked from
    // QML bindings, not the audio update loop.
    struct Cand { int idx; float w; };
    std::array<Cand, N_KEYS> ranked;
    for (int k = 0; k < N_KEYS; ++k) ranked[k] = {k, m_weights[k]};

    const int N = std::min(TOP_N_MAX, N_KEYS);
    std::partial_sort(ranked.begin(), ranked.begin() + N, ranked.end(),
                      [](const Cand& a, const Cand& b) { return a.w > b.w; });

    QVariantList out;
    out.reserve(N);
    for (int i = 0; i < N; ++i) {
        const int   idx     = ranked[i].idx;
        const int   root    = idx % 12;
        const bool  isMajor = idx < 12;
        QVariantMap row;
        row.insert(QStringLiteral("name"),
                   QString::fromUtf8(kPcNames[root])
                       + (isMajor ? QStringLiteral(" major")
                                  : QStringLiteral(" minor")));
        row.insert(QStringLiteral("u"),      m_keyU[idx]);
        row.insert(QStringLiteral("v"),      m_keyV[idx]);
        row.insert(QStringLiteral("weight"), ranked[i].w);
        row.insert(QStringLiteral("isMajor"), isMajor);
        row.insert(QStringLiteral("root"),    root);
        out.append(row);
    }
    return out;
}


void KeyEstimator::fillTopKeyUniforms(float* outUV, int* outCount)
{
    if (!outUV) return;
    struct Cand { int idx; float w; };
    std::array<Cand, N_KEYS> ranked;
    for (int k = 0; k < N_KEYS; ++k) ranked[k] = {k, m_weights[k]};

    const int N = std::min(TOP_N_MAX, N_KEYS);
    std::partial_sort(ranked.begin(), ranked.begin() + N, ranked.end(),
                      [](const Cand& a, const Cand& b) { return a.w > b.w; });

    // Pack as four vec4: (u, v, weight, 0). The trailing slot is unused
    // today; later passes might use it for a per-key color tint.
    int produced = 0;
    for (int i = 0; i < TOP_N_MAX; ++i) {
        float* base = &outUV[i * 4];
        if (i < N && ranked[i].w > 1e-6f) {
            const int idx = ranked[i].idx;
            base[0] = m_keyU[idx];
            base[1] = m_keyV[idx];
            base[2] = ranked[i].w;
            base[3] = 0.0f;
            ++produced;
        } else {
            base[0] = 0.0f;
            base[1] = 0.0f;
            base[2] = 0.0f;
            base[3] = 0.0f;
        }
    }
    if (outCount) *outCount = produced;
}


QVariantList KeyEstimator::keyTemplate(int k) const
{
    QVariantList out;
    if (k < 0 || k >= N_KEYS) return out;
    out.reserve(PITCH_CLASSES);
    for (int p = 0; p < PITCH_CLASSES; ++p) out.append(m_templates[k][p]);
    return out;
}


void KeyEstimator::fillWeights(float* out24) const
{
    if (!out24) return;
    std::memcpy(out24, m_weights.data(), sizeof(float) * N_KEYS);
}


const char* KeyEstimator::pcName(int pc)
{
    if (pc < 0 || pc >= 12) return "";
    return kPcNames[pc];
}
