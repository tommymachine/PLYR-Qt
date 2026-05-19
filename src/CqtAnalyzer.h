// CqtAnalyzer — efficient octave-recursive constant-Q transform engine
// powering Layer 1c (spectrogram waterfall).
//
// Implements the Schorkhuber-Klapuri algorithm (SMC 2010): the topmost
// octave's complex Morlet-style kernel is built once and FFT'd offline;
// every lower octave reuses the same kernel applied to a 2x-downsampled
// signal. This keeps total cost O(N log N) per hop instead of O(N^2)
// like the naive CQT, while giving geometrically-spaced pitch bins (one
// per equal-tempered fraction-of-a-semitone). A 24 bin/octave, 8-octave
// configuration covers ~32.7 Hz (C1) to ~8.3 kHz (just under C9) with
// quarter-tone resolution -- exactly 192 output magnitudes per hop.
//
// Threading: lives on the GUI thread. featuresUpdated() drives sample
// ingestion through pushSamples(); computeHop() runs synchronously on
// the same thread. Render-thread access goes through fillRow() under a
// short output mutex (same pattern AudioFeatures uses for its spectrum
// row).

#pragma once

#include <QMutex>
#include <QObject>
#include <array>
#include <atomic>
#include <complex>
#include <memory>
#include <qqmlregistration.h>
#include <vector>

class AudioFeatures;

class CqtAnalyzer : public QObject {
    Q_OBJECT
    QML_ELEMENT

    // When set, the analyzer auto-pulls fillScopeStereo() on every
    // featuresUpdated() and pushes the mono fold into its own ring.
    Q_PROPERTY(AudioFeatures* audioSource READ audioSource WRITE setAudioSource
               NOTIFY audioSourceChanged)

    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)

public:
    // Upper bound used to size the magnitude buffer. 24 bins/oct * 12 oct =
    // 288 would still fit; the runtime config picks the real count via
    // outputBins(). 384 leaves slack for future B=32 or n_oct=12.
    static constexpr int MAX_BINS = 384;

    // Default hop ring is 4 * fftSize so the analyzer can survive a
    // sluggish refresh tick without overwriting the unread tail.
    static constexpr int RING_MULTIPLIER = 4;

    explicit CqtAnalyzer(QObject* parent = nullptr,
                         int    binsPerOctave = 24,
                         int    nOctaves      = 8,
                         double fMin          = 32.70,    // C1
                         double sampleRate    = 44100.0,
                         int    fftSize       = 4096);
    ~CqtAnalyzer() override;

    AudioFeatures* audioSource() const { return m_source; }
    void           setAudioSource(AudioFeatures* s);

    bool active() const { return m_active; }
    void setActive(bool a);

    // Feed mono samples (caller mono-folds from stereo). Maintains the
    // top-octave ring + per-lower-octave decimated rings under a short
    // mutex so pushes from non-GUI threads stay safe -- though in
    // practice everything runs on the GUI thread today.
    Q_INVOKABLE void pushSamples(const float* data, int n);

    // Run the full octave-recursive CQT over the most recent fftSize
    // samples (at the top octave) plus the appropriately-downsampled
    // tails for the lower octaves. Returns false if we haven't yet
    // accumulated fftSize samples on every octave. On success, writes
    // magnitudes into m_lastCqt and emits hopComplete().
    Q_INVOKABLE bool computeHop();

    // Number of valid entries in fillRow / m_lastCqt: binsPerOctave * nOctaves.
    Q_INVOKABLE int outputBins() const { return m_outputBins; }

    int    binsPerOctave() const { return m_binsPerOctave; }
    int    nOctaves()      const { return m_nOctaves;      }
    double fMin()          const { return m_fMin;          }
    double sampleRate()    const { return m_sampleRate;    }
    int    fftSize()       const { return m_fftSize;       }

    // Render-thread accessor. Writes outputBins() dB-encoded magnitude
    // bytes (-80 dB -> 0, 0 dB -> 255); any tail bytes beyond outputBins
    // are zero. Safe to call concurrently with computeHop() -- protected
    // by m_outMutex, same pattern AudioFeatures uses.
    bool fillRow(uint8_t* out, int n);

    // Float-magnitude sibling. Writes outputBins() raw linear magnitudes
    // (the same values fillRow() then dB-encodes into bytes). Same lock
    // discipline. Added for Layer 2a (chromagram / Tonnetz) which needs
    // log(1 + |x|) compression against pre-dB values, not the lossy
    // byte-quantized dB representation.
    bool fillRowFloat(float* out, int n);

signals:
    void audioSourceChanged();
    void hopComplete();
    void activeChanged();

private slots:
    // Connected to AudioFeatures::featuresUpdated when audioSource is
    // set. Pulls a scope row, mono-folds, pushes to the ring, runs a
    // hop.
    void onFeaturesUpdated();

private:
    using cpx = std::complex<float>;

    // One (FFT-bin index, complex kernel value) pair. Schorkhuber-Klapuri
    // suggest thresholding the FFT'd kernels at 0.54% of their row max --
    // typical sparsity is ~3% of bins retained. Storing as a packed
    // vector keeps the per-hop inner loop cache-friendly.
    struct SparseEntry {
        int  bin;
        cpx  value;
    };

    void buildKernel();
    void buildDecimator();

    // Compute the topmost-octave CQT given the FFT spectrum of a single
    // octave's worth of audio. Writes binsPerOctave magnitudes starting
    // at outOffset in m_workCqt.
    void applyKernel(const cpx* spectrum, int specLen, int outOffset);

    // 2x decimator state. Polyphase FIR -- N taps split into two
    // phases so we only multiply-add half the taps per output sample.
    // See buildDecimator() for design notes.
    struct Decimator {
        std::vector<float> taps;          // full kernel (length 2K)
        std::vector<float> state;         // delay line (length 2K-1)
        int                writeIdx = 0;
        void reset();
    };
    // Process input -> output (output size = input size / 2). Reads from
    // the contiguous most-recent input window; emits half as many output
    // samples into the per-octave ring at writeOctRing(octave).
    void decimateInto(int octave, const float* in, int inN);

    // Per-octave windowed-input scratch + window function.
    // m_window is a Hann window of length m_fftSize, pre-computed.
    std::vector<float> m_window;

    // Per-octave audio rings. m_octRing[0] holds the original-rate audio;
    // m_octRing[i] holds the i-fold-decimated stream. Each ring is
    // m_fftSize * RING_MULTIPLIER samples; the write index is in
    // m_octRingWrite[i].
    std::vector<std::vector<float>> m_octRing;
    std::vector<int>                m_octRingWrite;
    std::vector<int>                m_octRingCount;  // running count of
                                                     // samples ever pushed
                                                     // (used to gate hops)

    // Scratch for the FFT input/output at the top octave -- reused for
    // every octave because each one runs at its own (downsampled) rate
    // but always with the same fftSize.
    std::vector<float>              m_fftInput;
    std::vector<cpx>                m_fftSpectrum;
    struct FftImpl;
    std::unique_ptr<FftImpl>        m_fft;

    // The sparse FFT'd kernel matrix. One row per top-octave bin; each
    // row is a list of (bin index in [0, fftSize/2], complex weight).
    std::vector<std::vector<SparseEntry>> m_sparseKernel;

    // Work buffer for the in-progress hop's CQT magnitudes. After the
    // hop completes, copied into m_lastCqt under m_outMutex.
    std::array<float, MAX_BINS> m_workCqt {};

    // Output: latest CQT magnitudes (linear scale) + dB-encoded bytes.
    // Both are sized MAX_BINS; only [0, m_outputBins) is meaningful.
    QMutex                       m_outMutex;
    std::array<float,   MAX_BINS> m_lastCqt {};
    std::array<uint8_t, MAX_BINS> m_lastCqtBytes {};
    std::atomic<bool>            m_haveHop {false};

    // Per-octave decimators (i-th decimator turns octave i-1's audio into
    // octave i's audio). Octave 0 has no upstream decimator; the vector
    // is sized nOctaves with index 0 unused for symmetry with m_octRing.
    std::vector<Decimator>       m_decimators;

    // Ingestion ring write mutex (audio could in principle come from a
    // non-GUI thread; keep the path safe).
    QMutex                       m_ringMutex;

    // Source. Lifetime owned by the caller.
    AudioFeatures* m_source = nullptr;

    // Configuration captured at construction.
    int    m_binsPerOctave = 24;
    int    m_nOctaves      = 8;
    double m_fMin          = 32.70;
    double m_sampleRate    = 44100.0;
    int    m_fftSize       = 4096;
    int    m_outputBins    = 192;
    bool   m_active        = true;

    // Scope pull scratch (only used when audioSource is set).
    std::array<float, 2048> m_pullL {};
    std::array<float, 2048> m_pullR {};
    std::array<float, 2048> m_pullMono {};
};
