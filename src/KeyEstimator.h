// KeyEstimator -- Krumhansl-Kessler key estimation + Janata-style
// toroidal embedding (Layer 2b).
//
// Pipeline per ChromaAnalyzer::chromaUpdated:
//   1. Pull the 12-PC smoothed chromagram.
//   2. Pearson-correlate against 24 K-K templates (12 major + 12 minor
//      keys), each pre-shifted from the C-rooted profile.
//   3. Softmax(temp=8) over the 24 correlations -> normalized weights.
//   4. Project each key onto a 2D torus:
//        u_k = ((root * 7) mod 12) / 12      (circle-of-fifths axis)
//        v_k = 0.0 if major else 0.5         (mode axis)
//      Then take the weight-blended *circular mean* of the 24 (u, v)
//      points: smooth sin(2pi u) and cos(2pi u) separately, recover u
//      via atan2 -- correct wraparound across the torus seam.
//   5. Exponential smoother (tau ~200 ms) over (sin u, cos u, sin v,
//      cos v) so the rendered point drifts instead of snapping.
//
// Threading: lives on the GUI thread. ChromaAnalyzer::chromaUpdated is
// emitted from a direct connection off CqtAnalyzer::hopComplete, also on
// the GUI thread, so we inherit that thread.

#pragma once

#include "ChromaAnalyzer.h"   // full definition needed for the
                              // Q_PROPERTY(ChromaAnalyzer*) meta-type

#include <QObject>
#include <QString>
#include <QVariantList>
#include <array>
#include <atomic>
#include <qqmlregistration.h>

class KeyEstimator : public QObject {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(ChromaAnalyzer* chromaSource READ chromaSource
               WRITE setChromaSource NOTIFY chromaSourceChanged)

    // Currently most-likely key name, e.g. "C major" / "F# minor". Empty
    // string before the first usable hop arrives.
    Q_PROPERTY(QString keyName       READ keyName       NOTIFY estimateUpdated)
    // Softmax weight of the most-likely key, in [0, 1]. With the default
    // temperature this is typically 0.5-0.9 for unambiguous classical
    // tonality, lower during modulations or noisy passages.
    Q_PROPERTY(float   keyConfidence READ keyConfidence NOTIFY estimateUpdated)
    // Toroidal centroid coordinates -- smoothed circular mean of the 24
    // key positions weighted by softmax. Both in [0, 1] with wraparound.
    Q_PROPERTY(float   torusU        READ torusU        NOTIFY estimateUpdated)
    Q_PROPERTY(float   torusV        READ torusV        NOTIFY estimateUpdated)
    // Top-N most-likely keys, each as a {name, u, v, weight} map. N = 4
    // by default; configurable via setTopN. Useful for QML to drive
    // secondary glow markers on the torus.
    Q_PROPERTY(QVariantList topKeys  READ topKeys       NOTIFY estimateUpdated)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)

public:
    static constexpr int PITCH_CLASSES = 12;
    static constexpr int N_KEYS        = 24;     // 12 major + 12 minor
    static constexpr int TOP_N_MAX     = 4;      // for shader uniforms

    explicit KeyEstimator(QObject* parent = nullptr);
    ~KeyEstimator() override;

    ChromaAnalyzer* chromaSource() const { return m_chromaSource; }
    void            setChromaSource(ChromaAnalyzer* s);

    bool active() const { return m_active; }
    void setActive(bool a);

    QString      keyName()       const { return m_keyName;       }
    float        keyConfidence() const { return m_keyConfidence; }
    float        torusU()        const { return m_torusU;        }
    float        torusV()        const { return m_torusV;        }
    QVariantList topKeys()       const;

    // Tuning knobs. Exposed for chromaverify_cli + future calibration;
    // the defaults are baked in by the constructor.
    Q_INVOKABLE void setSoftmaxTemperature(float t);
    Q_INVOKABLE void setSmoothingMs(double ms);
    Q_INVOKABLE void setHopRate(double hz);

    // Pack the top-N keys' (u, v, weight, 0) into outUV (16 floats = 4
    // vec4's). outCount returns the number actually filled (<= 4). For
    // a shader binding that wants the top-N as four vec4 uniforms.
    void fillTopKeyUniforms(float* outUV, int* outCount);

    // Static-sized template table, exposed for the verification harness.
    // Template k = m_templates[k]; k in [0, 12) is major, [12, 24) is
    // minor. Both are normalized (zero-mean, unit-variance) so Pearson
    // collapses to a plain dot product at runtime.
    Q_INVOKABLE QVariantList keyTemplate(int k) const;

    // Re-run a single estimation pass with a synthetic chroma snapshot.
    // Used only by chromaverify_cli to drive the estimator deterministi-
    // cally without standing up a Cqt + chroma pipeline + signal chain.
    // Bypasses smoothing on the toroidal centroid.
    void debugEstimate(const float* chroma12);

    // Direct accessor for the latest 24-element softmax weight vector.
    // Verification harness uses this to inspect the full posterior.
    // out must hold N_KEYS floats.
    void fillWeights(float* out24) const;

signals:
    void chromaSourceChanged();
    void estimateUpdated();
    void activeChanged();

private slots:
    void onChromaUpdated();

private:
    // --- one-time template construction --------------------------------
    void buildTemplates();
    // --- per-hop pipeline ----------------------------------------------
    // Compute Pearson correlation between chroma and a precomputed
    // normalized template (which is itself zero-mean / unit-norm). The
    // 12-element chroma is normalized inline.
    static float correlate(const float* a12, const float* tmpl12);
    void         runEstimate(const float* chroma12, bool smoothCentroid);
    // Recompute m_alpha when smoothing tau or hop rate changes.
    void         recomputeAlpha();
    // Pitch-class name (sharps only) for index 0..11. Static lookup.
    static const char* pcName(int pc);

    ChromaAnalyzer* m_chromaSource = nullptr;

    // 24 zero-mean / unit-norm key templates. Index 0..11 = major rooted
    // on PC index; index 12..23 = minor rooted on PC index - 12.
    std::array<std::array<float, PITCH_CLASSES>, N_KEYS> m_templates {};
    // Per-key toroidal coordinates -- cached constants derived in the
    // constructor from the major-root + mode mapping. u_k in [0,1) is
    // the circle-of-fifths position; v_k is 0 for major, 0.5 for minor.
    std::array<float, N_KEYS> m_keyU {};
    std::array<float, N_KEYS> m_keyV {};
    // Last softmax weights (24).
    std::array<float, N_KEYS> m_weights {};

    // Centroid trig accumulators, smoothed via one-pole IIR. We smooth
    // (sin, cos) for u and v separately so the wraparound at the seam
    // (u just below 1 then just above 0) doesn't blow up the average.
    float m_smSinU = 0.0f;
    float m_smCosU = 0.0f;
    float m_smSinV = 0.0f;
    float m_smCosV = 0.0f;

    // Published state -- read by the QML property getters above. Atomic
    // so a render-thread reader stays safe; this is overkill on the GUI
    // thread but the cost is zero.
    QString             m_keyName;
    std::atomic<float>  m_keyConfidence {0.0f};
    std::atomic<float>  m_torusU        {0.0f};
    std::atomic<float>  m_torusV        {0.0f};
    int                 m_argmax = -1;

    // Tuning knobs.
    float  m_softmaxTemp  = 8.0f;     // sharper => one key dominates
    double m_smoothingSec = 0.200;    // 200 ms tau on the centroid IIR
    double m_hopRateHz    = 60.0;     // matches AudioFeatures refresh
    float  m_alpha        = 0.0f;     // = exp(-1 / (tau * hopRate))

    // First-hop flag -- on the very first estimate, the trig accumula-
    // tors get *replaced* (no IIR blend with the zero init values),
    // otherwise the first frame's published centroid is half-way to
    // (0, 0) regardless of the actual chord.
    bool m_havePrior = false;

    bool m_active = true;
};
