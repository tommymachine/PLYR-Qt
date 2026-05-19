// ChromaAnalyzer -- folds CqtAnalyzer's per-bin magnitudes into a
// 12-element chromagram (one per pitch class, C..B), Layer 2a's input
// for the Tonnetz triad overlay.
//
// Pipeline per CqtAnalyzer::hopComplete signal:
//   1. Pull the raw linear magnitudes via CqtAnalyzer::fillRowFloat.
//   2. Fold octaves into pitch classes with Gaussian quarter-tone
//      weighting -- each pitch class p sums energy from the on-tune bin
//      and its quarter-tone neighbours, weights exp(-(offset^2)/(2sigma^2)).
//      This makes the chromagram robust to instruments slightly out of
//      A440 reference.
//   3. log(1 + x) compression so loud and quiet PCs are visible together.
//   4. Normalize to max=1 across the 12 PCs so the brightest one always
//      reads 1.0.
//   5. Two-time-constant envelope follower per PC (fast attack ~50 ms,
//      slower release ~250 ms) -- same idiom AudioFeatures uses for its
//      band envelopes.
//
// Threading: lives on the GUI thread alongside CqtAnalyzer. Direct-
// connected to hopComplete so the chromagram is ready synchronously when
// downstream consumers (TonnetzView) read it. The fillChroma* C++
// accessors snapshot under a short mutex so render-thread callers stay
// safe even though the only current caller (TonnetzView) is GUI-thread.

#pragma once

#include "CqtAnalyzer.h"   // full definition needed for the
                           // Q_PROPERTY(CqtAnalyzer*) meta-type below

#include <QMutex>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <array>
#include <atomic>
#include <qqmlregistration.h>

class ChromaAnalyzer : public QObject {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(CqtAnalyzer* cqtSource READ cqtSource WRITE setCqtSource
               NOTIFY cqtSourceChanged)
    // 12-element snapshots: the raw normalized chromagram and the
    // envelope-smoothed one. Exposed as QVariantList so QML bindings (if
    // anyone wants to drive bars from the chromagram) can iterate.
    Q_PROPERTY(QVariantList chroma         READ chromaList         NOTIFY chromaUpdated)
    Q_PROPERTY(QVariantList chromaSmoothed READ chromaSmoothedList NOTIFY chromaUpdated)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)

public:
    static constexpr int PITCH_CLASSES = 12;

    explicit ChromaAnalyzer(QObject* parent = nullptr);
    ~ChromaAnalyzer() override;

    CqtAnalyzer* cqtSource() const { return m_cqtSource; }
    void         setCqtSource(CqtAnalyzer* s);

    bool active() const { return m_active; }
    void setActive(bool a);

    // QML-readable snapshots (allocate on call -- only invoked from QML).
    QVariantList chromaList() const;
    QVariantList chromaSmoothedList() const;

    // C++ direct accessors. Out buffer must hold PITCH_CLASSES floats.
    // Safe to call concurrently with onHopComplete -- protected by
    // m_outMutex, the same pattern CqtAnalyzer uses for fillRow().
    void fillChroma(float* out12);
    void fillChromaSmoothed(float* out12);

    // True after the first hop has populated the buffers. Useful for
    // consumers that want to suppress all-zero initial readings.
    bool haveChroma() const { return m_haveChroma.load(std::memory_order_acquire); }

    // Tuning knobs for the smoothing envelope. Sensible defaults baked in,
    // but exposed for future calibration. Re-derives the alpha
    // coefficients from the hop rate (set via setHopRate).
    Q_INVOKABLE void setAttackMs(double ms);
    Q_INVOKABLE void setReleaseMs(double ms);
    Q_INVOKABLE void setHopRate(double hz);

signals:
    void cqtSourceChanged();
    void chromaUpdated();
    void activeChanged();

private slots:
    void onHopComplete();

private:
    void recomputeAlphas();

    CqtAnalyzer* m_cqtSource = nullptr;

    // Tuning -- chosen so the envelope tracks chord changes (~hundreds of
    // ms) without flickering on per-hop pitch-detection noise.
    double m_tauAttackSec  = 0.050;   // 50 ms attack
    double m_tauReleaseSec = 0.250;   // 250 ms release
    double m_hopRateHz     = 60.0;    // matches AudioFeatures refresh

    float  m_alphaAttack  = 0.0f;
    float  m_alphaRelease = 0.0f;

    bool   m_active = true;

    // Quarter-tone Gaussian sigma in bins (so offset=1 -> ~62% weight,
    // offset=0 -> 100% weight). With B=24 (CqtAnalyzer default) each PC
    // owns two consecutive bins per octave, so we accumulate at offsets
    // -1, 0, +1 around the in-tune bin -- the +1 catches the quarter-
    // tone-sharp neighbour, the -1 catches the quarter-tone-flat one
    // from the next-higher semitone.
    static constexpr float kGaussianSigmaBins = 1.0f;

    // Snapshot buffer of raw CQT magnitudes pulled each hop. Sized to
    // accommodate CqtAnalyzer's MAX_BINS without dynamic alloc.
    std::array<float, 384> m_cqtScratch {};

    // Output buffers -- raw normalized + envelope-smoothed. Updated under
    // m_outMutex by onHopComplete; readers snapshot under the same lock.
    QMutex                                m_outMutex;
    std::array<float, PITCH_CLASSES>      m_chroma {};         // [0,1]
    std::array<float, PITCH_CLASSES>      m_chromaSmoothed {}; // [0,1]
    std::atomic<bool>                     m_haveChroma {false};
};
