// SsmTexture -- a QQuickItem that exposes a loaded SsmLoader as an
// R8 grayscale texture at the matrix's natural T x T resolution.
//
// Pattern mirrors SpectrumTexture / CqtSpectrogram: own a single
// QSGTexture on the render thread, republish it via a TextureProvider so
// a ShaderEffect can bind us as `property var source: ssmTex`. The
// difference from those classes is that our backing data is immutable
// once SsmLoader::loaded becomes true -- so the texture is created at
// most once per loader.sourceFile change.

#pragma once

#include <QQuickItem>
#include <atomic>
#include <qqmlregistration.h>

class SsmLoader;
class QSGTexture;
class QSGTextureProvider;

class SsmTexture : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(SsmLoader* loader READ loader WRITE setLoader NOTIFY loaderChanged)

public:
    explicit SsmTexture(QQuickItem* parent = nullptr);
    ~SsmTexture() override;

    SsmLoader* loader() const { return m_loader; }
    void       setLoader(SsmLoader* l);

    bool isTextureProvider() const override { return true; }
    QSGTextureProvider* textureProvider() const override;

    QSGNode* updatePaintNode(QSGNode* node, UpdatePaintNodeData*) override;

signals:
    void loaderChanged();

protected:
    void itemChange(ItemChange change, const ItemChangeData& value) override;
    void releaseResources() override;

private slots:
    void onLoaderChanged();

private:
    SsmLoader*           m_loader = nullptr;
    std::atomic<bool>    m_dirty {true};

    class TextureProvider;
    mutable TextureProvider* m_provider = nullptr;
};
