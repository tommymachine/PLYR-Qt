#include "CqtSpectrogram.h"
#include "CqtAnalyzer.h"
#include "AudioFeatures.h"

#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QSGTextureProvider>
#include <cstring>


// Render-thread texture-provider wrapper. Republishes the QSGTexture
// pointer updatePaintNode hands us; matches SpectrumTexture's pattern.
class CqtSpectrogram::TextureProvider : public QSGTextureProvider {
public:
    QSGTexture* texture() const override { return m_tex; }
    void setTexture(QSGTexture* t) {
        if (m_tex != t) { m_tex = t; emit textureChanged(); }
    }
private:
    QSGTexture* m_tex = nullptr;
};


CqtSpectrogram::CqtSpectrogram(QQuickItem* parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    // Logical size matches the texture footprint, mirroring
    // SpectrumTexture / SpectrumAnalyzer. ShaderEffects will override
    // this via anchors.fill on the QML side, so 1x1 is harmless.
    setSize(QSizeF(m_columns, m_bins));

    m_analyzer = std::make_unique<CqtAnalyzer>(
        nullptr,
        m_binsPerOctave,
        m_nOctaves,
        m_fMin,
        44100.0,
        4096);
    m_bins = m_analyzer->outputBins();

    // Ring buffer: pre-allocated to bins * columns bytes. Initial all-
    // zero (= -80 dB / black after color mapping) so the first paint
    // shows a clean canvas rather than uninitialized memory.
    m_ring.assign(size_t(m_bins) * size_t(m_columns), 0);

    connect(m_analyzer.get(), &CqtAnalyzer::hopComplete,
            this, &CqtSpectrogram::onHopComplete,
            Qt::DirectConnection);
}


CqtSpectrogram::~CqtSpectrogram()
{
    if (m_provider) {
        m_provider->deleteLater();
        m_provider = nullptr;
    }
}


void CqtSpectrogram::setAudioSource(AudioFeatures* s)
{
    if (m_audioSource == s) return;
    m_audioSource = s;
    if (m_analyzer) m_analyzer->setAudioSource(s);
    emit audioSourceChanged();
}


void CqtSpectrogram::setDbMin(float v)
{
    if (std::fabs(v - m_dbMin) < 1e-4f) return;
    m_dbMin = v;
    emit dbMinChanged();
    // Note: the analyzer encodes magnitudes against its own -80..0 dB
    // window. Externally adjusting m_dbMin here only affects future
    // colormap rescaling (planned for a later iteration); the bytes
    // already in the ring keep their original encoding. Documenting so
    // the shader-side gamma + colormap can be the user-facing scale.
}


void CqtSpectrogram::setDbMax(float v)
{
    if (std::fabs(v - m_dbMax) < 1e-4f) return;
    m_dbMax = v;
    emit dbMaxChanged();
}


void CqtSpectrogram::setAutoScroll(bool v)
{
    if (v == m_autoScroll) return;
    m_autoScroll = v;
    emit autoScrollChanged();
}


void CqtSpectrogram::setActive(bool a)
{
    if (m_active == a) return;
    m_active = a;
    if (m_analyzer) m_analyzer->setActive(a);
    emit activeChanged();
}


// --- Hop -> ring -------------------------------------------------------------

void CqtSpectrogram::onHopComplete()
{
    if (!m_active) return;
    if (!m_autoScroll) return;

    // Snapshot the latest CQT row into the ring at m_writeCol. Column-
    // major in time means row-major in the texture (one row per pitch),
    // so each pitch p writes to ring[p * columns + writeCol]. We avoid
    // building a temporary row buffer by calling fillRow into a stack
    // scratch and then scattering into the ring columns under the lock.
    uint8_t tmp[CqtAnalyzer::MAX_BINS];
    if (!m_analyzer->fillRow(tmp, m_bins)) return;

    {
        QMutexLocker lk(&m_ringMutex);
        const int col = m_writeCol.load(std::memory_order_relaxed);
        for (int p = 0; p < m_bins; ++p) {
            m_ring[size_t(p) * size_t(m_columns) + size_t(col)] = tmp[p];
        }
        int next = col + 1;
        if (next >= m_columns) next = 0;
        m_writeCol.store(next, std::memory_order_release);
    }

    m_dirty.store(true, std::memory_order_release);
    emit scrollOffsetChanged();
    update();
}


// --- Scene-graph integration -------------------------------------------------

QSGTextureProvider* CqtSpectrogram::textureProvider() const
{
    if (!m_provider) m_provider = new TextureProvider();
    return m_provider;
}


void CqtSpectrogram::itemChange(ItemChange change, const ItemChangeData& data)
{
    QQuickItem::itemChange(change, data);
    if (change == ItemSceneChange && !data.window && m_provider) {
        m_provider->setTexture(nullptr);
    }
}


void CqtSpectrogram::releaseResources()
{
    if (m_provider && window()) {
        m_provider->deleteLater();
        m_provider = nullptr;
    }
}


QSGNode* CqtSpectrogram::updatePaintNode(QSGNode* oldNode,
                                         UpdatePaintNodeData* /*data*/)
{
    auto* node = static_cast<QSGSimpleTextureNode*>(oldNode);
    if (!node) {
        node = new QSGSimpleTextureNode();
        node->setOwnsTexture(true);
        // Linear filtering smooths the visible scroll between hops; with
        // hops at ~60 Hz the inter-column blur is barely perceptible.
        node->setFiltering(QSGTexture::Linear);
    }

    QQuickWindow* win = window();
    if (!win) return node;

    const bool need = m_dirty.exchange(false, std::memory_order_acq_rel);
    QSGTexture* tex = node->texture();
    if (!tex || need) {
        // Build the QImage on the render thread. The ring is sized
        // m_bins * m_columns; QImage Grayscale8 maps one byte per pixel,
        // tightly packed at width = m_columns (no row stride mismatch at
        // common widths like 1024). Snapshot under m_ringMutex so we
        // never see a half-updated column.
        QImage img(m_columns, m_bins, QImage::Format_Grayscale8);
        {
            QMutexLocker lk(&m_ringMutex);
            // QImage scanlines are rows; row p = pitch p. The ring is
            // row-major in pitch (lowest at row 0), but visually we want
            // the LOWEST pitch at the BOTTOM of the display. The fragment
            // shader flips v (1.0 - v) so v=0 is bottom; keep the texture
            // memory layout natural (row 0 = lowest pitch) and let the
            // shader flip.
            for (int p = 0; p < m_bins; ++p) {
                std::memcpy(img.scanLine(p),
                            m_ring.data() + size_t(p) * size_t(m_columns),
                            size_t(m_columns));
            }
        }
        QSGTexture* newTex = win->createTextureFromImage(img);
        // The ring carries semantically wraparound data. ClampToEdge
        // would still work because the fragment shader does mod(u, 1.0)
        // on its own, but Repeat lets the shader skip an explicit fract
        // call if it ever needs to. Set both to be safe.
        newTex->setHorizontalWrapMode(QSGTexture::Repeat);
        newTex->setVerticalWrapMode(QSGTexture::ClampToEdge);
        newTex->setFiltering(QSGTexture::Linear);
        node->setTexture(newTex);
        node->setOwnsTexture(true);
        if (m_provider) m_provider->setTexture(newTex);
    }

    node->setRect(0.0, 0.0, width(), height());
    return node;
}
