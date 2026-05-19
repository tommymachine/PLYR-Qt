// ScopeRenderer — analytic-line-integral oscilloscope / vectorscope with
// phosphor persistence, drawn through Qt RHI.
//
// Backing item: QQuickRhiItem (Qt 6.7+). The per-frame visible texture
// (colorTexture()) is the composite of an internal accumulation buffer
// that lives across frames. Pipeline:
//
//   pass A  prevAccum  ─ * decay ──► newAccum     (phosphor fade)
//   pass B  segments   ─ additive ─► newAccum     (1023 woscope quads)
//   pass C  newAccum   ─ * tint ───► colorTexture (composite to visible)
//
// Two accumulation textures ping-pong; no copies, no allocations after
// the first resize. The woscope shader (see shaders/scope_segment.frag)
// renders one quad per segment, evaluating the closed-form Gaussian
// line integral so beam-velocity modulation comes for free — short
// segments glow brighter, long ones dimmer, exactly like a CRT.
//
// Threading: the QML wrapper drives setAudioSource() on the GUI thread;
// the ScopeRenderer reads the 1024-sample scratch via AudioFeatures'
// short m_outMutex on the GUI thread inside onFeaturesUpdated(),
// computes segment endpoints, stages them, and calls update(). The
// renderer (lives on the render thread) picks the data up in its
// synchronize() callback.

#pragma once

#include <QColor>
#include <QMutex>
#include <QQuickRhiItem>
#include <array>
#include <atomic>
#include <qqmlregistration.h>
#include <vector>

class AudioFeatures;
class ScopeRendererImpl;

class ScopeRenderer : public QQuickRhiItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(Mode mode READ mode WRITE setMode NOTIFY modeChanged)
    Q_PROPERTY(float sigma READ sigma WRITE setSigma NOTIFY sigmaChanged)
    Q_PROPERTY(float decay READ decay WRITE setDecay NOTIFY decayChanged)
    Q_PROPERTY(float beamIntensity READ beamIntensity WRITE setBeamIntensity
               NOTIFY beamIntensityChanged)
    Q_PROPERTY(QColor beamColor READ beamColor WRITE setBeamColor
               NOTIFY beamColorChanged)
    Q_PROPERTY(bool stereoSeparated READ stereoSeparated WRITE setStereoSeparated
               NOTIFY stereoSeparatedChanged)
    // axisRotation, not rotation, to avoid hiding QQuickItem::rotation
    // (the 2D item-transform rotation). They mean different things — this
    // is the Lissajous-frame angle the L+R / L-R vectorscope is plotted in.
    Q_PROPERTY(float axisRotation READ axisRotation WRITE setAxisRotation
               NOTIFY axisRotationChanged)
    Q_PROPERTY(float audioGain READ audioGain WRITE setAudioGain NOTIFY audioGainChanged)

    // Named audioSource (not audioFeatures) to avoid a name collision with
    // the global QML context property of the same name — `scope.audioSource
    // = audioFeatures` reads cleanly from QML.
    Q_PROPERTY(AudioFeatures* audioSource READ audioSource WRITE setAudioSource
               NOTIFY audioSourceChanged)

    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)

public:
    enum class Mode { Oscilloscope, Vectorscope };
    Q_ENUM(Mode)

    static constexpr int SAMPLE_COUNT = 1024;
    static constexpr int SEGMENT_COUNT = SAMPLE_COUNT - 1;

    explicit ScopeRenderer(QQuickItem* parent = nullptr);
    ~ScopeRenderer() override;

    Mode mode() const { return m_mode; }
    void setMode(Mode m);

    float sigma() const { return m_sigma; }
    void setSigma(float v);

    float decay() const { return m_decay; }
    void setDecay(float v);

    float beamIntensity() const { return m_beamIntensity; }
    void setBeamIntensity(float v);

    QColor beamColor() const { return m_beamColor; }
    void setBeamColor(const QColor& c);

    bool stereoSeparated() const { return m_stereoSeparated; }
    void setStereoSeparated(bool v);

    float axisRotation() const { return m_axisRotation; }
    void setAxisRotation(float deg);

    float audioGain() const { return m_audioGain; }
    void setAudioGain(float v);

    AudioFeatures* audioSource() const { return m_source; }
    void setAudioSource(AudioFeatures* s);

    bool active() const { return m_active.load(std::memory_order_relaxed); }
    void setActive(bool a);

protected:
    QQuickRhiItemRenderer* createRenderer() override;

signals:
    void modeChanged();
    void sigmaChanged();
    void decayChanged();
    void beamIntensityChanged();
    void beamColorChanged();
    void stereoSeparatedChanged();
    void axisRotationChanged();
    void audioGainChanged();
    void audioSourceChanged();
    void activeChanged();

private slots:
    void onFeaturesUpdated();

private:
    friend class ScopeRendererImpl;

    void rebuildSegmentEndpoints();

    AudioFeatures* m_source = nullptr;

    Mode  m_mode = Mode::Oscilloscope;
    float m_sigma = 1.5f;
    float m_decay = 0.080f;
    float m_beamIntensity = 1.0f;
    QColor m_beamColor = QColor(0x34, 0xff, 0x34);
    bool  m_stereoSeparated = true;
    float m_axisRotation = 45.0f;
    float m_audioGain = 1.0f;
    std::atomic<bool> m_active {true};

    // Scratch buffers, GUI-thread only. Sized once at construction; no
    // resize/reallocate ever fires from the per-frame path.
    std::vector<float> m_sampleL;
    std::vector<float> m_sampleR;
    std::vector<float> m_endpointsXY;   // 2 floats per sample, packed (x,y)

    // Render-thread visible. The renderer pulls a fresh copy of the
    // endpoint pixel buffer in synchronize() under a short mutex.
    std::vector<float> m_stagedEndpointsPx;
    std::atomic<bool>  m_endpointsDirty{false};

    QMutex m_stageMutex;
};
