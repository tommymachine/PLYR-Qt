#include "SsmTexture.h"
#include "SsmLoader.h"

#include <QImage>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QSGTextureProvider>
#include <cstring>


// Render-thread texture-provider wrapper -- holds the QSGTexture pointer
// updatePaintNode produces and re-publishes via textureChanged whenever
// the texture is rebuilt (which happens once per loader change).
class SsmTexture::TextureProvider : public QSGTextureProvider {
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


SsmTexture::SsmTexture(QQuickItem* parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    // Default size = 1x1 placeholder; ShaderEffects anchor.fill us anyway.
    setSize(QSizeF(1, 1));
}


SsmTexture::~SsmTexture()
{
    if (m_provider) {
        m_provider->deleteLater();
        m_provider = nullptr;
    }
}


void SsmTexture::setLoader(SsmLoader* l)
{
    if (m_loader == l) return;
    if (m_loader) {
        disconnect(m_loader, &SsmLoader::loadedChanged,
                   this, &SsmTexture::onLoaderChanged);
    }
    m_loader = l;
    if (m_loader) {
        connect(m_loader, &SsmLoader::loadedChanged,
                this, &SsmTexture::onLoaderChanged,
                Qt::DirectConnection);
        m_dirty.store(true, std::memory_order_release);
        update();
    }
    emit loaderChanged();
}


void SsmTexture::onLoaderChanged()
{
    m_dirty.store(true, std::memory_order_release);
    update();
}


QSGTextureProvider* SsmTexture::textureProvider() const
{
    if (!m_provider) m_provider = new TextureProvider();
    return m_provider;
}


void SsmTexture::itemChange(ItemChange change, const ItemChangeData& data)
{
    QQuickItem::itemChange(change, data);
    if (change == ItemSceneChange && !data.window && m_provider) {
        m_provider->setTexture(nullptr);
    }
}


void SsmTexture::releaseResources()
{
    if (m_provider && window()) {
        m_provider->deleteLater();
        m_provider = nullptr;
    }
}


QSGNode* SsmTexture::updatePaintNode(QSGNode* oldNode,
                                     UpdatePaintNodeData* /*data*/)
{
    auto* node = static_cast<QSGSimpleTextureNode*>(oldNode);
    if (!node) {
        node = new QSGSimpleTextureNode();
        node->setOwnsTexture(true);
        node->setFiltering(QSGTexture::Linear);
    }

    QQuickWindow* win = window();
    if (!win) return node;

    const bool need = m_dirty.exchange(false, std::memory_order_acq_rel);
    QSGTexture* tex = node->texture();
    if ((!tex || need)) {
        // Build the texture only if the loader is loaded; otherwise the
        // shader sampling will just see whatever was there last (initially
        // nothing -> ShaderEffect renders the placeholder fallback path).
        if (m_loader && m_loader->loaded()) {
            const int T = m_loader->dim();
            QImage img(T, T, QImage::Format_Grayscale8);
            // Snapshot the matrix into the QImage row by row -- QImage
            // scanlines are aligned to 4-byte boundaries, so for odd T
            // we can't just memcpy the whole thing in one shot.
            const auto& mat = m_loader->matrixRef();
            for (int r = 0; r < T; ++r) {
                std::memcpy(img.scanLine(r),
                            &mat[size_t(r) * size_t(T)],
                            size_t(T));
            }
            QSGTexture* newTex = win->createTextureFromImage(img);
            newTex->setHorizontalWrapMode(QSGTexture::ClampToEdge);
            newTex->setVerticalWrapMode(QSGTexture::ClampToEdge);
            newTex->setFiltering(QSGTexture::Linear);
            node->setTexture(newTex);
            node->setOwnsTexture(true);
            if (m_provider) m_provider->setTexture(newTex);
        } else if (m_provider) {
            m_provider->setTexture(nullptr);
            node->setTexture(nullptr);
        }
    }

    node->setRect(0.0, 0.0, width(), height());
    return node;
}
