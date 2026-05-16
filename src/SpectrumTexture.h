// SpectrumTexture — a 512×2 R8 texture pump for shader-based visualizers.
//
// Wraps AudioFeatures::fillSpectrumRow + fillWaveformRow into a single
// QQuickItem that can be used directly as a `ShaderEffect.source` (via
// QSGTextureProvider) or sampled by an `Image { source: ... }`.
//
// Row 0: smoothed log-magnitude spectrum, [-100, -30] dB → [0, 255].
//        Matches ShaderToy's iChannel0 row-0 convention so shaders that
//        were authored for the web port can drop straight in.
// Row 1: mono waveform, [-1, +1] → [0, 255], 128 = silence.
//        Matches ShaderToy row-1.
//
// Lifetime: the item holds a non-owning AudioFeatures*; the caller (main.cpp)
// guarantees the AudioFeatures outlives every SpectrumTexture instance.
// The texture itself is owned by the scene graph and lives on the render
// thread; bytes get re-uploaded inside updatePaintNode whenever a new
// `featuresUpdated()` has fired since the last paint.

#pragma once

#include <QImage>
#include <QQuickItem>
#include <atomic>
#include <qqmlregistration.h>

class AudioFeatures;
class QSGTexture;
class QSGTextureProvider;

class SpectrumTexture : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(AudioFeatures* features READ features WRITE setFeatures NOTIFY featuresChanged)

public:
    explicit SpectrumTexture(QQuickItem* parent = nullptr);
    ~SpectrumTexture() override;

    AudioFeatures* features() const { return m_features; }
    void setFeatures(AudioFeatures* f);

    // QQuickItem texture-provider implementation. Lets a ShaderEffect
    // bind us as a source via `property variant src: spectrumTexture`.
    bool isTextureProvider() const override { return true; }
    QSGTextureProvider* textureProvider() const override;

    // Scene-graph paint hook. Runs on the render thread.
    QSGNode* updatePaintNode(QSGNode* node, UpdatePaintNodeData*) override;

signals:
    void featuresChanged();

protected:
    void itemChange(ItemChange change, const ItemChangeData& value) override;
    void releaseResources() override;

private slots:
    void onFeaturesUpdated();

private:
    AudioFeatures*       m_features = nullptr;
    std::atomic<bool>    m_dirty {true};   // bytes need re-upload?

    // Owned by the render thread when alive; nullptr on the GUI thread.
    class TextureProvider;
    mutable TextureProvider* m_provider = nullptr;
};
