// MfccAnalyzer -- live mel-frequency cepstral coefficient extractor for
// Layer 2c (3D MFCC-trajectory visualizer).
//
// Pipeline per AudioFeatures::featuresUpdated tick:
//   1. Pull the last 2048 samples (L+R) from AudioFeatures via
//      fillScopeStereo, mono-fold to a single 2048-sample frame.
//   2. Apply a Hann window, take the real FFT (kissfft).
//   3. Square magnitudes -> power spectrum.
//   4. Project through a precomputed mel filterbank (M=40 triangular
//      filters, log-mel-spaced from 80 Hz to 8 kHz).
//   5. log(max(mel, 1e-9)) per filter.
//   6. DCT type-II via a precomputed 40x13 basis matrix -> 13 cepstral
//      coefficients. Coefficient 0 (overall loudness) is discarded by
//      downstream consumers; we still produce all 13 so the caller can
//      decide.
//   7. Push the latest N_COEFFS-D vector into a 600-row circular buffer
//      (10 s of audio at the 60 Hz refresh cadence).
//
// References (formulas only, no code):
//   - Davis & Mermelstein 1980, "Comparison of parametric representations".
//   - librosa.feature.mfcc (ISC) and meyda/extractors/mfcc.ts (MIT) for
//     formula cross-checks. Clean-room: we read the formulas, not their
//     code.
//
// Threading: lives on the GUI thread, driven by the same featuresUpdated
// signal CqtAnalyzer subscribes to. fillLatestMfcc / fillRecentMfcc take
// a short output mutex so a render-thread reader can snapshot safely.

#pragma once

#include <QMutex>
#include <QObject>
#include <array>
#include <atomic>
#include <memory>
#include <qqmlregistration.h>
#include <utility>
#include <vector>

class AudioFeatures;

class MfccAnalyzer : public QObject {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(AudioFeatures* audioSource READ audioSource WRITE setAudioSource
               NOTIFY audioSourceChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)

public:
    static constexpr int N_MEL_FILTERS  = 40;
    static constexpr int N_COEFFS       = 13;
    // 10 s of audio at ~60 Hz refresh. Same window size we feed to the
    // online PCA in MfccTrajectory.
    static constexpr int RECENT_ROWS    = 600;
    static constexpr int FRAME_SAMPLES  = 2048;

    explicit MfccAnalyzer(QObject* parent = nullptr);
    ~MfccAnalyzer() override;

    AudioFeatures* audioSource() const { return m_source; }
    void           setAudioSource(AudioFeatures* s);

    bool active() const { return m_active; }
    void setActive(bool a);

    // Render-thread accessor. Writes the latest MFCC vector (N_COEFFS
    // floats; the caller is expected to ignore index 0 for trajectory
    // work). Returns false if no hop has run yet. Snapshots under a short
    // m_outMutex; uncontested in steady state.
    bool fillLatestMfcc(float* out, int n);

    // Snapshot the most recent rows from the circular buffer into a
    // contiguous (rows x N_COEFFS) row-major float matrix, oldest row
    // first, newest row last. Returns the number of rows actually filled
    // (<= maxRows and <= number of hops accumulated). Caller's out buffer
    // must hold at least maxRows * N_COEFFS floats. Allocates nothing.
    int  fillRecentMfcc(float* out, int maxRows);

    // True once the first hop has populated the buffer.
    bool haveMfcc() const { return m_haveMfcc.load(std::memory_order_acquire); }

    // Offline drive path used by the verification harness: bypass the
    // AudioFeatures source and push a mono frame directly through the
    // pipeline. The frame is assumed to be FRAME_SAMPLES long; shorter
    // frames are zero-padded, longer ones are truncated to the most
    // recent FRAME_SAMPLES samples.
    void debugProcessFrame(const float* mono, int n);

signals:
    void audioSourceChanged();
    // Emitted at the end of each successful hop. Direct-connected
    // consumers (MfccTrajectory) react synchronously on the GUI thread.
    void mfccUpdated();
    void activeChanged();

private slots:
    void onFeaturesUpdated();

private:
    void buildMelFilterbank();
    void buildDctBasis();
    void processWindow();      // m_windowed -> push one MFCC row

    AudioFeatures* m_source = nullptr;

    // FFT scratch. kissfft real-to-complex; the kissfft state itself is
    // held by a pimpl so the kissfft header doesn't leak into clients.
    struct FftImpl;
    std::unique_ptr<FftImpl> m_fft;

    int    m_fftSize    = FRAME_SAMPLES;
    double m_sampleRate = 44100.0;
    bool   m_active     = true;

    // Precomputed analysis state.
    std::vector<float> m_hann;            // FRAME_SAMPLES
    // Mel filterbank entries: filter f covers a contiguous range of
    // FFT bins from m_filterStart[f] to m_filterStart[f] + m_filterWeights[f]
    // entries. m_filterWeights[f] holds the triangular weights for those
    // bins. Storing as packed arrays (rather than vector<vector<pair>>)
    // keeps the per-hop accumulate loop cache-friendly.
    std::array<int,              N_MEL_FILTERS> m_filterStart   {};
    std::array<std::vector<float>, N_MEL_FILTERS> m_filterWeights;
    // DCT basis: m_dct[k * N_MEL_FILTERS + m] = cos(pi * k * (m + 0.5) / M).
    // Pre-multiplied by the type-II ortho normalization on row 0 so a
    // single matrix-vector product produces all 13 coefficients.
    std::array<float, N_COEFFS * N_MEL_FILTERS> m_dct {};

    // Per-hop scratch. Reused across hops; no allocation on the hop path.
    std::vector<float>      m_pullL;
    std::vector<float>      m_pullR;
    std::vector<float>      m_windowed;   // FRAME_SAMPLES
    std::vector<float>      m_power;      // m_fftSize/2 + 1
    std::array<float, N_MEL_FILTERS> m_melEnergy {};
    std::array<float, N_COEFFS>      m_coeffs {};

    // Output: latest coefficients + circular buffer of the last
    // RECENT_ROWS rows.
    QMutex                                            m_outMutex;
    std::array<float, N_COEFFS>                       m_latest {};
    std::array<float, RECENT_ROWS * N_COEFFS>         m_recent {};
    int                                               m_recentWrite = 0;
    int                                               m_recentCount = 0;
    std::atomic<bool>                                 m_haveMfcc{false};
};
