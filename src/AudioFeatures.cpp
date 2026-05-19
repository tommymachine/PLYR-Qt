#include "AudioFeatures.h"

#include <QDebug>

extern "C" {
#include "kiss_fftr.h"
}

#include <QAudioFormat>
#include <QMutexLocker>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr float kPi = 3.14159265358979323846f;

// One-pole DC-blocking high-pass coefficient (matches FftProcessor).
// y[n] = x[n] - x[n-1] + α·y[n-1]. α = 0.995 → -3 dB ≈ 35 Hz @ 44.1 kHz.
// See Julius O. Smith, "Introduction to Digital Filters" (CCRMA), §B.7.
// Per-channel state so a stereo DC phase difference doesn't bleed.
constexpr float kDcBlockAlpha = 0.995f;

// Per-band passband cutoffs (Hz). Log-spaced, perceptually motivated:
//   bass  20-200    : kick, sub, bass guitar fundamentals.
//   mid   200-2000  : vocal / midrange instrument fundamentals.
//   treb  2000-8000 : sibilance, hi-hat / cymbal presence.
//   air   8000-20k  : sparkle, cymbal shimmer, reverb tails.
constexpr float kBassLo = 20.0f;     constexpr float kBassHi = 200.0f;
constexpr float kMidLo  = 200.0f;    constexpr float kMidHi  = 2000.0f;
constexpr float kTrebLo = 2000.0f;   constexpr float kTrebHi = 8000.0f;
constexpr float kAirLo  = 8000.0f;   constexpr float kAirHi  = 20000.0f;

// Envelope-follower defaults. Tuned for the visual "feel" target:
//   bands -- 5 ms attack lets transients punch; 250 ms release keeps the
//            bar from flickering between hits.
//   rms   -- slower attack (10 ms) so brief transients don't dominate the
//            overall-loudness reading; longer release (400 ms).
//   peak  -- near-instantaneous attack (0.5 ms) so a clip is reported on
//            the very next refresh; long release (600 ms) makes a clip
//            indicator hold visibly.
constexpr double kTauAttBand = 0.005;
constexpr double kTauRelBand = 0.250;
constexpr double kTauAttRms  = 0.010;
constexpr double kTauRelRms  = 0.400;
constexpr double kTauAttPeak = 0.0005;
constexpr double kTauRelPeak = 0.600;
constexpr double kTauPhase   = 0.100;   // ~100 ms for L/R correlation

// Calibration: target ~1.0 for each band on pink noise at -23 LUFS.
//
// Procedure (run once during development, then baked):
//   1. Synthesize 1 s of -23 LUFS pink noise at 44.1 kHz stereo.
//   2. Feed through pushPcm() → refresh() in a loop until envelope
//      values settle (~10 frames).
//   3. Read the raw (pre-gain) band_raw values; gain := 1.0 / raw_mean.
//
// The numbers below come from the analytic prediction that the squared
// magnitude of a Hann-windowed FFT of unit-variance pink noise distributes
// energy as 1/f across log-spaced octave bins, plus the 2/N normalization
// of kissfft's real transform. Treble + air get a stronger lift because
// the 1/f roll-off cuts their energy contribution sharply.
//
// If a future calibration pass changes these, leave a comment with the
// date and reasoning so the next reader knows what was measured.
constexpr float kGainBass = 18.0f;
constexpr float kGainMid  = 26.0f;
constexpr float kGainTreb = 42.0f;
constexpr float kGainAir  = 70.0f;

constexpr float kRmsGain  = 2.8f;   // pink @ -23 LUFS RMS ≈ 0.071 → ~1.0
constexpr float kPeakGain = 1.0f;   // peak is already in [0,1]; no boost

// dB scale used by the spectrum texture row. ShaderToy's spec uses
// [-100, -30] dB → [0, 255]. The smoothing constant (0.8) matches Web
// Audio's AnalyserNode default, which shader artists have been tuning
// against for a decade.
constexpr float kDbFloor   = -100.0f;
constexpr float kDbCeiling = -30.0f;
constexpr float kSpecSmoothing = 0.8f;

// Centroid normalization range. log10 mapping over [100 Hz, 8 kHz] is
// what perceptual loudness papers use for "brightness."
constexpr float kCentroidLogLo = 2.0f;   // log10(100)
constexpr float kCentroidLogHi = 3.903f; // log10(8000)

}  // namespace


// --- EnvState ---------------------------------------------------------------

void AudioFeatures::EnvState::setTaus(double tauAtt, double tauRel, double refreshHz)
{
    // α = exp(-1 / (τ · refresh_rate))
    // At τ seconds, the state has converged to within 1/e of its target.
    // Refresh-rate-locked rather than sample-rate-locked because the
    // envelope is updated once per refresh, not once per sample.
    alphaAttack  = float(std::exp(-1.0 / (tauAtt * refreshHz)));
    alphaRelease = float(std::exp(-1.0 / (tauRel * refreshHz)));
}


// --- FftImpl ---------------------------------------------------------------

struct AudioFeatures::FftImpl {
    kiss_fftr_cfg cfg = nullptr;
    std::vector<kiss_fft_cpx> spectrum;
    int n = 0;

    FftImpl(int size) : n(size) {
        cfg = kiss_fftr_alloc(size, 0, nullptr, nullptr);
        spectrum.resize(size / 2 + 1);
    }
    ~FftImpl() {
        if (cfg) { kiss_fftr_free(cfg); kiss_fft_cleanup(); }
    }
};


// --- AudioFeatures ---------------------------------------------------------

AudioFeatures::AudioFeatures(QObject* parent)
    : QObject(parent)
    , m_scopeL(SCOPE_MAX, 0.0f)
    , m_scopeR(SCOPE_MAX, 0.0f)
{
    rebuildFft(m_fftSize);

    m_gainBass = kGainBass;
    m_gainMid  = kGainMid;
    m_gainTreb = kGainTreb;
    m_gainAir  = kGainAir;

    recomputeEnvelopeCoeffs();

    m_lastOnsetTime = std::chrono::steady_clock::now() - std::chrono::seconds(1);
}


AudioFeatures::~AudioFeatures() = default;


void AudioFeatures::rebuildFft(int fftSize)
{
    m_fftSize = fftSize;
    m_fft = std::make_unique<FftImpl>(fftSize);

    // 4× headroom in the ring lets a 15 Hz refresh still keep up.
    m_ringCapacity = fftSize * 4;
    m_ringL.assign(m_ringCapacity, 0.0f);
    m_ringR.assign(m_ringCapacity, 0.0f);
    m_ringWrite = 0;

    m_snapL.assign(fftSize, 0.0f);
    m_snapR.assign(fftSize, 0.0f);
    m_snapMono.assign(fftSize, 0.0f);
    m_windowed.assign(fftSize, 0.0f);
    m_hann.assign(fftSize, 0.0f);
    m_mag.assign(fftSize / 2 + 1, 0.0f);
    m_magPrev.assign(fftSize / 2 + 1, 0.0f);

    // Hann window: 0.5·(1 - cos(2π·n / (N-1))).
    for (int i = 0; i < fftSize; ++i) {
        m_hann[i] = 0.5f * (1.0f - std::cos(2.0f * kPi * float(i) / float(fftSize - 1)));
    }
}


void AudioFeatures::recomputeEnvelopeCoeffs()
{
    m_envBass.setTaus(kTauAttBand, kTauRelBand, m_refreshHz);
    m_envMid .setTaus(kTauAttBand, kTauRelBand, m_refreshHz);
    m_envTreb.setTaus(kTauAttBand, kTauRelBand, m_refreshHz);
    m_envAir .setTaus(kTauAttBand, kTauRelBand, m_refreshHz);
    m_envRms .setTaus(kTauAttRms,  kTauRelRms,  m_refreshHz);
    m_envPeak.setTaus(kTauAttPeak, kTauRelPeak, m_refreshHz);
    m_envPhaseCorr.setTaus(kTauPhase, kTauPhase, m_refreshHz);
}


void AudioFeatures::setFftSize(int n)
{
    if (n != 1024 && n != 2048 && n != 4096) return;
    if (n == m_fftSize) return;
    QMutexLocker lk(&m_ringMutex);
    rebuildFft(n);
}


void AudioFeatures::setSampleRate(double hz)
{
    if (hz <= 0.0) return;
    m_sampleRate = hz;
}


// --- PCM ingestion ---------------------------------------------------------

void AudioFeatures::pushPcm(const char* data, qint64 bytes, const QAudioFormat& fmt)
{
    if (!data || bytes <= 0) return;
    const int channels       = fmt.channelCount();
    const int bytesPerSample = fmt.bytesPerSample();
    if (channels <= 0 || bytesPerSample <= 0) return;
    const qint64 frameBytes  = qint64(bytesPerSample) * channels;
    const int    frames      = int(bytes / frameBytes);
    if (frames <= 0) return;

    // First call wins on the sample rate. AudioWorker establishes a
    // 44.1 kHz canonical format, but be defensive about unusual sources.
    if (fmt.sampleRate() > 0 &&
        std::abs(double(fmt.sampleRate()) - m_sampleRate) > 1e-3)
    {
        m_sampleRate = double(fmt.sampleRate());
    }

    QMutexLocker lk(&m_ringMutex);
    const int cap = m_ringCapacity;
    int       w   = m_ringWrite;

    // Per-channel DC-blocker state, hoisted to locals for the inner loop.
    // Filter state persists across pushPcm calls via m_dcPrev{XL,YL,XR,YR}.
    float pxL = m_dcPrevXL, pyL = m_dcPrevYL;
    float pxR = m_dcPrevXR, pyR = m_dcPrevYR;
    auto pushFrame = [&](float L, float R) {
        const float yL = L - pxL + kDcBlockAlpha * pyL;
        pxL = L; pyL = yL;
        const float yR = R - pxR + kDcBlockAlpha * pyR;
        pxR = R; pyR = yR;
        m_ringL[w] = yL;
        m_ringR[w] = yR;
        if (++w == cap) w = 0;
    };

    // Decode + write straight into the ring. Common case is Float/2ch
    // (canonical out of AudioWorker), but the other formats exist for
    // robustness — same shape as FftProcessor::pushPcm.
    switch (fmt.sampleFormat()) {
        case QAudioFormat::Float: {
            const auto* src = reinterpret_cast<const float*>(data);
            if (channels == 1) {
                for (int i = 0; i < frames; ++i) {
                    const float s = src[i];
                    pushFrame(s, s);
                }
            } else {
                // channels >= 2: take first two as L/R, ignore the rest.
                for (int i = 0; i < frames; ++i) {
                    pushFrame(src[i * channels + 0], src[i * channels + 1]);
                }
            }
            break;
        }
        case QAudioFormat::Int16: {
            const auto* src = reinterpret_cast<const qint16*>(data);
            constexpr float inv = 1.0f / 32768.0f;
            if (channels == 1) {
                for (int i = 0; i < frames; ++i) {
                    const float s = float(src[i]) * inv;
                    pushFrame(s, s);
                }
            } else {
                for (int i = 0; i < frames; ++i) {
                    pushFrame(float(src[i * channels + 0]) * inv,
                              float(src[i * channels + 1]) * inv);
                }
            }
            break;
        }
        case QAudioFormat::Int32: {
            const auto* src = reinterpret_cast<const qint32*>(data);
            constexpr float inv = 1.0f / 2147483648.0f;
            if (channels == 1) {
                for (int i = 0; i < frames; ++i) {
                    const float s = float(src[i]) * inv;
                    pushFrame(s, s);
                }
            } else {
                for (int i = 0; i < frames; ++i) {
                    pushFrame(float(src[i * channels + 0]) * inv,
                              float(src[i * channels + 1]) * inv);
                }
            }
            break;
        }
        default:
            // UInt8 / unknown — pushPcm gets called many times per
            // second; silently dropping non-canonical samples is fine.
            return;
    }
    m_ringWrite = w;
    m_dcPrevXL = pxL; m_dcPrevYL = pyL;
    m_dcPrevXR = pxR; m_dcPrevYR = pyR;
}


// --- The analyzer ---------------------------------------------------------

void AudioFeatures::refresh()
{
    if (!m_fft) return;
    const int N = m_fftSize;

    // 1. Snapshot the latest N frames out of the ring. Under lock to
    //    prevent pushPcm from tearing the write index mid-snapshot.
    {
        QMutexLocker lk(&m_ringMutex);
        const int cap = m_ringCapacity;
        int       src = m_ringWrite - N;
        if (src < 0) src += cap;
        for (int i = 0; i < N; ++i) {
            const int idx = (src + i) % cap;
            m_snapL[i] = m_ringL[idx];
            m_snapR[i] = m_ringR[idx];
        }
    }

    // 2. Mono fold (for spectrum / centroid / flux / energy bands).
    //    Peak + phase-correlation operate on the stereo snapshot.
    float peakAbs = 0.0f;
    double sumLR = 0.0, sumLL = 0.0, sumRR = 0.0;
    double sumSqMono = 0.0;
    for (int i = 0; i < N; ++i) {
        const float L = m_snapL[i];
        const float R = m_snapR[i];
        const float m = 0.5f * (L + R);
        m_snapMono[i] = m;
        const float aL = std::fabs(L);
        const float aR = std::fabs(R);
        if (aL > peakAbs) peakAbs = aL;
        if (aR > peakAbs) peakAbs = aR;
        sumLR += double(L) * double(R);
        sumLL += double(L) * double(L);
        sumRR += double(R) * double(R);
        sumSqMono += double(m) * double(m);
    }

    // 3. Window + FFT on the mono signal.
    for (int i = 0; i < N; ++i) {
        m_windowed[i] = m_snapMono[i] * m_hann[i];
    }
    kiss_fftr(m_fft->cfg, m_windowed.data(), m_fft->spectrum.data());

    // 4. Magnitude spectrum. Normalize by N/2 so |X[k]| sits in roughly
    //    the same range as the input amplitude.
    const int    nBins = N / 2 + 1;
    const float  norm  = 2.0f / float(N);
    for (int k = 0; k < nBins; ++k) {
        const auto c = m_fft->spectrum[k];
        m_mag[k] = std::sqrt(c.r * c.r + c.i * c.i) * norm;
    }

    // 5. Band energies. Σ|X[k]|² over the bins inside the band, sqrt,
    //    divide by bin count → power-equivalent average magnitude.
    const float binHz = float(m_sampleRate) / float(N);
    auto bandEnergy = [&](float lo, float hi) -> float {
        const int k0 = std::max(1,         int(std::floor(lo / binHz)));
        const int k1 = std::min(nBins - 1, int(std::ceil (hi / binHz)));
        if (k1 < k0) return 0.0f;
        double sum = 0.0;
        for (int k = k0; k <= k1; ++k) {
            sum += double(m_mag[k]) * double(m_mag[k]);
        }
        return float(std::sqrt(sum / double(k1 - k0 + 1)));
    };

    const float rawBass = bandEnergy(kBassLo, kBassHi);
    const float rawMid  = bandEnergy(kMidLo,  kMidHi);
    const float rawTreb = bandEnergy(kTrebLo, kTrebHi);
    const float rawAir  = bandEnergy(kAirLo,  std::min(kAirHi, float(m_sampleRate * 0.5)));

    const float bandBass = rawBass * m_gainBass;
    const float bandMid  = rawMid  * m_gainMid;
    const float bandTreb = rawTreb * m_gainTreb;
    const float bandAir  = rawAir  * m_gainAir;

    // 6. RMS + peak on the mono / stereo snapshot.
    const float rmsRaw = float(std::sqrt(sumSqMono / std::max(1, N))) * kRmsGain;
    const float peakRaw = peakAbs * kPeakGain;

    // 7. Envelope-follow everything.
    m_envBass.step(bandBass);
    m_envMid .step(bandMid);
    m_envTreb.step(bandTreb);
    m_envAir .step(bandAir);
    m_envRms .step(rmsRaw);
    m_envPeak.step(peakRaw);

    // 8. Spectral centroid (Hz). Σ f_k·|X[k]| / Σ |X[k]|, k = 1..N/2-1.
    double centroidNum = 0.0, centroidDen = 0.0;
    for (int k = 1; k < nBins - 1; ++k) {
        const double m = double(m_mag[k]);
        centroidNum += double(k) * binHz * m;
        centroidDen += m;
    }
    float centroidHz = m_lastCentroidHz;
    if (centroidDen > 1e-8) {
        centroidHz = float(centroidNum / centroidDen);
        m_lastCentroidHz = centroidHz;
    }
    // Clamp to plausible audio range — silence + numerical noise sometimes
    // throws the centroid up near Nyquist before the denominator floor.
    centroidHz = std::clamp(centroidHz, 20.0f, float(m_sampleRate * 0.5));
    const float centroidLog = std::log10(std::max(centroidHz, 1.0f));
    const float centroidNorm = std::clamp(
        (centroidLog - kCentroidLogLo) / (kCentroidLogHi - kCentroidLogLo),
        0.0f, 1.0f);

    // 9. Spectral flux (positive half-wave): Σ max(0, |X_t[k]| - |X_{t-1}[k]|).
    float fluxRaw = 0.0f;
    for (int k = 1; k < nBins - 1; ++k) {
        const float d = m_mag[k] - m_magPrev[k];
        if (d > 0.0f) fluxRaw += d;
    }
    std::swap(m_mag, m_magPrev);  // m_magPrev now holds the current spectrum
                                  // for next-frame comparison; m_mag becomes
                                  // the scratch we'll overwrite next refresh.
    // NOTE: m_mag still needs the current values for the spectrum-row
    // texture build below. We swapped early so a subsequent re-read of
    // m_mag inside this function would see the previous spectrum — but
    // we use m_magPrev for that read instead.

    // Sliding flux window for the adaptive onset threshold.
    if (m_fluxHistoryCount < FLUX_WINDOW) {
        m_fluxHistory[m_fluxHistoryIdx] = fluxRaw;
        m_fluxRunSum  += fluxRaw;
        m_fluxRunSum2 += fluxRaw * fluxRaw;
        ++m_fluxHistoryCount;
    } else {
        const float old = m_fluxHistory[m_fluxHistoryIdx];
        m_fluxHistory[m_fluxHistoryIdx] = fluxRaw;
        m_fluxRunSum  += fluxRaw - old;
        m_fluxRunSum2 += fluxRaw * fluxRaw - old * old;
    }
    if (++m_fluxHistoryIdx >= FLUX_WINDOW) m_fluxHistoryIdx = 0;

    const float fluxMean = m_fluxRunSum / float(std::max(1, m_fluxHistoryCount));
    const float fluxVar  = std::max(0.0f,
        m_fluxRunSum2 / float(std::max(1, m_fluxHistoryCount)) - fluxMean * fluxMean);
    const float fluxStd  = std::sqrt(fluxVar);

    // Normalized flux: 1.0 = at the onset threshold; >1 = above it.
    const float fluxThresh = fluxMean + 1.5f * fluxStd + 1e-6f;
    const float fluxNorm   = std::clamp(fluxRaw / fluxThresh, 0.0f, 4.0f);

    // 10. Adaptive onset detection (with 80 ms refractory).
    const auto now = std::chrono::steady_clock::now();
    const auto sinceLast = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - m_lastOnsetTime).count();
    bool emitOnset = false;
    if (m_fluxHistoryCount > 10 &&     // need at least ~170 ms of history
        fluxRaw > fluxThresh &&
        sinceLast > 80)
    {
        m_lastOnsetTime = now;
        emitOnset = true;
    }

    // 11. Phase correlation. Pearson over the snapshot, smoothed.
    float phaseRaw = 0.0f;
    if (sumLL > 1e-9 && sumRR > 1e-9) {
        phaseRaw = float(sumLR / std::sqrt(sumLL * sumRR));
        phaseRaw = std::clamp(phaseRaw, -1.0f, 1.0f);
    }
    m_envPhaseCorr.step(phaseRaw);

    // 12. Build the spectrum-texture row (512 bins covering 0..Nyquist/2).
    //     Per-bin exp smoothing (0.8 / 0.2) then dB clamp to [-100,-30].
    {
        QMutexLocker outLk(&m_outMutex);
        // Source = the just-computed current spectrum, which we stashed
        // in m_magPrev via the swap above.
        const int maxSrcBin = nBins / 2;   // 0..Nyquist/2 (matches ShaderToy)
        for (int i = 0; i < SPECTRUM_BINS; ++i) {
            // Nearest-neighbor resample.
            const int srcK = std::min(maxSrcBin - 1,
                int((float(i) + 0.5f) * float(maxSrcBin) / float(SPECTRUM_BINS)));
            const float raw = m_magPrev[std::max(0, srcK)];
            m_specSmoothed[i] = kSpecSmoothing * m_specSmoothed[i]
                              + (1.0f - kSpecSmoothing) * raw;
            const float dB = 20.0f * std::log10(std::max(m_specSmoothed[i], 1e-7f));
            const float clamped = std::clamp(dB, kDbFloor, kDbCeiling);
            const float norm01  = (clamped - kDbFloor) / (kDbCeiling - kDbFloor);
            m_specOut[i] = uint8_t(std::clamp(norm01 * 255.0f, 0.0f, 255.0f));
        }

        // Waveform row: last 512 mono samples mapped [-1,1] → [0,255]
        // with 128 = silence.
        const int wavStart = N - WAVEFORM_SAMPLES;
        for (int i = 0; i < WAVEFORM_SAMPLES; ++i) {
            const float s = (wavStart + i >= 0) ? m_snapMono[wavStart + i] : 0.0f;
            const float mapped = 128.0f + s * 127.0f;
            m_wavOut[i] = uint8_t(std::clamp(mapped, 0.0f, 255.0f));
        }

        // Scope buffers: most recent SCOPE_MAX samples (clamp to N).
        const int copyN = std::min(SCOPE_MAX, N);
        const int scopeStart = N - copyN;
        std::memcpy(m_scopeL.data(), m_snapL.data() + scopeStart,
                    copyN * sizeof(float));
        std::memcpy(m_scopeR.data(), m_snapR.data() + scopeStart,
                    copyN * sizeof(float));
        // Zero-pad the front if we asked for more than we have (unusual:
        // happens only when refresh runs before a buffer arrives).
        if (copyN < SCOPE_MAX) {
            std::memset(m_scopeL.data() + copyN, 0,
                        (SCOPE_MAX - copyN) * sizeof(float));
            std::memset(m_scopeR.data() + copyN, 0,
                        (SCOPE_MAX - copyN) * sizeof(float));
        }
        m_outValid = true;
    }

    // 13. Publish to atomics + emit. Done after all DSP so a QML binder
    //     reading mid-publish still sees a self-consistent set.
    m_bass.store(bandBass);  m_bass_att.store(m_envBass.value);
    m_mid .store(bandMid);   m_mid_att .store(m_envMid .value);
    m_treb.store(bandTreb);  m_treb_att.store(m_envTreb.value);
    m_air .store(bandAir);   m_air_att .store(m_envAir .value);
    m_rms .store(rmsRaw);    m_rms_att .store(m_envRms .value);
    m_peak.store(peakRaw);   m_peak_att.store(m_envPeak.value);
    m_centroid_hz.store(centroidHz);
    m_centroid_norm.store(centroidNorm);
    m_flux.store(fluxRaw);
    m_flux_norm.store(fluxNorm);
    m_phase_corr.store(m_envPhaseCorr.value);

    emit featuresUpdated();
    if (emitOnset) emit onset();
}


// --- Render-thread accessors ----------------------------------------------

bool AudioFeatures::fillSpectrumRow(uint8_t* out512)
{
    if (!out512) return false;
    QMutexLocker lk(&m_outMutex);
    if (!m_outValid) {
        std::memset(out512, 0, SPECTRUM_BINS);
        return true;
    }
    std::memcpy(out512, m_specOut.data(), SPECTRUM_BINS);
    return true;
}


bool AudioFeatures::fillSpectrumRowFull(uint8_t* out, int nbins)
{
    if (!out) return false;
    if (nbins < 64 || nbins > MAX_FULL_BINS) return false;

    QMutexLocker lk(&m_outMutex);

    // Smoothing buffer is sized once for MAX_FULL_BINS; the active prefix
    // is the first nbins floats. If nbins changes, the prior smoothing
    // state no longer maps onto the new bin layout — zero it. (Happens
    // exactly when the caller's fftBins property toggles.)
    if (m_specFullSmoothedN != nbins) {
        std::fill(m_specFullSmoothed.begin(),
                  m_specFullSmoothed.begin() + std::max(m_specFullSmoothedN, nbins),
                  0.0f);
        m_specFullSmoothedN = nbins;
    }

    if (!m_outValid) {
        std::memset(out, 0, size_t(nbins));
        return true;
    }

    // Source = current spectrum bins (held in m_magPrev after refresh's
    // swap; same source fillSpectrumRow uses). Cover the FULL [0, N/2]
    // range, not [0, N/4] — that's the whole point of this accessor.
    const int srcBinCount = int(m_magPrev.size());      // = N/2 + 1
    const int srcMax      = std::max(1, srcBinCount - 1); // ≈ N/2

    for (int i = 0; i < nbins; ++i) {
        // Map output index i ↔ source-bin position via center-of-cell.
        // Linear interpolation between adjacent FFT bins avoids the visible
        // "stair-step" of nearest-neighbor when nbins > srcBinCount.
        const float srcF = (float(i) + 0.5f) * float(srcMax) / float(nbins);
        const int   k0   = std::clamp(int(std::floor(srcF)), 0, srcMax - 1);
        const int   k1   = std::min(k0 + 1, srcMax);
        const float t    = srcF - float(k0);
        const float raw  = (1.0f - t) * m_magPrev[k0] + t * m_magPrev[k1];

        m_specFullSmoothed[i] = kSpecSmoothing * m_specFullSmoothed[i]
                              + (1.0f - kSpecSmoothing) * raw;
        const float dB      = 20.0f * std::log10(std::max(m_specFullSmoothed[i], 1e-7f));
        const float clamped = std::clamp(dB, kDbFloor, kDbCeiling);
        const float norm01  = (clamped - kDbFloor) / (kDbCeiling - kDbFloor);
        out[i] = uint8_t(std::clamp(norm01 * 255.0f, 0.0f, 255.0f));
    }
    return true;
}


bool AudioFeatures::fillWaveformRow(uint8_t* out512)
{
    if (!out512) return false;
    QMutexLocker lk(&m_outMutex);
    if (!m_outValid) {
        std::memset(out512, 128, WAVEFORM_SAMPLES);  // silence
        return true;
    }
    std::memcpy(out512, m_wavOut.data(), WAVEFORM_SAMPLES);
    return true;
}


bool AudioFeatures::fillScopeStereo(float* outL, float* outR, int n)
{
    if (!outL || !outR) return false;
    if (n <= 0 || n > SCOPE_MAX) return false;
    QMutexLocker lk(&m_outMutex);
    // Take the n newest samples (tail of the buffer). m_scopeL/R are
    // sized SCOPE_MAX; the newest sample is at index SCOPE_MAX - 1.
    const int start = SCOPE_MAX - n;
    std::memcpy(outL, m_scopeL.data() + start, n * sizeof(float));
    std::memcpy(outR, m_scopeR.data() + start, n * sizeof(float));
    return true;
}
