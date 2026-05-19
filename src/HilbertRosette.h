// HilbertRosette -- "Hilbert-pair rosette" visualizer (Layer 4c).
//
// 8 dots orbit around a common center, one per log-spaced subband. Each
// dot's radius is its band envelope; each dot's angular position is the
// sum of its band-specific base angle (where it "lives") and the
// instantaneous phase of its analytic signal. The instantaneous phase
// rotates at the band's center frequency, so low-frequency bass dots
// crawl while treble dots zip; together the eight overlapping orbits
// trace out a beating rosette / Lissajous bouquet.
//
// Render pipeline (mirrors ScopeRenderer's three-pass phosphor pattern):
//
//   pass A   prevAccum * decay      -> newAccum   (phosphor fade)
//   pass A'  N_BANDS additive splats -> newAccum   (each dot a Gaussian)
//   pass B   newAccum               -> colorTexture (composite, tinted = white)
//
// Two RGBA8 accumulation textures ping-pong. No allocations after the
// first resize. The dot pipeline reuses ScopeRenderer's idiom of writing
// premultiplied-alpha RGBA from the splat fragment shader, then letting
// additive blending sum the splats into the accumulation buffer.
//
// References (clean-room; algorithms, not code):
//   - The analytic signal -> dot orbit mapping is the standard
//     instantaneous-amplitude / instantaneous-frequency reading from
//     Bedrosian (1962), "A product theorem for Hilbert transforms",
//     Proc. IRE 50(5), 868-869 -- the foundational result that the
//     analytic envelope of a narrowband signal *is* the band's slowly-
//     varying amplitude.
//   - Phosphor decay implemented as exp(-dt/tau) (per-frame multiplier);
//     same form ScopeRenderer (Layer 1a) uses.

#pragma once

#include <QColor>
#include <QMutex>
#include <QQuickRhiItem>
#include <array>
#include <atomic>
#include <qqmlregistration.h>
#include <vector>

#include "HilbertAnalyzer.h"

class AudioFeatures;
class HilbertRosetteImpl;

class HilbertRosette : public QQuickRhiItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(AudioFeatures* audioSource READ audioSource WRITE setAudioSource
               NOTIFY audioSourceChanged)
    Q_PROPERTY(float trailTau     READ trailTau     WRITE setTrailTau
               NOTIFY trailTauChanged)
    Q_PROPERTY(float dotRadius    READ dotRadius    WRITE setDotRadius
               NOTIFY dotRadiusChanged)
    Q_PROPERTY(float ringRadius   READ ringRadius   WRITE setRingRadius
               NOTIFY ringRadiusChanged)
    Q_PROPERTY(bool  showBaseRing READ showBaseRing WRITE setShowBaseRing
               NOTIFY showBaseRingChanged)
    Q_PROPERTY(bool  showBandLabels READ showBandLabels WRITE setShowBandLabels
               NOTIFY showBandLabelsChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)

public:
    static constexpr int N_BANDS = HilbertAnalyzer::N_BANDS;

    explicit HilbertRosette(QQuickItem* parent = nullptr);
    ~HilbertRosette() override;

    AudioFeatures* audioSource() const { return m_source; }
    void           setAudioSource(AudioFeatures* s);

    float trailTau() const { return m_trailTau; }
    void  setTrailTau(float v);

    float dotRadius() const { return m_dotRadius; }
    void  setDotRadius(float v);

    float ringRadius() const { return m_ringRadius; }
    void  setRingRadius(float v);

    bool  showBaseRing() const { return m_showBaseRing; }
    void  setShowBaseRing(bool v);

    bool  showBandLabels() const { return m_showBandLabels; }
    void  setShowBandLabels(bool v);

    bool  active() const { return m_active.load(std::memory_order_relaxed); }
    void  setActive(bool a);

    // The fixed log-spaced band centers (Hz) -- forwarded from the
    // internal analyzer so QML can label the bands without owning its
    // own HilbertAnalyzer instance.
    Q_INVOKABLE QVariantList bandCenters() const;

protected:
    QQuickRhiItemRenderer* createRenderer() override;

signals:
    void audioSourceChanged();
    void trailTauChanged();
    void dotRadiusChanged();
    void ringRadiusChanged();
    void showBaseRingChanged();
    void showBandLabelsChanged();
    void activeChanged();

private slots:
    void onBandsUpdated();

private:
    friend class HilbertRosetteImpl;

    AudioFeatures*    m_source   = nullptr;
    HilbertAnalyzer*  m_analyzer = nullptr;   // owned (composition)

    // Staged latest snapshot (env/phase/instFreq) plus the current
    // pixel-size of the item, all under m_stageMutex. The renderer
    // pulls a copy in synchronize().
    QMutex                    m_stageMutex;
    std::array<float, N_BANDS> m_stagedEnv   {};
    std::array<float, N_BANDS> m_stagedPhase {};
    std::array<float, N_BANDS> m_stagedFreq  {};
    std::atomic<bool>         m_stagedDirty { false };

    // Tuneables. Defaults match the prompt.
    float m_trailTau    = 0.15f;          // s -- phosphor decay tau
    float m_dotRadius   = 8.0f;           // px -- Gaussian splat sigma scale
    float m_ringRadius  = 0.40f;          // fraction of half-min(w, h)
    bool  m_showBaseRing  = true;
    bool  m_showBandLabels = false;       // QML side; here for completeness

    std::atomic<bool> m_active {true};
};
