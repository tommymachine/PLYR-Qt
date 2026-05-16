// SpectrumAnalyzer — SPAN-style log-frequency spectrum analyzer that
// feeds shaders/spectrum.frag.
//
// Architectural shape mirrors SpectrumTexture: a QQuickItem that is its
// own QSGTextureProvider, re-uploads a tiny R8 1×N texture each refresh,
// and lets a sibling ShaderEffect bind it as `source`. The display curve
// is computed CPU-side (log-x remap, +N dB/oct tilt, optional fractional-
// octave smoothing), so the shader is a simple sampler — no log/pow in
// the per-pixel path.
//
// We need three textures (main curve / peak-hold / infinite-peak), each
// of which a ShaderEffect property has to be able to bind independently.
// Strategy: the SpectrumAnalyzer is the main-curve provider; it exposes
// two sibling QQuickItem* Q_PROPERTYs (peakProvider, infPeakProvider),
// each a TextureProviderItem instance owned by the analyzer. From QML:
//
//     SpectrumAnalyzer { id: an }
//     ShaderEffect {
//         property var source:        an
//         property var peakSource:    an.peakProvider
//         property var infPeakSource: an.infPeakProvider
//     }
//
// Threading: featuresUpdated arrives on the GUI thread (Layer 0's refresh
// runs on a QTimer). The analyzer reads the spectrum row, runs the CPU
// post-processing pass, and stages the three display buffers into
// pre-allocated arrays under a short m_outMutex. updatePaintNode (render
// thread) drains the staged buffers into QImage scanlines and uploads.

#pragma once

#include <QColor>
#include <QImage>
#include <QMutex>
#include <QQuickItem>
#include <array>
#include <atomic>
#include <chrono>
#include <qqmlregistration.h>

class AudioFeatures;
class QSGTexture;
class QSGTextureProvider;

class SpectrumAnalyzer : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT

    // Wired from QML: typically `audioSource: audioFeatures`. The analyzer
    // holds a non-owning pointer; the AudioFeatures lifetime is managed by
    // main.cpp and outlives any visualizer.
    Q_PROPERTY(AudioFeatures* audioSource READ audioSource WRITE setAudioSource
               NOTIFY audioSourceChanged)

    // FFT-row resolution we ask AudioFeatures for. Larger = finer frequency
    // resolution at the cost of more interpolation work in fillSpectrumRowFull.
    // Allowed: 512 / 1024 / 2048.
    Q_PROPERTY(int fftBins READ fftBins WRITE setFftBins NOTIFY fftBinsChanged)

    // Display-slope tilt in dB/octave applied AFTER converting bytes back to
    // dB but BEFORE the dB→[0,1] mapping for the texture upload.
    // Mastering-standard +4.5 dB/oct makes pink noise look ≈ flat. 0 = off.
    Q_PROPERTY(float displaySlope READ displaySlope WRITE setDisplaySlope
               NOTIFY displaySlopeChanged)

    // Fractional-octave smoothing (in octaves; e.g. 1/24 ≈ 0.0417 → "1/24 oct").
    // 0 = raw spectrum (default). Implemented as a moving average across log-
    // spaced display bins covering ± half the window.
    Q_PROPERTY(float smoothingOctaves READ smoothingOctaves WRITE setSmoothingOctaves
               NOTIFY smoothingOctavesChanged)

    // Peak-hold curve. Holds each bin's all-time-max for HOLD_MS then linearly
    // decays at DECAY_DB_PER_SEC. Resets to the current value when its trace
    // falls below the live curve.
    Q_PROPERTY(bool showPeakHold READ showPeakHold WRITE setShowPeakHold
               NOTIFY showPeakHoldChanged)

    // Infinite peak — never decays unless resetPeakHold() is called.
    Q_PROPERTY(bool showInfinitePeak READ showInfinitePeak WRITE setShowInfinitePeak
               NOTIFY showInfinitePeakChanged)

    // Display window bounds.
    Q_PROPERTY(float dBMin    READ dBMin    WRITE setDBMin    NOTIFY dBMinChanged)
    Q_PROPERTY(float dBMax    READ dBMax    WRITE setDBMax    NOTIFY dBMaxChanged)
    Q_PROPERTY(float freqMin  READ freqMin  WRITE setFreqMin  NOTIFY freqMinChanged)
    Q_PROPERTY(float freqMax  READ freqMax  WRITE setFreqMax  NOTIFY freqMaxChanged)

    Q_PROPERTY(QColor fillTint READ fillTint WRITE setFillTint NOTIFY fillTintChanged)

    // Sibling texture providers for the peak-hold curve and the infinite-peak
    // curve. Lazy-allocated; each is a tiny QQuickItem whose only job is to
    // implement isTextureProvider/textureProvider over its assigned buffer.
    Q_PROPERTY(QQuickItem* peakProvider    READ peakProvider    CONSTANT)
    Q_PROPERTY(QQuickItem* infPeakProvider READ infPeakProvider CONSTANT)

public:
    // Display curve resolution. Width of the 1×N texture pumped to the
    // shader. Fixed at 1024 — that's effectively the horizontal resolution
    // of the analyzer; higher resolutions buy nothing once the shader is
    // texture-linear-sampling onto a few hundred display pixels.
    static constexpr int DISPLAY_BINS = 1024;

    explicit SpectrumAnalyzer(QQuickItem* parent = nullptr);
    ~SpectrumAnalyzer() override;

    AudioFeatures* audioSource() const { return m_source; }
    void setAudioSource(AudioFeatures* s);

    int  fftBins() const { return m_fftBins; }
    void setFftBins(int n);

    float displaySlope() const { return m_displaySlope; }
    void  setDisplaySlope(float s);

    float smoothingOctaves() const { return m_smoothingOctaves; }
    void  setSmoothingOctaves(float v);

    bool showPeakHold() const { return m_showPeakHold; }
    void setShowPeakHold(bool v);

    bool showInfinitePeak() const { return m_showInfinitePeak; }
    void setShowInfinitePeak(bool v);

    float dBMin() const { return m_dBMin; }
    void  setDBMin(float v);
    float dBMax() const { return m_dBMax; }
    void  setDBMax(float v);

    float freqMin() const { return m_freqMin; }
    void  setFreqMin(float v);
    float freqMax() const { return m_freqMax; }
    void  setFreqMax(float v);

    QColor fillTint() const { return m_fillTint; }
    void   setFillTint(const QColor& c);

    QQuickItem* peakProvider() const;
    QQuickItem* infPeakProvider() const;

    // Reset the infinite-peak buffer to silence. Cheap; safe to call from QML.
    Q_INVOKABLE void resetPeakHold();

    // QQuickItem texture-provider implementation — exposes the main display
    // curve (m_displayCurve8) as a 1×DISPLAY_BINS R8 texture.
    bool isTextureProvider() const override { return true; }
    QSGTextureProvider* textureProvider() const override;

    QSGNode* updatePaintNode(QSGNode* node, UpdatePaintNodeData*) override;

signals:
    void audioSourceChanged();
    void fftBinsChanged();
    void displaySlopeChanged();
    void smoothingOctavesChanged();
    void showPeakHoldChanged();
    void showInfinitePeakChanged();
    void dBMinChanged();
    void dBMaxChanged();
    void freqMinChanged();
    void freqMaxChanged();
    void fillTintChanged();

protected:
    void itemChange(ItemChange change, const ItemChangeData& value) override;
    void releaseResources() override;

private slots:
    void onFeaturesUpdated();

private:
    // CPU pipeline (on the GUI thread, inside onFeaturesUpdated):
    //   1. fillSpectrumRowFull → m_rawBytes  (raw [0,255] from AudioFeatures)
    //   2. byte → dB → +slope tilt → [0,1] linear  (m_processed)
    //   3. Optional fractional-octave smoothing across log-spaced bins
    //   4. Log-x remap into m_displayCurve (DISPLAY_BINS samples)
    //   5. Peak-hold + infinite-peak update
    //   6. Pack the three [0,1] curves to uint8 staging buffers under m_outMutex
    void runPipeline();

    AudioFeatures* m_source = nullptr;

    // Properties (with sensible mastering defaults).
    int    m_fftBins         = 1024;
    float  m_displaySlope    = 4.5f;
    float  m_smoothingOctaves = 0.0f;
    bool   m_showPeakHold    = true;
    bool   m_showInfinitePeak = false;
    float  m_dBMin   = -90.0f;
    float  m_dBMax   =   0.0f;
    float  m_freqMin =    20.0f;
    float  m_freqMax = 20000.0f;
    QColor m_fillTint = QColor(255, 255, 255);  // unused at this layer; the
                                                // shader picks up the palette

    // Pipeline scratch (GUI thread only). Sized for the maxima so no realloc
    // ever fires inside runPipeline().
    std::array<uint8_t, 2048> m_rawBytes {};   // up to max fftBins
    std::array<float,   2048> m_processed {};  // dB-tilted, [0,1] linear
    std::array<float, DISPLAY_BINS> m_displayCurve {};
    std::array<float, DISPLAY_BINS> m_peakCurve {};
    std::array<float, DISPLAY_BINS> m_infPeakCurve {};

    // Per-display-bin "hold until this time" + last-value bookkeeping for the
    // 600-ms hold / 30 dB/sec decay rule.
    std::array<std::chrono::steady_clock::time_point, DISPLAY_BINS> m_peakHoldUntil {};
    std::chrono::steady_clock::time_point m_lastPipelineTime {};

    // Staging — written under m_outMutex, drained by updatePaintNode (render
    // thread). uint8 because that's the texture format we want anyway.
    QMutex m_outMutex;
    std::array<uint8_t, DISPLAY_BINS> m_displayBytes {};
    std::array<uint8_t, DISPLAY_BINS> m_peakBytes {};
    std::array<uint8_t, DISPLAY_BINS> m_infPeakBytes {};
    std::atomic<bool> m_dirty {true};

    // Texture provider for our own (main-curve) texture.
    class TextureProvider;
    mutable TextureProvider* m_provider = nullptr;

    // The two sibling items expose the peak / infpeak buffers via the same
    // staging arrays. Live for the analyzer's whole lifetime.
    class BufferProviderItem;
    BufferProviderItem* m_peakItem    = nullptr;
    BufferProviderItem* m_infPeakItem = nullptr;
};
