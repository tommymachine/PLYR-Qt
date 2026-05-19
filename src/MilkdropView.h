// MilkdropView — Layer 3b QQuickRhiItem visualizer that hosts a
// MilkdropRuntime (NS-EEL2 expression evaluator) and renders a
// MilkDrop-style warp-mesh feedback loop into the item's visible
// surface.
//
// Pipeline per frame (after preset is loaded):
//
//   pass A   prev → warp[write] :
//            sample previous-frame texture at the per-vertex UV table
//            computed by MilkdropRuntime::runPerVertex; apply `decay`.
//            The warp mesh is a 32×24 triangle-strip-indexed grid
//            (kMeshW × kMeshH constants).
//
//   pass B   warp[write] → visibleColor :
//            full-screen blit + optional gamma correction.
//
// The two warp textures ping-pong so reading the previous-frame UV
// table at the sampler doesn't conflict with writing.
//
// Threading: identical to ScopeRenderer's pattern.
//   * GUI thread: setters on the QObject, audio-features signal
//     stages the latest scalar values.
//   * Render thread: pulls staged audio scalars in synchronize(),
//     runs the runtime, uploads buffers, draws.
// MilkdropRuntime itself is never touched outside the renderer
// (lives inside the MilkdropViewImpl instance).

#pragma once

#include <QColor>
#include <QMutex>
#include <QObject>
#include <QQuickRhiItem>
#include <QString>
#include <atomic>
#include <memory>
#include <qqmlregistration.h>

class AudioFeatures;
class MilkdropViewImpl;

class MilkdropView : public QQuickRhiItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(AudioFeatures* audioSource READ audioSource
               WRITE setAudioSource NOTIFY audioSourceChanged)

    // Path to the .milk preset file. May be a filesystem path or a
    // qrc:/... URL — QFile transparently handles both. Setting this
    // queues a reload on the next render pass.
    Q_PROPERTY(QString presetPath READ presetPath
               WRITE setPresetPath NOTIFY presetPathChanged)

    // Read-only text published by the runtime when parsing/compiling
    // fails — QML surfaces this for the user.
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)

public:
    explicit MilkdropView(QQuickItem* parent = nullptr);
    ~MilkdropView() override;

    AudioFeatures* audioSource() const { return m_audioSource; }
    void           setAudioSource(AudioFeatures* s);

    QString presetPath() const { return m_presetPath; }
    void    setPresetPath(const QString& p);

    QString lastError() const { return m_lastError; }

    bool active() const { return m_active.load(std::memory_order_relaxed); }
    void setActive(bool a);

protected:
    QQuickRhiItemRenderer* createRenderer() override;

signals:
    void audioSourceChanged();
    void presetPathChanged();
    void lastErrorChanged();
    void activeChanged();

private slots:
    // Pulls fresh bass/mid/treb from AudioFeatures into the staged
    // scalars on the GUI thread, then update()s the item.
    void onFeaturesUpdated();

private:
    friend class MilkdropViewImpl;

    // -- Staged inputs (GUI thread writes, render thread reads in sync) --
    struct StagedAudio {
        float bass     = 0.0f;
        float mid      = 0.0f;
        float treb     = 0.0f;
        float bass_att = 0.0f;
        float mid_att  = 0.0f;
        float treb_att = 0.0f;
    };

    AudioFeatures* m_audioSource = nullptr;

    QMutex     m_stagedMutex;
    StagedAudio m_staged;
    std::atomic<bool> m_audioDirty{false};

    QString m_presetPath;
    std::atomic<bool> m_presetDirty{false};   // → trigger reload in sync

    QString m_lastError;

    std::atomic<bool> m_active {true};
};
