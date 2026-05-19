// PersistentHomologyAnalyzer -- Layer 4a. Real-time Vietoris-Rips
// persistent homology over the sliding-window MFCC trajectory.
//
// Math reference:
//   Tralie & Perea 2017, "Sliding windows and persistence: An
//   application of topological methods to signal analysis"
//   (arXiv:1703.04127). The core idea: an audio signal that repeats
//   (chorus, riff, loop) produces a closed sliding-window trajectory
//   in MFCC feature space, and that loop shows up as a long-lived
//   1-dimensional homology class (H1 bar) in the trajectory's
//   Vietoris-Rips persistence barcode.
//
// Engine: Ripser (Ulrich Bauer, MIT) -- vendored at
// third_party/ripser/. We patched ripser.cpp to (a) fence its main()
// behind RIPSER_NO_MAIN and (b) replace its stdout-based barcode
// emission with two extern callbacks (ripser_emit_pair /
// ripser_begin_dim). The patch is documented in
// third_party/ripser/CONCERTO.patch.
//
// Pipeline:
//   1. On every mfccSource->mfccUpdated tick, increment a hop counter.
//   2. Every hopsPerCompute hops, schedule a Ripser run on a
//      QThreadPool worker:
//        a. Snapshot the most recent windowSize x 13 MFCC rows
//           (skipping coefficient 0 -- it tracks loudness, not
//           timbre, and would dominate the geometry).
//        b. Build the windowSize x windowSize Euclidean distance
//           matrix, stored in Ripser's lower-triangular flat
//           format.
//        c. Pick a filtration threshold: ratio * median pairwise
//           distance, with a hard floor so an entirely silent
//           segment (all distances ~= 0) doesn't degenerate.
//        d. Run Ripser with dim_max = dimMax (default 1: H0+H1).
//        e. Stash the resulting (birth, death, dim) triples into
//           a member vector under m_resultMutex.
//   3. Emit barcodeUpdated() on the GUI thread.
//
// Threading model:
//   * mfcc hop slot runs on the GUI thread (mfccSource is direct-
//     connected from AudioFeatures, which fires on the GUI thread).
//   * Ripser runs on a QThreadPool worker. We hold m_computeMutex
//     across the whole call: ripser.cpp uses static thread-locals
//     inside its simplex enumerators and is not reentrant; we never
//     run two Ripser calls concurrently.
//   * latestPairs() is a render-thread reader (the
//     PersistenceBarcode QQuickPaintedItem calls it from its paint).
//     Snapshot under m_resultMutex.
//   * The compute job emits barcodeUpdated() via
//     QMetaObject::invokeMethod onto the analyzer's thread, so QML
//     bindings (which read latestPairs) react on their owning thread.
//
// Performance:
//   For N=64 (the default), the typical cohomology+clearing run on a
//   2024 Apple Silicon Mac is 5-20 ms. Even at N=128 it stays under
//   100 ms, well below the 0.5 s recompute cadence at the default
//   hopsPerCompute=30. The compute thread holds m_inFlight so a
//   slow run never queues a second copy of itself.

#pragma once

// Pull MfccAnalyzer's full definition (rather than forward-declaring)
// because QPointer<MfccAnalyzer>::data() requires the complete type
// and our Q_PROPERTY meta-type registration would otherwise need a
// custom Q_DECLARE_METATYPE incantation.
#include "MfccAnalyzer.h"

#include <QObject>
#include <QMutex>
#include <QPointer>
#include <QVariant>
#include <QVariantList>
#include <QVector>
#include <atomic>
#include <qqmlregistration.h>
#include <vector>

class PersistentHomologyAnalyzer : public QObject {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(MfccAnalyzer* mfccSource READ mfccSource WRITE setMfccSource
               NOTIFY mfccSourceChanged)
    Q_PROPERTY(int  windowSize     READ windowSize     WRITE setWindowSize
               NOTIFY windowSizeChanged)
    Q_PROPERTY(int  hopsPerCompute READ hopsPerCompute WRITE setHopsPerCompute
               NOTIFY hopsPerComputeChanged)
    Q_PROPERTY(int  dimMax         READ dimMax         WRITE setDimMax
               NOTIFY dimMaxChanged)
    // Snapshot of the filtration threshold the last Ripser run used. The
    // barcode item maps x = filtration radius into screen space; reading
    // this lets the UI tune its `maxRadius` to the data adaptively
    // rather than hard-coding it.
    Q_PROPERTY(float lastThreshold READ lastThreshold NOTIFY barcodeUpdated)
    // Wall-clock cost of the last Ripser run, in microseconds. Surfaced
    // for the "compute time" diagnostic chip and the verification CLI.
    Q_PROPERTY(int   lastComputeUsec READ lastComputeUsec NOTIFY barcodeUpdated)
    // True once at least one Ripser run has completed.
    Q_PROPERTY(bool  hasData         READ hasData         NOTIFY barcodeUpdated)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)

public:
    struct PersistencePair {
        float birth = 0.0f;   // filtration radius at birth
        float death = 0.0f;   // filtration radius at death; +inf if unkillable
        int   dim   = 0;      // 0 = connected component, 1 = loop, ...
    };

    // Hard upper bound on the sliding window size. Ripser's runtime grows
    // ~O(N^(d+2)) in the worst case; N=256 is the largest we'd ever
    // realistically try at 60 Hz hop rate. Most settings use 64.
    static constexpr int MAX_WINDOW = 256;

    // The MFCC source publishes 13 coefficients per frame. We drop
    // coefficient 0 (loudness) so geometry isn't dominated by a single
    // global amplitude axis -- same trick MfccTrajectory uses for PCA.
    static constexpr int FEATURE_DIM = 12;

    explicit PersistentHomologyAnalyzer(QObject* parent = nullptr);
    ~PersistentHomologyAnalyzer() override;

    MfccAnalyzer* mfccSource() const { return m_mfcc.data(); }
    void          setMfccSource(MfccAnalyzer* s);

    int  windowSize() const { return m_windowSize; }
    void setWindowSize(int n);

    int  hopsPerCompute() const { return m_hopsPerCompute; }
    void setHopsPerCompute(int n);

    int  dimMax() const { return m_dimMax; }
    void setDimMax(int n);

    float lastThreshold() const  { return m_lastThreshold.load(std::memory_order_acquire); }
    int   lastComputeUsec() const { return m_lastComputeUsec.load(std::memory_order_acquire); }
    bool  hasData() const         { return m_hasData.load(std::memory_order_acquire); }

    bool  active() const { return m_active; }
    void  setActive(bool a);

    // QML-accessible accessor that returns the latest barcode as a
    // QVariantList of { birth, death, dim } maps. Used by the corner
    // readout in PersistenceBarcodeView.qml (counting H1 loops, etc).
    Q_INVOKABLE QVariantList latestPairs() const;

    // Native-thread snapshot for the C++ painter. Renderer-callable.
    // Copies under m_resultMutex; uncontested in steady state because
    // the writer side is on a worker thread that locks only at the end
    // of a multi-millisecond Ripser run.
    QVector<PersistencePair> snapshotPairs() const;

    // Reset internal state -- hop counter, last-pairs snapshot, and
    // the hasData flag. Call this from QML on track changes so the
    // "ANALYZING..." chip re-appears and the previous song's
    // barcode doesn't bleed into the new one's first compute.
    Q_INVOKABLE void reset();

    // Verification hook. Drives the worker pipeline synchronously
    // from a precomputed N x N distance matrix supplied as a flat,
    // row-major array. Returns the resulting pairs. Bypasses the
    // mfccSource entirely so the algorithmic correctness can be
    // tested with synthetic point clouds (circle, trefoil, etc.).
    QVector<PersistencePair>
    computeFromDistanceMatrixSync(const float* distFull, int n,
                                  float threshold, int dimMax);

    // Verification hook: convert an n x d point cloud into a distance
    // matrix and run the persistence computation synchronously. Helper
    // used by phverify_cli; takes the same threshold + dimMax knobs.
    QVector<PersistencePair>
    computeFromPointCloudSync(const float* points, int n, int d,
                              float threshold, int dimMax);

signals:
    void mfccSourceChanged();
    void windowSizeChanged();
    void hopsPerComputeChanged();
    void dimMaxChanged();
    void barcodeUpdated();
    void activeChanged();

private slots:
    void onMfccUpdated();

private:
    // Worker entry point. Runs on a QThreadPool thread. Performs the
    // snapshot, distance-matrix build, and Ripser invocation, then
    // posts the result back to the analyzer's thread.
    void runCompute(int generation);

    // Build the lower-triangular distance matrix from a row-major
    // (rows x FEATURE_DIM) MFCC snapshot. Writes rows*(rows-1)/2 floats.
    static void buildLowerTriangular(const float* mfccRows, int rows,
                                     std::vector<float>& outLower);

    // Compute a robust threshold for the Ripser filtration: the median
    // pairwise distance, scaled by 2.0, with a minimum so silent
    // segments don't degenerate. lowerSize = N*(N-1)/2.
    static float pickThreshold(const std::vector<float>& lowerDistances);

    // The C-style emit callbacks find the active instance via the
    // file-scope g_activeAnalyzer pointer in the .cpp (set/cleared
    // around the Ripser call). Ripser is mutually exclusive
    // (m_computeMutex) so the global is safe -- only one compute is
    // in flight at a time.
    friend void ripser_emit_pair(int dim, float birth, float death);
    friend void ripser_begin_dim(int dim);

    void appendPair(int dim, float birth, float death);

    QPointer<MfccAnalyzer> m_mfcc;

    // Tuneables, set from QML.
    int m_windowSize     = 64;
    int m_hopsPerCompute = 30;
    int m_dimMax         = 1;
    bool m_active        = true;

    // Hop counter since the last compute fired.
    int m_hopsSinceCompute = 0;

    // Generation number bumped each time we schedule a compute. The
    // worker checks against it to drop stale runs (e.g. if windowSize
    // changed while a compute was in flight).
    std::atomic<int> m_generation {0};

    // Result + diagnostics.
    mutable QMutex                m_resultMutex;
    QVector<PersistencePair>      m_results;
    std::atomic<float>            m_lastThreshold   {0.0f};
    std::atomic<int>              m_lastComputeUsec {0};
    std::atomic<bool>             m_hasData         {false};
    std::atomic<bool>             m_inFlight        {false};

    // Ripser is not reentrant per its source comments (the
    // simplex_coboundary_enumerator etc. use function-static state).
    // We hold this across the whole compute_barcodes() call to be safe.
    // Also serializes the "active instance" pointer used by the
    // friend emit callbacks.
    QMutex                        m_computeMutex;

    // Scratch buffer for the in-flight compute. Owned by the worker
    // thread between the lock acquire and lock release.
    std::vector<float>            m_workerMfcc;        // rows*FEATURE_DIM
    std::vector<float>            m_workerLower;       // N*(N-1)/2 floats
    QVector<PersistencePair>      m_workerResults;
};
