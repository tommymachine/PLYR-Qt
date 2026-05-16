#include "PersistentHomologyAnalyzer.h"
#include "MfccAnalyzer.h"

#include <QMutexLocker>
#include <QMetaObject>
#include <QPointer>
#include <QThreadPool>
#include <QVariantMap>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

// Ripser linkage. The vendored ripser.cpp is compiled as a single
// static library with -DRIPSER_NO_MAIN -DRIPSER_USE_OUTPUT_HOOK
// -DRIPSER_CONCERTO_TRAMPOLINE. The trampoline knob appends an
// extern-"C" entry point to the bottom of ripser.cpp:
//
//     extern "C" void phRunRipser(const float* lowerDistances,
//                                 int n, float threshold, int dimMax);
//
// so callers can drive a compute_barcodes() run without #include'ing
// the 1300-line templated source. See
// third_party/ripser/CONCERTO.patch for the full patch summary.
extern "C" void phRunRipser(const float* lowerDistances, int n,
                            float threshold, int dimMax);

namespace {

// The active analyzer instance, set for the duration of a compute call.
// Ripser is single-threaded internally and we hold m_computeMutex
// across the call, so a single atomic pointer is sufficient.
std::atomic<PersistentHomologyAnalyzer*> g_activeAnalyzer {nullptr};

// Current dim tracker -- the emit callback in ripser.cpp passes the
// dim explicitly per pair, but we also keep this for sanity logging
// when needed.
std::atomic<int> g_currentDim {0};

}  // namespace


// Free-function callbacks for ripser.cpp. ripser.cpp declares these
// extern under RIPSER_USE_OUTPUT_HOOK and calls them whenever it would
// otherwise print a persistence pair to std::cout. Definitions are at
// global scope to match the extern declarations.
void ripser_begin_dim(int dim) {
    g_currentDim.store(dim, std::memory_order_relaxed);
}

void ripser_emit_pair(int dim, float birth, float death) {
    auto* a = g_activeAnalyzer.load(std::memory_order_acquire);
    if (a) a->appendPair(dim, birth, death);
}


// --- PersistentHomologyAnalyzer -------------------------------------------

PersistentHomologyAnalyzer::PersistentHomologyAnalyzer(QObject* parent)
    : QObject(parent)
{
    m_workerMfcc.reserve(size_t(MAX_WINDOW) * FEATURE_DIM);
    m_workerLower.reserve(size_t(MAX_WINDOW) * (MAX_WINDOW - 1) / 2);
    m_workerResults.reserve(256);
    m_results.reserve(256);
}


PersistentHomologyAnalyzer::~PersistentHomologyAnalyzer()
{
    // Make sure any in-flight worker has cleared g_activeAnalyzer
    // before we vanish. The compute mutex protects this -- if a worker
    // is mid-Ripser-run it holds the mutex, so locking here blocks
    // until it finishes.
    QMutexLocker lk(&m_computeMutex);
    if (g_activeAnalyzer.load() == this) {
        g_activeAnalyzer.store(nullptr, std::memory_order_release);
    }
}


// --- Property setters -----------------------------------------------------

void PersistentHomologyAnalyzer::setMfccSource(MfccAnalyzer* s)
{
    if (m_mfcc.data() == s) return;
    if (m_mfcc) {
        disconnect(m_mfcc.data(), nullptr, this, nullptr);
    }
    m_mfcc = s;
    if (m_mfcc) {
        connect(m_mfcc.data(), &MfccAnalyzer::mfccUpdated,
                this, &PersistentHomologyAnalyzer::onMfccUpdated,
                Qt::DirectConnection);
    }
    emit mfccSourceChanged();
}


void PersistentHomologyAnalyzer::setWindowSize(int n)
{
    if (n < 4) n = 4;
    if (n > MAX_WINDOW) n = MAX_WINDOW;
    // Window size also caps at the analyzer's recent-row capacity. The
    // analyzer publishes up to RECENT_ROWS (600) rows, so we're safely
    // below that.
    if (n == m_windowSize) return;
    m_windowSize = n;
    m_generation.fetch_add(1, std::memory_order_acq_rel);
    emit windowSizeChanged();
}


void PersistentHomologyAnalyzer::setHopsPerCompute(int n)
{
    if (n < 1) n = 1;
    if (n == m_hopsPerCompute) return;
    m_hopsPerCompute = n;
    emit hopsPerComputeChanged();
}


void PersistentHomologyAnalyzer::setDimMax(int n)
{
    if (n < 0) n = 0;
    if (n > 2) n = 2;  // Ripser handles higher dims but the cost
                      // grows fast and we don't render them.
    if (n == m_dimMax) return;
    m_dimMax = n;
    m_generation.fetch_add(1, std::memory_order_acq_rel);
    emit dimMaxChanged();
}


// --- Reset ----------------------------------------------------------------

void PersistentHomologyAnalyzer::reset()
{
    // If a worker is mid-Ripser-run we wait for it to finish before
    // clearing results -- otherwise the worker could publish stale
    // pairs over our clean state right after we return.
    QMutexLocker lk(&m_computeMutex);
    {
        QMutexLocker rl(&m_resultMutex);
        m_results.clear();
    }
    m_hopsSinceCompute = 0;
    m_hasData.store(false, std::memory_order_release);
    m_lastThreshold.store(0.0f, std::memory_order_release);
    m_lastComputeUsec.store(0, std::memory_order_release);
    m_generation.fetch_add(1, std::memory_order_acq_rel);
    emit barcodeUpdated();
}


// --- Accessors ------------------------------------------------------------

QVector<PersistentHomologyAnalyzer::PersistencePair>
PersistentHomologyAnalyzer::snapshotPairs() const
{
    QMutexLocker lk(&m_resultMutex);
    return m_results;
}


QVariantList PersistentHomologyAnalyzer::latestPairs() const
{
    QMutexLocker lk(&m_resultMutex);
    QVariantList out;
    out.reserve(m_results.size());
    for (const PersistencePair& p : m_results) {
        QVariantMap m;
        m.insert("birth", p.birth);
        m.insert("death", p.death);
        m.insert("dim",   p.dim);
        out.append(m);
    }
    return out;
}


// --- Hop slot -------------------------------------------------------------

void PersistentHomologyAnalyzer::onMfccUpdated()
{
    if (!m_mfcc) return;
    if (++m_hopsSinceCompute < m_hopsPerCompute) return;

    // Don't queue a second compute if the previous is still running.
    // At hopsPerCompute=30 (0.5 s) and a typical 10-20 ms run, this
    // shouldn't happen; safety net for slow N=128+ settings or large
    // dim_max.
    if (m_inFlight.exchange(true, std::memory_order_acq_rel)) return;

    m_hopsSinceCompute = 0;
    const int gen = m_generation.fetch_add(1, std::memory_order_acq_rel) + 1;

    // QThreadPool::globalInstance() is process-wide. Ripser runs are
    // short and not parallelized internally, so going through the
    // shared pool is fine -- we never run two PH computes
    // concurrently (m_inFlight gate above).
    QPointer<PersistentHomologyAnalyzer> self(this);
    QThreadPool::globalInstance()->start([self, gen]() {
        if (!self) return;
        self->runCompute(gen);
    });
}


// --- Worker pipeline ------------------------------------------------------

void PersistentHomologyAnalyzer::buildLowerTriangular(
    const float* mfccRows, int rows, std::vector<float>& outLower)
{
    outLower.clear();
    outLower.resize(size_t(rows) * size_t(rows - 1) / 2);
    // Ripser's compressed_lower_distance_matrix expects entries in
    // row-major order: for i = 1..N-1, j = 0..i-1, layout[k++] =
    // d(i, j). Match that.
    size_t k = 0;
    for (int i = 1; i < rows; ++i) {
        const float* pi = mfccRows + size_t(i) * FEATURE_DIM;
        for (int j = 0; j < i; ++j) {
            const float* pj = mfccRows + size_t(j) * FEATURE_DIM;
            float acc = 0.0f;
            for (int d = 0; d < FEATURE_DIM; ++d) {
                const float diff = pi[d] - pj[d];
                acc += diff * diff;
            }
            outLower[k++] = std::sqrt(acc);
        }
    }
}


float PersistentHomologyAnalyzer::pickThreshold(
    const std::vector<float>& lowerDistances)
{
    if (lowerDistances.empty()) return 1.0f;
    // Median pairwise distance, scaled by 2.0. Empirical: this catches
    // every "musically meaningful" H1 feature without spending compute
    // on the long tail of nearly-everything-connected high-radius
    // simplices. Hard floor of 1e-3 keeps a silent / DC segment from
    // collapsing to threshold = 0 (which would make Ripser produce no
    // useful pairs at all).
    std::vector<float> sorted = lowerDistances;
    const size_t mid = sorted.size() / 2;
    std::nth_element(sorted.begin(), sorted.begin() + mid, sorted.end());
    const float median = sorted[mid];
    const float thresh = std::max(2.0f * median, 1e-3f);
    return thresh;
}


void PersistentHomologyAnalyzer::runCompute(int generation)
{
    // If a newer compute superseded us (window size changed mid-flight,
    // etc.), drop this one.
    if (generation != m_generation.load(std::memory_order_acquire)) {
        m_inFlight.store(false, std::memory_order_release);
        return;
    }

    QMutexLocker lk(&m_computeMutex);

    // Snapshot the recent MFCC window.
    if (!m_mfcc) {
        m_inFlight.store(false, std::memory_order_release);
        return;
    }

    const int rows = m_windowSize;
    m_workerMfcc.assign(size_t(rows) * MfccAnalyzer::N_COEFFS, 0.0f);
    const int got = m_mfcc->fillRecentMfcc(m_workerMfcc.data(), rows);
    if (got < 4) {
        // Not enough history yet (need at least ~4 points to have any
        // chance of a non-trivial H1). Skip silently.
        m_inFlight.store(false, std::memory_order_release);
        return;
    }

    // The MfccAnalyzer fills (N_COEFFS = 13) floats per row, oldest
    // first. We drop coefficient 0 -- loudness, which dominates
    // geometry -- and project into a (rows x FEATURE_DIM = 12)
    // contiguous buffer.
    std::vector<float> features(size_t(got) * FEATURE_DIM);
    for (int r = 0; r < got; ++r) {
        const float* src = &m_workerMfcc[size_t(r) * MfccAnalyzer::N_COEFFS];
        float*       dst = &features  [size_t(r) * FEATURE_DIM];
        for (int d = 0; d < FEATURE_DIM; ++d) {
            dst[d] = src[d + 1];  // skip coeff 0
        }
    }

    // Distance matrix.
    buildLowerTriangular(features.data(), got, m_workerLower);
    const float threshold = pickThreshold(m_workerLower);

    // Run Ripser. The friend free-functions append into m_workerResults
    // via appendPair() while compute_barcodes() runs.
    m_workerResults.clear();
    g_activeAnalyzer.store(this, std::memory_order_release);

    const auto t0 = std::chrono::steady_clock::now();
    phRunRipser(m_workerLower.data(), got, threshold, m_dimMax);
    const auto t1 = std::chrono::steady_clock::now();

    g_activeAnalyzer.store(nullptr, std::memory_order_release);

    const auto usec = std::chrono::duration_cast<std::chrono::microseconds>(
                          t1 - t0).count();
    m_lastComputeUsec.store(int(usec), std::memory_order_release);
    m_lastThreshold.store(threshold, std::memory_order_release);

    // Publish.
    {
        QMutexLocker rl(&m_resultMutex);
        m_results = m_workerResults;
    }
    m_hasData.store(true, std::memory_order_release);
    m_inFlight.store(false, std::memory_order_release);

    // Hop back onto the analyzer's thread to fire the signal.
    QMetaObject::invokeMethod(this, [this]() {
        emit barcodeUpdated();
    }, Qt::QueuedConnection);
}


void PersistentHomologyAnalyzer::appendPair(int dim, float birth, float death)
{
    // No mutex needed: this is called only inside compute_barcodes()
    // which itself runs under m_computeMutex. The other writer is the
    // result-publish step at the end of runCompute(), which is on the
    // same thread.
    PersistencePair p;
    p.dim   = dim;
    p.birth = birth;
    p.death = death;
    m_workerResults.append(p);
}


// --- Verification hooks ---------------------------------------------------

QVector<PersistentHomologyAnalyzer::PersistencePair>
PersistentHomologyAnalyzer::computeFromDistanceMatrixSync(
    const float* distFull, int n, float threshold, int dimMax)
{
    if (!distFull || n < 2) return {};
    QMutexLocker lk(&m_computeMutex);

    std::vector<float> lower(size_t(n) * size_t(n - 1) / 2);
    size_t k = 0;
    for (int i = 1; i < n; ++i) {
        for (int j = 0; j < i; ++j) {
            lower[k++] = distFull[size_t(i) * n + j];
        }
    }

    m_workerResults.clear();
    g_activeAnalyzer.store(this, std::memory_order_release);
    phRunRipser(lower.data(), n, threshold, dimMax);
    g_activeAnalyzer.store(nullptr, std::memory_order_release);

    return m_workerResults;
}


QVector<PersistentHomologyAnalyzer::PersistencePair>
PersistentHomologyAnalyzer::computeFromPointCloudSync(
    const float* points, int n, int d, float threshold, int dimMax)
{
    if (!points || n < 2 || d < 1) return {};

    std::vector<float> dist(size_t(n) * size_t(n), 0.0f);
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < i; ++j) {
            float acc = 0.0f;
            const float* pi = points + size_t(i) * d;
            const float* pj = points + size_t(j) * d;
            for (int k = 0; k < d; ++k) {
                const float diff = pi[k] - pj[k];
                acc += diff * diff;
            }
            const float r = std::sqrt(acc);
            dist[size_t(i) * n + j] = r;
            dist[size_t(j) * n + i] = r;
        }
    }

    return computeFromDistanceMatrixSync(dist.data(), n, threshold, dimMax);
}
