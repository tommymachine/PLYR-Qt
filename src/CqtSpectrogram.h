// CqtSpectrogram — scrolling 2D waterfall driven by CqtAnalyzer.
//
// Architectural shape: identical to SpectrumAnalyzer (Layer 1b). A
// QQuickItem that owns its DSP engine (CqtAnalyzer) and acts as a
// QSGTextureProvider for a downstream ShaderEffect. The texture is a
// (binsPerOctave * nOctaves) tall by `columns` wide R8 ring; each hop
// writes one fresh column at m_writeCol and increments the index. The
// fragment shader handles the wraparound by reading at
//   (u + scrollOffset/columns) mod 1.0
// so the texture itself never gets memcpy-shifted -- the scroll is free.
//
// Threading: the ring lives on the GUI thread, written under m_ringMutex
// in onHopComplete. updatePaintNode runs on the render thread, snapshots
// the ring into a QImage under the same mutex, and rebuilds the texture
// whenever a hop has marked us dirty. Re-uploading the whole 192 x 1024
// = 196 KB at 60 Hz costs ~12 MB/s, far below the bandwidth a per-
// subregion upload would save -- and lets us reuse the proven QSGNode
// + QImage path SpectrumTexture and SpectrumAnalyzer use.

#pragma once

#include <QImage>
#include <QMutex>
#include <QQuickItem>
#include <atomic>
#include <memory>
#include <qqmlregistration.h>
#include <vector>

class AudioFeatures;
class CqtAnalyzer;
class QSGTexture;
class QSGTextureProvider;

class CqtSpectrogram : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(AudioFeatures* audioSource READ audioSource WRITE setAudioSource
               NOTIFY audioSourceChanged)
    Q_PROPERTY(int binsPerOctave READ binsPerOctave NOTIFY binsPerOctaveChanged)
    Q_PROPERTY(int nOctaves      READ nOctaves      NOTIFY nOctavesChanged)
    Q_PROPERTY(double fMin       READ fMin          NOTIFY fMinChanged)
    Q_PROPERTY(int columns       READ columns       NOTIFY columnsChanged)
    Q_PROPERTY(float dbMin       READ dbMin WRITE setDbMin NOTIFY dbMinChanged)
    Q_PROPERTY(float dbMax       READ dbMax WRITE setDbMax NOTIFY dbMaxChanged)
    Q_PROPERTY(bool  autoScroll  READ autoScroll WRITE setAutoScroll
               NOTIFY autoScrollChanged)
    // Current write column. Used by the shader's wraparound math.
    Q_PROPERTY(int   scrollOffset READ scrollOffset NOTIFY scrollOffsetChanged)
    // Total bins exposed by the analyzer (binsPerOctave * nOctaves).
    Q_PROPERTY(int   bins          READ bins         NOTIFY binsChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)

public:
    static constexpr int DEFAULT_COLUMNS = 1024;

    explicit CqtSpectrogram(QQuickItem* parent = nullptr);
    ~CqtSpectrogram() override;

    AudioFeatures* audioSource() const { return m_audioSource; }
    void setAudioSource(AudioFeatures* s);

    int    binsPerOctave() const { return m_binsPerOctave; }
    int    nOctaves()      const { return m_nOctaves;      }
    double fMin()          const { return m_fMin;          }
    int    columns()       const { return m_columns;       }
    int    bins()          const { return m_bins;          }
    int    scrollOffset()  const { return m_writeCol.load(std::memory_order_relaxed); }

    float dbMin() const { return m_dbMin; }
    void  setDbMin(float v);
    float dbMax() const { return m_dbMax; }
    void  setDbMax(float v);

    bool  autoScroll() const { return m_autoScroll; }
    void  setAutoScroll(bool v);

    bool  active() const { return m_active; }
    void  setActive(bool a);

    // QQuickItem texture-provider plumbing.
    bool isTextureProvider() const override { return true; }
    QSGTextureProvider* textureProvider() const override;

    QSGNode* updatePaintNode(QSGNode* node, UpdatePaintNodeData*) override;

signals:
    void audioSourceChanged();
    void binsPerOctaveChanged();
    void nOctavesChanged();
    void fMinChanged();
    void columnsChanged();
    void binsChanged();
    void dbMinChanged();
    void dbMaxChanged();
    void autoScrollChanged();
    void scrollOffsetChanged();
    void activeChanged();

protected:
    void itemChange(ItemChange change, const ItemChangeData& value) override;
    void releaseResources() override;

private slots:
    // Connected to the internal CqtAnalyzer. Copies the latest CQT
    // magnitudes into the ring at m_writeCol and bumps the index.
    void onHopComplete();

private:
    AudioFeatures*               m_audioSource = nullptr;
    std::unique_ptr<CqtAnalyzer> m_analyzer;

    int    m_binsPerOctave = 24;
    int    m_nOctaves      = 8;
    double m_fMin          = 32.70;
    int    m_columns       = DEFAULT_COLUMNS;
    int    m_bins          = 192;
    float  m_dbMin         = -80.0f;
    float  m_dbMax         =   0.0f;
    bool   m_autoScroll    = true;
    bool   m_active        = true;

    // Ring: bins x columns R8, row-major (one row per pitch, lowest at
    // row 0). Written GUI-side; snapshotted under m_ringMutex by the
    // render thread. Pre-allocated to avoid per-hop heap traffic.
    QMutex             m_ringMutex;
    std::vector<uint8_t> m_ring;
    std::atomic<int>   m_writeCol {0};

    // Dirty bit -- flipped on every hop, cleared inside updatePaintNode.
    // Tracks whether the GPU-side texture is up to date.
    std::atomic<bool>  m_dirty {true};

    // Texture provider plumbing.
    class TextureProvider;
    mutable TextureProvider* m_provider = nullptr;
};
