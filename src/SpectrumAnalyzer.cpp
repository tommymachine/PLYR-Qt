#include "SpectrumAnalyzer.h"
#include "AudioFeatures.h"

#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QSGTextureProvider>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

// dB scale that matches AudioFeatures' spectrum-row contract. fillSpectrumRowFull
// produces bytes in [0, 255] where 0 = -100 dB and 255 = -30 dB. We invert that
// back to dB, apply the tilt + smoothing, then re-encode to [0, 1] using the
// display window the user picked (m_dBMin / m_dBMax — typically -90..0).
constexpr float kAudioFeaturesDbFloor   = -100.0f;
constexpr float kAudioFeaturesDbCeiling =  -30.0f;
constexpr float kAudioFeaturesDbSpan    = kAudioFeaturesDbCeiling - kAudioFeaturesDbFloor;

// Peak-hold parameters. SPAN convention: hold ~600 ms then linearly decay at
// ~30 dB/sec — fast enough to feel responsive, slow enough that a transient
// is visible long enough to read.
constexpr int   kPeakHoldMs            = 600;
constexpr float kPeakDecayDbPerSecond  = 30.0f;

// Internal "dB" reference for peak-hold storage. We store peak values in the
// normalized [0, 1] domain (after dBMin/dBMax remap), but the decay rate is
// dB-per-second, so we convert through whatever dB span the user has set.
//
// Math: dropping D dB over a dB-span of S maps to a Δ in normalized units of
// D / S. So per-second normalized decay = kPeakDecayDbPerSecond / (dBMax - dBMin).
inline float peakDecayPerSecondNorm(float dBMin, float dBMax) {
    const float span = std::max(1.0f, dBMax - dBMin);
    return kPeakDecayDbPerSecond / span;
}

// log-spaced index → linear FFT-bin index. SAMPLE-RATE-AWARE: x is in [0,1]
// across the display (freqMin → freqMax in log frequency), output is the
// fractional bin position into the FULL-NYQUIST spectrum (0..fftBins-1
// corresponds to 0..Nyquist).
//
// We assume the canonical 44.1 kHz sample rate AudioFeatures uses. (Querying
// it dynamically would require a getter; the entire DSP stack is locked to
// 44.1 kHz by AudioWorker, so this is a benign coupling.)
constexpr float kSampleRate = 44100.0f;
constexpr float kNyquist    = kSampleRate * 0.5f;

}  // namespace


// ---------------------------------------------------------------------------
//  Texture providers
// ---------------------------------------------------------------------------
//
// Both the main SpectrumAnalyzer and each BufferProviderItem use the same
// trivial QSGTextureProvider wrapper. We give it a friend-method twin so the
// owning QQuickItem can publish a new texture pointer after each upload.

class SpectrumAnalyzer::TextureProvider : public QSGTextureProvider {
public:
    QSGTexture* texture() const override { return m_tex; }
    void setTexture(QSGTexture* t) {
        if (m_tex != t) { m_tex = t; emit textureChanged(); }
    }
private:
    QSGTexture* m_tex = nullptr;
};


// Tiny pump-only QQuickItem. Holds a pointer into one of the analyzer's
// staging arrays + a dirty flag the analyzer flips on each pipeline run.
// Its updatePaintNode uploads the buffer into a 1×DISPLAY_BINS R8 texture
// and publishes it via its own QSGTextureProvider.
//
// Owned by the analyzer; lifetime is the analyzer's lifetime.
class SpectrumAnalyzer::BufferProviderItem : public QQuickItem {
public:
    BufferProviderItem(SpectrumAnalyzer* analyzer, const uint8_t* src)
        : QQuickItem(analyzer)
        , m_analyzer(analyzer)
        , m_src(src)
    {
        setFlag(ItemHasContents, true);
        // Logical size matches the texture footprint, same convention as
        // SpectrumTexture — keeps ShaderEffect from inventing a weird size.
        setSize(QSizeF(SpectrumAnalyzer::DISPLAY_BINS, 1));
    }

    ~BufferProviderItem() override {
        if (m_provider) {
            m_provider->deleteLater();
            m_provider = nullptr;
        }
    }

    bool isTextureProvider() const override { return true; }
    QSGTextureProvider* textureProvider() const override {
        if (!m_provider) m_provider = new SpectrumAnalyzer::TextureProvider();
        return m_provider;
    }

    // Called from the analyzer when staging is refreshed.
    void markDirty() { m_dirty.store(true); update(); }

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override {
        auto* node = static_cast<QSGSimpleTextureNode*>(oldNode);
        if (!node) {
            node = new QSGSimpleTextureNode();
            node->setOwnsTexture(true);
            node->setFiltering(QSGTexture::Linear);  // sub-pixel curve interp
        }
        QQuickWindow* win = window();
        if (!win) return node;

        const bool need = m_dirty.exchange(false);
        QSGTexture* tex = node->texture();
        if (!tex || need) {
            // 1×DISPLAY_BINS R8. Use Format_Grayscale8 — exactly one byte per
            // pixel, tightly packed (modulo Qt's row stride alignment, but at
            // width=1024 stride == width so it's a no-op).
            QImage img(SpectrumAnalyzer::DISPLAY_BINS, 1, QImage::Format_Grayscale8);
            // Snapshot under analyzer's mutex so we don't read mid-write.
            {
                QMutexLocker lk(&m_analyzer->m_outMutex);
                std::memcpy(img.scanLine(0), m_src, SpectrumAnalyzer::DISPLAY_BINS);
            }
            QSGTexture* newTex = win->createTextureFromImage(img);
            node->setTexture(newTex);
            node->setOwnsTexture(true);
            if (m_provider) m_provider->setTexture(newTex);
        }
        node->setRect(0.0, 0.0, width(), height());
        return node;
    }

    void releaseResources() override {
        if (m_provider && window()) {
            m_provider->deleteLater();
            m_provider = nullptr;
        }
    }

private:
    SpectrumAnalyzer*  m_analyzer = nullptr;
    const uint8_t*     m_src      = nullptr;
    std::atomic<bool>  m_dirty {true};
    mutable SpectrumAnalyzer::TextureProvider* m_provider = nullptr;
};


// ---------------------------------------------------------------------------
//  SpectrumAnalyzer
// ---------------------------------------------------------------------------

SpectrumAnalyzer::SpectrumAnalyzer(QQuickItem* parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    setSize(QSizeF(DISPLAY_BINS, 1));   // see SpectrumTexture rationale

    m_peakItem    = new BufferProviderItem(this, m_peakBytes.data());
    m_infPeakItem = new BufferProviderItem(this, m_infPeakBytes.data());

    m_lastPipelineTime = std::chrono::steady_clock::now();
}


SpectrumAnalyzer::~SpectrumAnalyzer()
{
    if (m_provider) {
        m_provider->deleteLater();
        m_provider = nullptr;
    }
    // m_peakItem / m_infPeakItem are QObject children → auto-deleted.
}


void SpectrumAnalyzer::setAudioSource(AudioFeatures* s)
{
    if (m_source == s) return;
    if (m_source) {
        disconnect(m_source, &AudioFeatures::featuresUpdated,
                   this, &SpectrumAnalyzer::onFeaturesUpdated);
    }
    m_source = s;
    if (m_source) {
        connect(m_source, &AudioFeatures::featuresUpdated,
                this, &SpectrumAnalyzer::onFeaturesUpdated,
                Qt::DirectConnection);
    }
    emit audioSourceChanged();
}


void SpectrumAnalyzer::setFftBins(int n)
{
    // We only meaningfully support the AudioFeatures-friendly sizes. Out-of-
    // range values are silently rejected — matches setFftSize's contract.
    if (n != 512 && n != 1024 && n != 2048) return;
    if (n == m_fftBins) return;
    m_fftBins = n;
    emit fftBinsChanged();
}

void SpectrumAnalyzer::setDisplaySlope(float s)
{
    // Negative slopes are unusual but mathematically fine; clamp to sane
    // mastering range so a typo from QML can't make the curve diverge.
    s = std::clamp(s, -12.0f, 12.0f);
    if (std::fabs(s - m_displaySlope) < 1e-4f) return;
    m_displaySlope = s;
    emit displaySlopeChanged();
}

void SpectrumAnalyzer::setSmoothingOctaves(float v)
{
    v = std::clamp(v, 0.0f, 2.0f);
    if (std::fabs(v - m_smoothingOctaves) < 1e-4f) return;
    m_smoothingOctaves = v;
    emit smoothingOctavesChanged();
}

void SpectrumAnalyzer::setShowPeakHold(bool v)
{
    if (v == m_showPeakHold) return;
    m_showPeakHold = v;
    emit showPeakHoldChanged();
}

void SpectrumAnalyzer::setShowInfinitePeak(bool v)
{
    if (v == m_showInfinitePeak) return;
    m_showInfinitePeak = v;
    if (!v) {
        // Switching infinite-peak off shouldn't keep stale highs visible
        // if the user toggles back on later. Zero the buffer.
        std::fill(m_infPeakCurve.begin(), m_infPeakCurve.end(), 0.0f);
    }
    emit showInfinitePeakChanged();
}

void SpectrumAnalyzer::setDBMin(float v) {
    if (std::fabs(v - m_dBMin) < 1e-4f) return;
    m_dBMin = v; emit dBMinChanged();
}
void SpectrumAnalyzer::setDBMax(float v) {
    if (std::fabs(v - m_dBMax) < 1e-4f) return;
    m_dBMax = v; emit dBMaxChanged();
}
void SpectrumAnalyzer::setFreqMin(float v) {
    v = std::max(1.0f, v);
    if (std::fabs(v - m_freqMin) < 1e-4f) return;
    m_freqMin = v; emit freqMinChanged();
}
void SpectrumAnalyzer::setFreqMax(float v) {
    v = std::min(kNyquist, std::max(m_freqMin + 1.0f, v));
    if (std::fabs(v - m_freqMax) < 1e-4f) return;
    m_freqMax = v; emit freqMaxChanged();
}
void SpectrumAnalyzer::setFillTint(const QColor& c) {
    if (c == m_fillTint) return;
    m_fillTint = c; emit fillTintChanged();
}

void SpectrumAnalyzer::setActive(bool a)
{
    if (m_active == a) return;
    m_active = a;
    emit activeChanged();
}

QQuickItem* SpectrumAnalyzer::peakProvider() const    { return m_peakItem; }
QQuickItem* SpectrumAnalyzer::infPeakProvider() const { return m_infPeakItem; }


void SpectrumAnalyzer::resetPeakHold()
{
    std::fill(m_peakCurve.begin(),    m_peakCurve.end(),    0.0f);
    std::fill(m_infPeakCurve.begin(), m_infPeakCurve.end(), 0.0f);
    const auto now = std::chrono::steady_clock::now();
    std::fill(m_peakHoldUntil.begin(), m_peakHoldUntil.end(), now);
    m_dirty.store(true);
    if (m_peakItem)    m_peakItem->markDirty();
    if (m_infPeakItem) m_infPeakItem->markDirty();
    update();
}


// ---------------------------------------------------------------------------
//  Pipeline (runs on the GUI thread per featuresUpdated)
// ---------------------------------------------------------------------------

void SpectrumAnalyzer::onFeaturesUpdated()
{
    if (!m_active) return;
    runPipeline();
    m_dirty.store(true);
    if (m_peakItem)    m_peakItem->markDirty();
    if (m_infPeakItem) m_infPeakItem->markDirty();
    update();
}


void SpectrumAnalyzer::runPipeline()
{
    if (!m_source) return;
    const int N = m_fftBins;
    if (N <= 0 || N > int(m_rawBytes.size())) return;

    // 1. Pull the full-Nyquist row from AudioFeatures.
    if (!m_source->fillSpectrumRowFull(m_rawBytes.data(), N)) return;

    // 2. byte → dB → tilt. The byte→dB inverse maps [0, 255] to [-100, -30] dB,
    //    matching AudioFeatures' clamp.
    //    Tilt: at frequency f (Hz), add `slope * log2(f / 1000)` dB. Reference
    //    pivot is 1 kHz, the convention SPAN / FabFilter use. Above 1 kHz this
    //    LIFTS the spectrum; below 1 kHz it cuts it. Pink noise (1/f power) on
    //    a +4.5 dB/oct display reads flat because its natural -3 dB/oct slope
    //    is mostly cancelled and the residual is mastering taste.
    const float binHz = kNyquist / float(N);
    for (int i = 0; i < N; ++i) {
        const float dbFromBytes = float(m_rawBytes[i]) / 255.0f * kAudioFeaturesDbSpan
                                + kAudioFeaturesDbFloor;
        // Bin-center frequency. The +0.5 mirrors fillSpectrumRowFull's mapping
        // so a 1 kHz sine peaks at the bin centered closest to 1 kHz.
        const float f = (float(i) + 0.5f) * binHz;
        const float tiltDb = (m_displaySlope != 0.0f && f > 0.0f)
                           ? m_displaySlope * std::log2(f / 1000.0f)
                           : 0.0f;
        const float dbTilted = dbFromBytes + tiltDb;
        // Display normalization: map [m_dBMin, m_dBMax] → [0, 1].
        const float span = std::max(1e-3f, m_dBMax - m_dBMin);
        const float v = std::clamp((dbTilted - m_dBMin) / span, 0.0f, 1.0f);
        m_processed[i] = v;
    }

    // 3. Optional fractional-octave smoothing. We compute the smoothing over
    //    LOG-spaced display bins (step 4 below already produces those), so
    //    we defer this until after the log-x remap — applying it here would
    //    blur unevenly across the audible range.

    // 4. Log-x remap into DISPLAY_BINS. For each display column u ∈ [0, 1],
    //    map to a log-spaced frequency f = freqMin * (freqMax/freqMin)^u,
    //    then linear-interpolate between the two adjacent FFT bins covering
    //    that frequency.
    //
    //    Edge case: tiny u values map to f below the first FFT bin (binHz/2).
    //    Clamp the source-bin index, so the bottom edge of the display reads
    //    the DC-adjacent bin instead of garbage.
    const float logFmin = std::log(m_freqMin);
    const float logFmax = std::log(m_freqMax);
    const float logFspan = logFmax - logFmin;

    for (int u = 0; u < DISPLAY_BINS; ++u) {
        const float t = (float(u) + 0.5f) / float(DISPLAY_BINS);
        const float f = std::exp(logFmin + t * logFspan);
        // Solve for the fractional FFT bin where this frequency lives. The
        // forward mapping in step 2 was f = (i + 0.5) * binHz; invert it.
        const float binF = (f / binHz) - 0.5f;
        const int   k0 = std::clamp(int(std::floor(binF)), 0, N - 1);
        const int   k1 = std::min(k0 + 1, N - 1);
        const float fr = std::clamp(binF - float(k0), 0.0f, 1.0f);
        m_displayCurve[u] = (1.0f - fr) * m_processed[k0] + fr * m_processed[k1];
    }

    // 3' (deferred). Fractional-octave smoothing. The display-bin step in log
    //    space is logFspan / DISPLAY_BINS NATs ≈ logFspan / DISPLAY_BINS / ln(2)
    //    octaves. Window half-width in display bins = (smoothingOctaves / 2) ÷
    //    (octaves-per-display-bin).
    if (m_smoothingOctaves > 0.0f) {
        const float octPerBin = (logFspan / float(DISPLAY_BINS)) / float(M_LN2);
        const int halfW = std::max(1, int(std::round(
            (m_smoothingOctaves * 0.5f) / std::max(1e-6f, octPerBin))));
        // Two-pass to allow in-place: temp = smoothed, then copy back.
        // The temp lives in m_peakCurve briefly — we have to recompute peak
        // anyway in step 5, and aliasing through the live curve would smear
        // the smoothing over itself. After the smoothing we'll overwrite
        // m_peakCurve in step 5.
        for (int i = 0; i < DISPLAY_BINS; ++i) {
            const int lo = std::max(0, i - halfW);
            const int hi = std::min(DISPLAY_BINS - 1, i + halfW);
            float sum = 0.0f;
            for (int j = lo; j <= hi; ++j) sum += m_displayCurve[j];
            m_peakCurve[i] = sum / float(hi - lo + 1);
        }
        std::memcpy(m_displayCurve.data(), m_peakCurve.data(),
                    sizeof(float) * DISPLAY_BINS);
    }

    // 5. Peak-hold + infinite peak.
    const auto now = std::chrono::steady_clock::now();
    const float dtSec = std::chrono::duration<float>(now - m_lastPipelineTime).count();
    m_lastPipelineTime = now;
    const float decayPerSec = peakDecayPerSecondNorm(m_dBMin, m_dBMax);
    const float decayThisFrame = decayPerSec * std::max(0.0f, dtSec);

    for (int i = 0; i < DISPLAY_BINS; ++i) {
        const float live = m_displayCurve[i];

        // Peak-hold trace. Live value resets the hold timer. Otherwise we
        // freeze the peak for kPeakHoldMs ms, then linearly decay.
        if (live >= m_peakCurve[i]) {
            m_peakCurve[i] = live;
            m_peakHoldUntil[i] = now + std::chrono::milliseconds(kPeakHoldMs);
        } else if (now > m_peakHoldUntil[i]) {
            m_peakCurve[i] = std::max(live, m_peakCurve[i] - decayThisFrame);
        }
        // Else: still inside the hold window; m_peakCurve[i] stays.

        // Infinite peak: just keep the max.
        if (live > m_infPeakCurve[i]) m_infPeakCurve[i] = live;
    }

    // 6. Pack the three curves into uint8 staging buffers.
    QMutexLocker outLk(&m_outMutex);
    for (int i = 0; i < DISPLAY_BINS; ++i) {
        m_displayBytes[i] = uint8_t(std::clamp(m_displayCurve[i] * 255.0f, 0.0f, 255.0f));
        m_peakBytes[i]    = uint8_t(std::clamp(m_peakCurve[i]    * 255.0f, 0.0f, 255.0f));
        m_infPeakBytes[i] = uint8_t(std::clamp(m_infPeakCurve[i] * 255.0f, 0.0f, 255.0f));
    }
}


// ---------------------------------------------------------------------------
//  Texture provider / scene-graph plumbing (main curve)
// ---------------------------------------------------------------------------

QSGTextureProvider* SpectrumAnalyzer::textureProvider() const
{
    if (!m_provider) m_provider = new TextureProvider();
    return m_provider;
}


void SpectrumAnalyzer::itemChange(ItemChange change, const ItemChangeData& data)
{
    QQuickItem::itemChange(change, data);
    if (change == ItemSceneChange && !data.window && m_provider) {
        m_provider->setTexture(nullptr);
    }
}


void SpectrumAnalyzer::releaseResources()
{
    if (m_provider && window()) {
        m_provider->deleteLater();
        m_provider = nullptr;
    }
}


QSGNode* SpectrumAnalyzer::updatePaintNode(QSGNode* oldNode,
                                           UpdatePaintNodeData* /*data*/)
{
    auto* node = static_cast<QSGSimpleTextureNode*>(oldNode);
    if (!node) {
        node = new QSGSimpleTextureNode();
        node->setOwnsTexture(true);
        node->setFiltering(QSGTexture::Linear);
    }

    QQuickWindow* win = window();
    if (!win || !m_source) return node;

    const bool need = m_dirty.exchange(false);
    QSGTexture* tex = node->texture();
    if (!tex || need) {
        QImage img(DISPLAY_BINS, 1, QImage::Format_Grayscale8);
        {
            QMutexLocker lk(&m_outMutex);
            std::memcpy(img.scanLine(0), m_displayBytes.data(), DISPLAY_BINS);
        }
        QSGTexture* newTex = win->createTextureFromImage(img);
        node->setTexture(newTex);
        node->setOwnsTexture(true);
        if (m_provider) m_provider->setTexture(newTex);
    }
    node->setRect(0.0, 0.0, width(), height());
    return node;
}
