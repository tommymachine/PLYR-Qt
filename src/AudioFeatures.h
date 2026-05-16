// AudioFeatures — perceptually-motivated DSP feature extractor that
// underpins every audio-driven visualizer in the app.
//
// Sibling to FftProcessor: same architectural pattern (QObject on the GUI
// thread, pushPcm() called from the audio thread, refresh() from a QML
// Timer at ~60 Hz). Differences:
//
//   - Stereo throughout. L and R are kept separately so the goniometer
//     and phase-correlation features have real data to work with; only
//     the band-energy / centroid / flux path collapses to mono.
//   - Four log-spaced perceptual bands (bass / mid / treb / air) with
//     two-time-constant envelope followers (separate attack + release
//     time-constants). FftProcessor's release-only smoothing isn't
//     enough for "musical" envelopes — fast attack lets transients
//     punch, slow release keeps the bars from chattering.
//   - Spectral centroid, spectral flux, onset detection, RMS, peak,
//     L/R phase correlation.
//   - Exposes a 512×2 R8 texture row pair (smoothed log-mag spectrum
//     row 0, mono waveform row 1) for shader consumption via the
//     sibling SpectrumTexture QQuickItem.
//
// Threading contract:
//   - pushPcm() is called on the audio thread (DirectConnection from
//     PcmPipe::samplesServed). Internally writes to the stereo ring
//     under a short mutex.
//   - refresh() is called on the GUI thread by a QML Timer. Reads the
//     ring under the same mutex, then does all FFT + smoothing work on
//     the GUI thread. Updates std::atomic<float> scalars and emits
//     featuresUpdated() so QML bindings re-evaluate. Onsets are emitted
//     as a separate signal.
//   - fill{Spectrum,Waveform}Row + fillScopeStereo are render-thread-
//     callable. Output buffers are double-buffered: refresh() writes
//     into a back buffer under a short output mutex, then swaps.
//
// All scalar Q_PROPERTYs are backed by std::atomic<float> so QML
// bindings reading from any thread are correct without locks.

#pragma once

#include <QMutex>
#include <QObject>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <qqmlregistration.h>
#include <vector>

class QAudioFormat;

class AudioFeatures : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Instantiated in C++ and exposed as a context property")

    // Per-band: instantaneous + envelope-followed value. _att is what
    // shaders typically want; the raw is useful for transient/onset work.
    Q_PROPERTY(float bass     READ bass     NOTIFY featuresUpdated)
    Q_PROPERTY(float bass_att READ bass_att NOTIFY featuresUpdated)
    Q_PROPERTY(float mid      READ mid      NOTIFY featuresUpdated)
    Q_PROPERTY(float mid_att  READ mid_att  NOTIFY featuresUpdated)
    Q_PROPERTY(float treb     READ treb     NOTIFY featuresUpdated)
    Q_PROPERTY(float treb_att READ treb_att NOTIFY featuresUpdated)
    Q_PROPERTY(float air      READ air      NOTIFY featuresUpdated)
    Q_PROPERTY(float air_att  READ air_att  NOTIFY featuresUpdated)

    Q_PROPERTY(float rms      READ rms      NOTIFY featuresUpdated)
    Q_PROPERTY(float rms_att  READ rms_att  NOTIFY featuresUpdated)
    Q_PROPERTY(float peak     READ peak     NOTIFY featuresUpdated)
    Q_PROPERTY(float peak_att READ peak_att NOTIFY featuresUpdated)

    Q_PROPERTY(float centroid_hz   READ centroid_hz   NOTIFY featuresUpdated)
    Q_PROPERTY(float centroid_norm READ centroid_norm NOTIFY featuresUpdated)
    Q_PROPERTY(float flux          READ flux          NOTIFY featuresUpdated)
    Q_PROPERTY(float flux_norm     READ flux_norm     NOTIFY featuresUpdated)
    Q_PROPERTY(float phase_corr    READ phase_corr    NOTIFY featuresUpdated)

public:
    // FFT size choices that fit the existing ring budget (4 × FFT_SIZE).
    static constexpr int DEFAULT_FFT_SIZE = 2048;
    static constexpr int SPECTRUM_BINS    = 512;   // matches ShaderToy spec
    static constexpr int WAVEFORM_SAMPLES = 512;   // matches ShaderToy spec
    static constexpr int SCOPE_MAX        = 2048;
    static constexpr int MAX_FULL_BINS    = 8192;  // upper bound for
                                                   // fillSpectrumRowFull's nbins

    explicit AudioFeatures(QObject* parent = nullptr);
    ~AudioFeatures() override;

    // -- QML invokables ----------------------------------------------------

    // Run one analysis pass. Called by a QML Timer; safe to invoke from
    // the GUI thread only.
    Q_INVOKABLE void refresh();

    // Allowed values: 1024, 2048, 4096. Smaller = lower freq resolution
    // but better time resolution; the default 2048 matches FftProcessor.
    Q_INVOKABLE void setFftSize(int n);

    // Override the inferred sample rate. Normally pushPcm()'s first call
    // sets it; provide this for testing / future format-change support.
    Q_INVOKABLE void setSampleRate(double hz);

    // Set the refresh rate the envelope-follower coefficients are tuned
    // against. Default 60 Hz to match the QML timer. Re-derives the
    // attack/release α constants for every band.
    Q_INVOKABLE void setRefreshHz(double hz);

    // -- Scalar reads (atomic; safe from any thread) ----------------------

    float bass()       const { return m_bass.load(std::memory_order_relaxed); }
    float bass_att()   const { return m_bass_att.load(std::memory_order_relaxed); }
    float mid()        const { return m_mid.load(std::memory_order_relaxed); }
    float mid_att()    const { return m_mid_att.load(std::memory_order_relaxed); }
    float treb()       const { return m_treb.load(std::memory_order_relaxed); }
    float treb_att()   const { return m_treb_att.load(std::memory_order_relaxed); }
    float air()        const { return m_air.load(std::memory_order_relaxed); }
    float air_att()    const { return m_air_att.load(std::memory_order_relaxed); }

    float rms()        const { return m_rms.load(std::memory_order_relaxed); }
    float rms_att()    const { return m_rms_att.load(std::memory_order_relaxed); }
    float peak()       const { return m_peak.load(std::memory_order_relaxed); }
    float peak_att()   const { return m_peak_att.load(std::memory_order_relaxed); }

    float centroid_hz()   const { return m_centroid_hz.load(std::memory_order_relaxed); }
    float centroid_norm() const { return m_centroid_norm.load(std::memory_order_relaxed); }
    float flux()          const { return m_flux.load(std::memory_order_relaxed); }
    float flux_norm()     const { return m_flux_norm.load(std::memory_order_relaxed); }
    float phase_corr()    const { return m_phase_corr.load(std::memory_order_relaxed); }

    // -- PCM ingestion (audio thread; thread-safe) ------------------------

    // Same signature as FftProcessor::pushPcm. Interprets the bytes
    // according to fmt's sample format + channel count, writes
    // interleaved L/R into the stereo ring under a short mutex.
    void pushPcm(const char* data, qint64 bytes, const QAudioFormat& fmt);

    // -- Render-thread direct accessors -----------------------------------
    //
    // These are safe to call from the render thread while refresh() runs
    // on the GUI thread. Each fill*() takes a short m_outMutex lock to
    // snapshot the latest back-buffer. The lock is held only for the
    // memcpy and is uncontended in steady state (refresh runs ~60 Hz,
    // render runs ~60 Hz, both touch the lock briefly).

    // Write the smoothed log-magnitude spectrum into 512 bytes, clamped
    // to [-100, -30] dB → [0, 255]. Same scale ShaderToy's iChannel0
    // row 0 uses, so existing shader artists' code can render unchanged.
    bool fillSpectrumRow(uint8_t* out512);

    // Same dB clamp + smoothing constant as fillSpectrumRow but covers
    // the full [0, Nyquist] range rather than [0, Nyquist/2]. nbins is
    // the output byte count (common: 512 / 1024 / 2048); returns false
    // for nbins outside [64, MAX_FULL_BINS]. Linear interpolation across
    // FFT bins for sub-bin smoothness.
    //
    // WHY a separate accessor: fillSpectrumRow matches ShaderToy's
    // 0–Nyquist/2 spec for shader-art compatibility; fillSpectrumRowFull
    // is the SPAN-style full-range output used by SpectrumView, where
    // 20 Hz–20 kHz analysis needs everything up to Nyquist. The two
    // outputs keep independent smoothing state for that reason.
    bool fillSpectrumRowFull(uint8_t* out, int nbins);

    // Write the last 512 mono waveform samples into 512 bytes, with
    // 128 = silence and full-scale = [0, 255]. Same scale as ShaderToy
    // row 1.
    bool fillWaveformRow(uint8_t* out512);

    // Write the last n raw float L/R samples for high-precision scope
    // rendering. n must be ≤ SCOPE_MAX (2048). Returns false if n is
    // out of range. outL/outR receive n samples each.
    bool fillScopeStereo(float* outL, float* outR, int n);

signals:
    // Emitted at the end of each refresh(). Drives Q_PROPERTY bindings
    // and the SpectrumTexture re-upload.
    void featuresUpdated();

    // Adaptive-threshold onset: flux crossed (running_mean + 1.5·std)
    // and the refractory period (80 ms) has elapsed.
    void onset();

private:
    // --- Engine plumbing ---------------------------------------------------

    void rebuildFft(int fftSize);
    void recomputeEnvelopeCoeffs();

    // FFT state — kissfft real-to-complex.
    struct FftImpl;
    std::unique_ptr<FftImpl> m_fft;

    int    m_fftSize    = DEFAULT_FFT_SIZE;
    double m_sampleRate = 44100.0;
    double m_refreshHz  = 60.0;

    // --- Input ring (audio thread writes, GUI reads) -----------------------
    // Layout: interleaved L,R floats, capacity = 4 × FFT_SIZE frames. The
    // factor of 4 means even with a sluggish 15 Hz refresh we still have
    // plenty of headroom before overwriting unread data.
    QMutex             m_ringMutex;
    std::vector<float> m_ringL;
    std::vector<float> m_ringR;
    int                m_ringWrite = 0;   // next-write index, in frames
    int                m_ringCapacity = 0; // in frames

    // --- Snapshot + windowed buffers (GUI thread only) ---------------------
    std::vector<float> m_snapL;
    std::vector<float> m_snapR;
    std::vector<float> m_snapMono;
    std::vector<float> m_windowed;
    std::vector<float> m_hann;
    std::vector<float> m_mag;       // |X[k]|, k = 0..N/2
    std::vector<float> m_magPrev;   // previous-frame magnitudes (for flux)

    // --- Envelope follower state ------------------------------------------
    struct EnvState {
        float value = 0.0f;
        float alphaAttack  = 0.0f;
        float alphaRelease = 0.0f;
        void setTaus(double tauAtt, double tauRel, double refreshHz);
        inline void step(float x) {
            // Two-time-constant follower. Same form Web Audio's
            // AnalyserNode uses for its smoothing, except with separate
            // attack and release coefficients so peaks "spring up" fast
            // while still decaying gracefully.
            const float a = (x > value) ? alphaAttack : alphaRelease;
            value = a * value + (1.0f - a) * x;
        }
    };

    EnvState m_envBass, m_envMid, m_envTreb, m_envAir;
    EnvState m_envRms;
    EnvState m_envPeak;
    EnvState m_envPhaseCorr;

    // Calibration constants per band — see AudioFeatures.cpp for the
    // procedure that produced them.
    float m_gainBass = 0.0f, m_gainMid = 0.0f, m_gainTreb = 0.0f, m_gainAir = 0.0f;

    // --- Spectral flux running stats --------------------------------------
    static constexpr int FLUX_WINDOW = 60;   // 1 s at 60 Hz refresh
    std::array<float, FLUX_WINDOW> m_fluxHistory {};
    int   m_fluxHistoryIdx = 0;
    int   m_fluxHistoryCount = 0;
    float m_fluxRunSum  = 0.0f;
    float m_fluxRunSum2 = 0.0f;            // sum of squares for std
    std::chrono::steady_clock::time_point m_lastOnsetTime;

    // --- Phase correlation accumulator ------------------------------------
    // Cheap running estimate over the same snapshot we use for the FFT.
    // No separate sliding window — we just re-derive every refresh and
    // smooth via m_envPhaseCorr (~100 ms τ).

    float m_lastCentroidHz = 0.0f;

    // --- Atomic outputs ----------------------------------------------------
    std::atomic<float> m_bass{0}, m_bass_att{0};
    std::atomic<float> m_mid{0},  m_mid_att{0};
    std::atomic<float> m_treb{0}, m_treb_att{0};
    std::atomic<float> m_air{0},  m_air_att{0};
    std::atomic<float> m_rms{0},  m_rms_att{0};
    std::atomic<float> m_peak{0}, m_peak_att{0};
    std::atomic<float> m_centroid_hz{0}, m_centroid_norm{0};
    std::atomic<float> m_flux{0}, m_flux_norm{0};
    std::atomic<float> m_phase_corr{0};

    // --- Render-thread visible buffers (double-buffered) ------------------
    // Filled inside refresh() under m_outMutex, then copied out by the
    // fill*() accessors. We don't strictly need two buffers — a single
    // mutex-guarded set works because refresh and render both run at
    // similar low frequency. Keep the door open for future zero-copy by
    // using staging vectors here.
    QMutex             m_outMutex;
    std::array<uint8_t, SPECTRUM_BINS> m_specOut {};
    std::array<uint8_t, WAVEFORM_SAMPLES> m_wavOut {};
    std::array<float,  SPECTRUM_BINS>  m_specSmoothed {};  // [0,1] linear

    // Smoothing state for fillSpectrumRowFull. Sized for MAX_FULL_BINS so
    // callers can switch nbins between requests without reallocating; the
    // smoothing length tracks the active nbins via m_specFullSmoothedN.
    // If the caller changes nbins between calls, the buffer is rezeroed
    // (smoothing across mismatched bin layouts has no defined meaning).
    std::array<float, MAX_FULL_BINS>   m_specFullSmoothed {};
    int                                m_specFullSmoothedN = 0;
    std::vector<float> m_scopeL;          // SCOPE_MAX samples (newest at end)
    std::vector<float> m_scopeR;
    bool m_outValid = false;
};
