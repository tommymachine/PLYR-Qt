// MfccTrajectory -- 3D sliding-window MFCC trajectory visualizer (Layer
// 2c). The musical evolution of a track is drawn as a fading 3D
// polyline: 13-D MFCC vectors are projected to R^3 by online PCA, then
// rendered as orbit-camera-projected thick lines with per-vertex
// age-based fade.
//
// References (algorithms only, no copied code):
//   - Chris Tralie, LoopDitty (HTML/JS; study-only) -- the sliding-
//     window dimensionality-reduction trick on MFCCs is the same.
//   - Tralie & Perea 2017, "Sliding Windows and Persistence"
//     (arxiv.org/abs/1703.04127) -- algorithmic backdrop for why this
//     embedding makes loops visible.
//
// Pipeline per MfccAnalyzer::mfccUpdated:
//   1. Pull the latest 13-D MFCC vector. Coefficient 0 is dropped (it
//      tracks gross loudness and would swamp the geometry); we project
//      coefficients 1..12 -- a 12-D vector.
//   2. Project to R^3 via the cached PCA basis: p3 = (mfcc - mean) * V.
//      V is a (12 x 3) matrix whose columns are the top-3 eigenvectors
//      of the recent-window covariance.
//   3. Append to a 600-row circular buffer of (x, y, z) points.
//   4. Every 30 hops (~0.5 s), recompute the PCA: snapshot the last 600
//      MFCC vectors, compute the mean + 12x12 covariance, Jacobi-
//      eigen-decompose, reorder + sign-flip the new top-3 eigenvectors
//      against the previous basis so the trajectory's orientation
//      doesn't snap on each refresh.
//
// Render per frame:
//   - Camera orbits the origin at cameraOrbitHz revolutions per second.
//   - 599 segments, each drawn as a screen-space-extruded quad. The
//     vertex shader projects both endpoints, computes the perpendicular
//     in NDC, expands the corner outward by lineWidth/viewportPx.
//   - Per-vertex age in [0, 1] drives the fragment-shader color mix
//     between tailColor (oldest) and headColor (newest) and a quadratic
//     alpha fade so the recent trail is bright and the distant trail
//     melts into the background.
//
// Threading:
//   - GUI thread owns the MfccAnalyzer and runs the PCA recompute on
//     mfccUpdated (Qt::DirectConnection).
//   - The 3D-projected trajectory is staged into m_stagedSegmentXyzAge
//     under m_stageMutex, then picked up by synchronize() on the render
//     thread.
//   - No allocations after construction on the hop or render path.

#pragma once

#include <QColor>
#include <QMutex>
#include <QQuickRhiItem>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <qqmlregistration.h>
#include <vector>

#include "MfccAnalyzer.h"

class AudioFeatures;
class MfccTrajectoryImpl;

class MfccTrajectory : public QQuickRhiItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(AudioFeatures* audioSource READ audioSource WRITE setAudioSource
               NOTIFY audioSourceChanged)
    Q_PROPERTY(QColor headColor READ headColor WRITE setHeadColor NOTIFY headColorChanged)
    Q_PROPERTY(QColor tailColor READ tailColor WRITE setTailColor NOTIFY tailColorChanged)
    Q_PROPERTY(float lineWidth READ lineWidth WRITE setLineWidth NOTIFY lineWidthChanged)
    Q_PROPERTY(float cameraOrbitHz READ cameraOrbitHz WRITE setCameraOrbitHz
               NOTIFY cameraOrbitHzChanged)
    Q_PROPERTY(int   filledRows READ filledRows NOTIFY trajectoryChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)

public:
    // Project coefficients 1..N_COEFFS-1 -> 12-D feature space. Coeff 0
    // is dropped at PCA-input time, not at trajectory-buffer time, so
    // the analyzer continues to publish all 13 (handy for diagnostics).
    static constexpr int FEATURE_DIM      = MfccAnalyzer::N_COEFFS - 1;  // 12
    static constexpr int TRAJECTORY_LEN   = MfccAnalyzer::RECENT_ROWS;   // 600
    static constexpr int PCA_RECOMPUTE_EVERY = 30;                       // hops

    explicit MfccTrajectory(QQuickItem* parent = nullptr);
    ~MfccTrajectory() override;

    AudioFeatures* audioSource() const { return m_source; }
    void           setAudioSource(AudioFeatures* s);

    QColor headColor() const { return m_headColor; }
    void   setHeadColor(const QColor& c);

    QColor tailColor() const { return m_tailColor; }
    void   setTailColor(const QColor& c);

    float lineWidth() const { return m_lineWidth; }
    void  setLineWidth(float v);

    float cameraOrbitHz() const { return m_cameraOrbitHz; }
    void  setCameraOrbitHz(float v);

    int   filledRows() const { return m_filledRows.load(std::memory_order_relaxed); }

    bool  active() const { return m_active.load(std::memory_order_relaxed); }
    void  setActive(bool a);

    // Force a PCA recompute on the next mfcc hop -- e.g. when a new track
    // starts and the previous song's basis would skew the geometry.
    Q_INVOKABLE void resetPCA();

    // Snapshot the current 3D trajectory + the eigenvalue diagonal. Used
    // by the verification harness; returns the number of valid rows
    // (oldest first, newest last). outXYZ buffer holds maxRows*3 floats;
    // outEigs holds 3 floats (top-3 eigenvalues). Thread-safe.
    int debugSnapshot(float* outXYZ, int maxRows, float* outEigs);

    // Per-recompute diagnostics. Updated each time recomputePCA() fires
    // (every PCA_RECOMPUTE_EVERY hops); the verification harness reads
    // these after the drive loop to report convergence + cost.
    int   debugLastSweeps()    const { return m_dbgLastSweeps; }
    long  debugLastUsec()      const { return m_dbgLastUsec;  }
    int   debugRecomputeCount() const { return m_dbgRecomputes; }
    int   debugSignFlips()     const { return m_dbgSignFlips; }

protected:
    QQuickRhiItemRenderer* createRenderer() override;

signals:
    void audioSourceChanged();
    void headColorChanged();
    void tailColorChanged();
    void lineWidthChanged();
    void cameraOrbitHzChanged();
    void trajectoryChanged();
    void activeChanged();

private slots:
    void onMfccUpdated();

private:
    friend class MfccTrajectoryImpl;

    // Build the (FEATURE_DIM x 3) PCA basis from the recent window of
    // MFCC vectors. Sets m_mean, m_pca, m_eigs. The sign-stability fix is
    // applied here: each new eigenvector is matched against the previous
    // top-3 by absolute dot product and sign-flipped if needed.
    void recomputePCA(float* sweepsOut = nullptr);
    // Append the latest 3D point to the trajectory and stage a fresh
    // snapshot for the renderer.
    void stageTrajectory();

    AudioFeatures* m_source = nullptr;
    MfccAnalyzer*  m_analyzer = nullptr;     // owned (composition)

    // PCA state.
    std::array<float, FEATURE_DIM>          m_mean   {};
    std::array<float, FEATURE_DIM * 3>      m_pca    {};   // column-major: 3 vec_n
    std::array<float, 3>                    m_eigs   {};   // top-3 eigenvalues
    bool                                    m_havePca = false;
    int                                     m_hopsSinceRecompute = 0;
    // Hops accumulated since the last resetPCA(). We need at least ~60
    // hops (~1 s) of data before the covariance is meaningful; below that
    // we just keep zero-projected points.
    int                                     m_hopsSeen = 0;

    // 3D trajectory buffer. Index 0 = oldest valid, index (filled-1) =
    // newest. Stored as packed (x,y,z) triples to keep the staging copy
    // a single memcpy.
    std::array<float, TRAJECTORY_LEN * 3>   m_trajectory {};
    int                                     m_trajWrite = 0;
    std::atomic<int>                        m_filledRows {0};

    // Scratch buffers used by recomputePCA. Reused; no allocation on the
    // hop path after construction.
    std::array<float, TRAJECTORY_LEN * FEATURE_DIM>  m_pcaScratch {};
    // 12x12 covariance + Jacobi-eigen scratch.
    std::array<float, FEATURE_DIM * FEATURE_DIM>     m_cov  {};
    std::array<float, FEATURE_DIM * FEATURE_DIM>     m_vec  {};

    // Render-thread visible. The renderer pulls a fresh copy under
    // m_stageMutex in synchronize().
    // Layout: TRAJECTORY_LEN * 4 floats = (x, y, z, age) per row.
    std::array<float, TRAJECTORY_LEN * 4>   m_stagedXyzAge {};
    int                                     m_stagedFilled = 0;
    QMutex                                  m_stageMutex;
    std::atomic<bool>                       m_stagedDirty {false};

    // Diagnostics. Only read by the verification harness; cheap to
    // maintain (one assignment per recompute).
    int  m_dbgLastSweeps    = 0;
    long m_dbgLastUsec      = 0;
    int  m_dbgRecomputes    = 0;
    int  m_dbgSignFlips     = 0;

    // Tuneables.
    QColor m_headColor      = QColor(0x00, 0xE0, 0xFF);
    QColor m_tailColor      = QColor(0x5B, 0x1E, 0x96);
    float  m_lineWidth      = 2.5f;
    float  m_cameraOrbitHz  = 1.0f / 60.0f;     // 1 rev per 60 s
    std::atomic<bool> m_active {true};
};
