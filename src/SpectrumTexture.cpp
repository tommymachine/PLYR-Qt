#include "SpectrumTexture.h"
#include "AudioFeatures.h"

#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QSGTextureProvider>
#include <cstring>


// Render-thread texture-provider wrapper. Holds the QSGTexture pointer
// that updatePaintNode() produces and re-publishes it via textureChanged
// whenever the bytes were re-uploaded.
class SpectrumTexture::TextureProvider : public QSGTextureProvider {
public:
    QSGTexture* texture() const override { return m_tex; }
    void setTexture(QSGTexture* t) {
        if (m_tex != t) {
            m_tex = t;
            emit textureChanged();
        }
    }
private:
    QSGTexture* m_tex = nullptr;
};


SpectrumTexture::SpectrumTexture(QQuickItem* parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    // Logical size matches the texture so a 1:1 ShaderEffect source pump
    // doesn't need its own size hints.
    setSize(QSizeF(AudioFeatures::SPECTRUM_BINS, 2));
}


SpectrumTexture::~SpectrumTexture()
{
    // releaseResources() handles the in-window teardown path. If the
    // item never made it into a window (m_provider stays null) there's
    // nothing to free. Otherwise the provider was already deleteLater'd.
    // The remaining edge case — destruction without releaseResources()
    // ever firing — is rare in practice; rely on deleteLater here so
    // we don't accidentally touch render-thread state from the GUI dtor.
    if (m_provider) {
        m_provider->deleteLater();
        m_provider = nullptr;
    }
}


void SpectrumTexture::setFeatures(AudioFeatures* f)
{
    if (m_features == f) return;
    if (m_features) {
        disconnect(m_features, &AudioFeatures::featuresUpdated,
                   this, &SpectrumTexture::onFeaturesUpdated);
    }
    m_features = f;
    if (m_features) {
        connect(m_features, &AudioFeatures::featuresUpdated,
                this, &SpectrumTexture::onFeaturesUpdated,
                Qt::DirectConnection);
        m_dirty.store(true);
        update();
    }
    emit featuresChanged();
}


void SpectrumTexture::onFeaturesUpdated()
{
    m_dirty.store(true);
    // Queue a render-thread visit. update() is thread-safe per Qt docs.
    update();
}


QSGTextureProvider* SpectrumTexture::textureProvider() const
{
    if (!m_provider) {
        // Allocated lazily on the render thread. textureProvider() is
        // documented as being called from the render thread when the item
        // is being prepared as a ShaderEffect source.
        m_provider = new TextureProvider();
    }
    return m_provider;
}


void SpectrumTexture::itemChange(ItemChange change, const ItemChangeData& data)
{
    QQuickItem::itemChange(change, data);
    if (change == ItemSceneChange && !data.window && m_provider) {
        // Leaving the window — drop the provider's texture reference so
        // QSGTexture cleanup happens cleanly on the right thread.
        m_provider->setTexture(nullptr);
    }
}


void SpectrumTexture::releaseResources()
{
    // Hand the provider off to the render thread for deletion. Mirrors
    // QQuickShaderEffectSource's teardown pattern from the Qt source.
    if (m_provider && window()) {
        m_provider->deleteLater();
        m_provider = nullptr;
    }
}


QSGNode* SpectrumTexture::updatePaintNode(QSGNode* oldNode,
                                          UpdatePaintNodeData* /*data*/)
{
    // We only build a node if someone actually wants a paintable item;
    // when used purely as a texture provider (ShaderEffect.source), the
    // node body is still drawn (zero-area), and the provider exposure
    // means ShaderEffect picks the texture up via textureProvider().
    auto* node = static_cast<QSGSimpleTextureNode*>(oldNode);
    if (!node) {
        node = new QSGSimpleTextureNode();
        node->setOwnsTexture(true);
        node->setFiltering(QSGTexture::Nearest);
    }

    QQuickWindow* win = window();
    if (!win || !m_features) {
        return node;
    }

    // Re-upload bytes only when AudioFeatures has signalled a refresh
    // since our last visit. Atomic exchange so two consecutive paints
    // without an audio refresh skip the work.
    const bool needRefresh = m_dirty.exchange(false);

    QSGTexture* tex = node->texture();
    if (!tex || needRefresh) {
        // 512×2 Grayscale8 → R8 on the GPU side. QImage's Grayscale8
        // format is exactly 1 byte per pixel, tightly packed (modulo
        // alignment), which is what we want.
        QImage img(AudioFeatures::SPECTRUM_BINS, 2, QImage::Format_Grayscale8);
        // Row 0: spectrum. Row 1: waveform.
        m_features->fillSpectrumRow(img.scanLine(0));
        m_features->fillWaveformRow(img.scanLine(1));

        if (tex) {
            // QQuickWindow takes ownership through setTexture below; the
            // previous tex was owned by the node (setOwnsTexture(true))
            // and will be freed when we replace it.
        }
        QSGTexture* newTex = win->createTextureFromImage(img);
        node->setTexture(newTex);
        node->setOwnsTexture(true);

        if (m_provider) m_provider->setTexture(newTex);
    }

    // Visible footprint = the item's own size. Zero by default (we set
    // SPECTRUM_BINS × 2 in the ctor — users can override via QML).
    node->setRect(0.0, 0.0, width(), height());
    return node;
}
