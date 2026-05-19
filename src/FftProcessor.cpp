#include "FftProcessor.h"

extern "C" {
#include "kiss_fftr.h"
}

#include <QAudioBuffer>
#include <QAudioFormat>
#include <QDebug>
#include <QMutexLocker>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

// One-pole DC-blocking high-pass coefficient. y[n] = x[n] - x[n-1] + α·y[n-1].
// α = 0.995 gives -3 dB at f_c = (sr / (2π)) · (1 - α) ≈ 35 Hz @ 44.1 kHz,
// which kills sub-25 Hz drift while preserving 30 Hz musical bass. See
// Julius O. Smith, "Introduction to Digital Filters" (CCRMA), §B.7.
constexpr float kDcBlockAlpha = 0.995f;

}  // namespace


struct FftProcessor::FftImpl {
    kiss_fftr_cfg cfg = nullptr;
    std::vector<kiss_fft_cpx> spectrum;

    FftImpl(int n) {
        cfg = kiss_fftr_alloc(n, 0, nullptr, nullptr);
        spectrum.resize(n / 2 + 1);
    }
    ~FftImpl() {
        if (cfg) { kiss_fftr_free(cfg); kiss_fft_cleanup(); }
    }
};


FftProcessor::FftProcessor(QObject* parent)
    : QObject(parent)
    , m_ring(FFT_SIZE * 2, 0.0f)
    , m_snapshot(FFT_SIZE, 0.0f)
    , m_windowed(FFT_SIZE, 0.0f)
    , m_window(FFT_SIZE, 0.0f)
    , m_fft(std::make_unique<FftImpl>(FFT_SIZE))
{
    // Hann window.
    for (int i = 0; i < FFT_SIZE; ++i) {
        m_window[i] = 0.5f * (1.0f - std::cos(2.0f * float(M_PI) * i / (FFT_SIZE - 1)));
    }
}

FftProcessor::~FftProcessor() = default;


void FftProcessor::setDisplaySlope(float dbPerOct)
{
    const float v = std::clamp(dbPerOct, 0.0f, 6.0f);
    if (std::fabs(v - m_displaySlopeDbPerOct) < 1e-4f) return;
    m_displaySlopeDbPerOct = v;
    emit displaySlopeChanged();
}


void FftProcessor::setActive(bool a)
{
    bool prev = m_active.exchange(a, std::memory_order_relaxed);
    if (prev != a) emit activeChanged();
}


void FftProcessor::pushBuffer(const QAudioBuffer& buf)
{
    const auto fmt      = buf.format();
    const int  frames   = buf.frameCount();
    const int  channels = fmt.channelCount();
    if (frames <= 0 || channels <= 0) return;


    // Fold each frame to mono, run the one-pole DC blocker, append to
    // the ring under lock. DC-blocking before the ring so the snapshot
    // → window → FFT chain downstream is unchanged; only sub-30-Hz
    // drift gets evicted.
    QMutexLocker lk(&m_ringMutex);
    const int cap = int(m_ring.size());
    int w = m_writeIndex;
    float px = m_dcPrevX;
    float py = m_dcPrevY;

    auto pushMono = [&](float s) {
        const float y = s - px + kDcBlockAlpha * py;
        px = s;
        py = y;
        m_ring[w] = y;
        if (++w == cap) w = 0;
    };

    switch (fmt.sampleFormat()) {
        case QAudioFormat::Float: {
            const auto* src = buf.constData<float>();
            for (int i = 0; i < frames; ++i) {
                float s = 0.0f;
                for (int c = 0; c < channels; ++c)
                    s += src[i * channels + c];
                pushMono(s / float(channels));
            }
            break;
        }
        case QAudioFormat::Int16: {
            const auto* src = buf.constData<qint16>();
            constexpr float inv = 1.0f / 32768.0f;
            for (int i = 0; i < frames; ++i) {
                float s = 0.0f;
                for (int c = 0; c < channels; ++c)
                    s += float(src[i * channels + c]) * inv;
                pushMono(s / float(channels));
            }
            break;
        }
        case QAudioFormat::Int32: {
            const auto* src = buf.constData<qint32>();
            constexpr float inv = 1.0f / 2147483648.0f;
            for (int i = 0; i < frames; ++i) {
                float s = 0.0f;
                for (int c = 0; c < channels; ++c)
                    s += float(src[i * channels + c]) * inv;
                pushMono(s / float(channels));
            }
            break;
        }
        default:
            break;
    }
    m_writeIndex = w;
    m_dcPrevX = px;
    m_dcPrevY = py;
}


void FftProcessor::pushPcm(const char* data, qint64 bytes, const QAudioFormat& fmt)
{
    if (!m_active.load(std::memory_order_relaxed)) return;
    if (!data || bytes <= 0) return;
    const int channels      = fmt.channelCount();
    const int bytesPerSample = fmt.bytesPerSample();
    if (channels <= 0 || bytesPerSample <= 0) return;

    const int frames = int(bytes / (qint64(channels) * bytesPerSample));
    if (frames <= 0) return;

    // First call wins on the sample rate; subsequent format changes also
    // take effect. AudioWorker establishes a 44.1 kHz canonical format,
    // but be defensive about unusual sources (mirrors AudioFeatures).
    // Bin-to-band mapping recomputes inline in fillBandsAndPeaks, so
    // changing m_sampleRate is enough — no scaffold to rebuild here.
    if (fmt.sampleRate() > 0 &&
        std::abs(double(fmt.sampleRate()) - m_sampleRate) > 1e-3)
    {
        m_sampleRate = double(fmt.sampleRate());
    }

    QMutexLocker lk(&m_ringMutex);
    const int cap = int(m_ring.size());
    int w = m_writeIndex;
    float px = m_dcPrevX;
    float py = m_dcPrevY;

    // Fold-to-mono then DC-block, same as pushBuffer. The filter state
    // (px, py) is shared between push paths via m_dcPrev{X,Y} so swapping
    // between the QAudioBuffer and PCM-pipe sources is seamless.
    auto pushMono = [&](float s) {
        const float y = s - px + kDcBlockAlpha * py;
        px = s;
        py = y;
        m_ring[w] = y;
        if (++w == cap) w = 0;
    };

    switch (fmt.sampleFormat()) {
        case QAudioFormat::Float: {
            const auto* src = reinterpret_cast<const float*>(data);
            for (int i = 0; i < frames; ++i) {
                float s = 0.0f;
                for (int c = 0; c < channels; ++c)
                    s += src[i * channels + c];
                pushMono(s / float(channels));
            }
            break;
        }
        case QAudioFormat::Int16: {
            const auto* src = reinterpret_cast<const qint16*>(data);
            constexpr float inv = 1.0f / 32768.0f;
            for (int i = 0; i < frames; ++i) {
                float s = 0.0f;
                for (int c = 0; c < channels; ++c)
                    s += float(src[i * channels + c]) * inv;
                pushMono(s / float(channels));
            }
            break;
        }
        case QAudioFormat::Int32: {
            const auto* src = reinterpret_cast<const qint32*>(data);
            constexpr float inv = 1.0f / 2147483648.0f;
            for (int i = 0; i < frames; ++i) {
                float s = 0.0f;
                for (int c = 0; c < channels; ++c)
                    s += float(src[i * channels + c]) * inv;
                pushMono(s / float(channels));
            }
            break;
        }
        default:
            break;
    }
    m_writeIndex = w;
    m_dcPrevX = px;
    m_dcPrevY = py;
}


bool FftProcessor::fillBandsAndPeaks(float* out)
{
    if (!out) return false;

    // Snapshot the latest FFT_SIZE samples, linearized.
    {
        QMutexLocker lk(&m_ringMutex);
        const int cap = int(m_ring.size());
        int src = m_writeIndex - FFT_SIZE;
        if (src < 0) src += cap;
        for (int i = 0; i < FFT_SIZE; ++i) {
            m_snapshot[i] = m_ring[(src + i) % cap];
        }
    }

    // Apply Hann window.
    for (int i = 0; i < FFT_SIZE; ++i)
        m_windowed[i] = m_snapshot[i] * m_window[i];

    // Real FFT.
    kiss_fftr(m_fft->cfg, m_windowed.data(), m_fft->spectrum.data());

    // Group bins into 16 log-spaced bands (30Hz → 16kHz), dB-scale.
    // Sample rate is adapted from the source in pushPcm.
    const float     sampleRate = float(m_sampleRate);
    constexpr float minFreq    = 30.0f;
    constexpr float maxFreq    = 16000.0f;
    const float     binHz      = sampleRate / float(FFT_SIZE);
    const int       binCount   = int(m_fft->spectrum.size());
    const float     norm       = 1.0f / float(FFT_SIZE / 2);

    // SPAN-style display tilt — multiply each band's magnitude by
    //   10^( slope · log2(fc / 1 kHz) / 20 )
    // before the dB conversion. Reference 1 kHz; +3 dB/oct compensates
    // for the ~pink spectrum of typical music so the bars sit roughly
    // flat. See Voxengo SPAN's "slope" parameter for the standard
    // implementation. Peak-hold values track m_bands[i] downstream, so
    // tilting newBands[i] propagates to peaks automatically.
    constexpr float kFRef = 1000.0f;
    const float slopeDb = m_displaySlopeDbPerOct;

    std::array<float, BAND_COUNT> newBands {};
    for (int i = 0; i < BAND_COUNT; ++i) {
        const float f0 = minFreq * std::pow(maxFreq / minFreq, float(i)     / float(BAND_COUNT));
        const float f1 = minFreq * std::pow(maxFreq / minFreq, float(i + 1) / float(BAND_COUNT));
        int b0 = std::max(1,              int(f0 / binHz));
        int b1 = std::min(binCount - 1,  std::max(b0, int(f1 / binHz)));
        float sum = 0;
        for (int b = b0; b <= b1; ++b) {
            const auto c = m_fft->spectrum[b];
            sum += std::sqrt(c.r * c.r + c.i * c.i);
        }
        float mag = (sum / float(b1 - b0 + 1)) * norm;

        // Per-band tilt at the geometric-mean center frequency.
        const float fc   = std::sqrt(f0 * f1);
        const float tilt = std::pow(10.0f,
            slopeDb * std::log2(fc / kFRef) / 20.0f);
        mag *= tilt;

        const float db  = 20.0f * std::log10(std::max(mag, 1e-7f));
        newBands[i] = std::clamp((db + 80.0f) / 80.0f, 0.0f, 1.0f);
    }

    // Per-band attack + release smoothing. Low bands have inherently
    // higher per-frame variance because they aggregate only 1–2 FFT
    // bins each (band 0 ≈ 30 Hz at 21 Hz bin width = 1 bin), while
    // high bands aggregate 100+ bins (band 15 ≈ 240 bins). Same
    // spatial averaging would mean low bars twitch while high bars
    // sit calmly. Compensate with extra temporal averaging on the
    // low end:
    //
    //   attack:  0.30 (band 0, smoothed)  →  1.00 (band 15, instant)
    //   release: 0.08 (band 0, slow)      →  0.35 (band 15, snappy)
    //
    // attack=1.0 reproduces the old "instant attack" behavior. The
    // release values are LERP coefficients applied per refresh tick;
    // smaller = slower decay.
    constexpr int   peakHoldFrames  = 36;   // ~0.6s at 60fps
    constexpr float peakFallPerFrame = 0.010f;

    for (int i = 0; i < BAND_COUNT; ++i) {
        const float t       = float(i) / float(BAND_COUNT - 1);
        const float attack  = 0.30f + (1.00f - 0.30f) * t;
        const float release = 0.08f + (0.35f - 0.08f) * t;

        if (newBands[i] >= m_bands[i]) m_bands[i] += (newBands[i] - m_bands[i]) * attack;
        else                           m_bands[i] += (newBands[i] - m_bands[i]) * release;

        if (m_bands[i] >= m_peaks[i]) {
            m_peaks[i] = m_bands[i];
            m_peakHoldFrames[i] = peakHoldFrames;
        } else if (m_peakHoldFrames[i] > 0) {
            m_peakHoldFrames[i]--;
        } else {
            m_peaks[i] = std::max(0.0f, m_peaks[i] - peakFallPerFrame);
        }

        out[i]              = m_bands[i];
        out[BAND_COUNT + i] = m_peaks[i];
    }

    // --- EQ-panel tap: 10 ISO octave bands (same dB normalization) -------
    // Each band integrates a half-octave window around its ISO center so
    // the bar lines up with what the corresponding EQ slider is shaping.
    constexpr float eqIsoFreqs[EQ_BAND_COUNT] = {
        31.5f, 63.0f, 125.0f, 250.0f, 500.0f,
        1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f
    };
    const float halfOctave = std::sqrt(2.0f);

    for (int i = 0; i < EQ_BAND_COUNT; ++i) {
        const float fc = eqIsoFreqs[i];
        const float f0 = fc / halfOctave;
        const float f1 = fc * halfOctave;
        int b0 = std::max(1, int(f0 / binHz));
        int b1 = std::min(binCount - 1, std::max(b0, int(f1 / binHz)));
        float sum = 0.0f;
        for (int b = b0; b <= b1; ++b) {
            const auto c = m_fft->spectrum[b];
            sum += std::sqrt(c.r * c.r + c.i * c.i);
        }
        const float mag   = (sum / float(b1 - b0 + 1)) * norm;
        const float db    = 20.0f * std::log10(std::max(mag, 1e-7f));
        const float level = std::clamp((db + 80.0f) / 80.0f, 0.0f, 1.0f);

        // EQ-panel bars keep the old uniform release. The 10 ISO bands
        // are half-octave-wide each — even the lowest (31.5 Hz with
        // ~22 Hz bandwidth) gets at least one full FFT bin, so per-band
        // variance is uniform enough that one release works for all.
        constexpr float eqRelease = 0.25f;
        if (level >= m_eqBands[i]) m_eqBands[i] = level;
        else                       m_eqBands[i] += (level - m_eqBands[i]) * eqRelease;

        if (m_eqBands[i] >= m_eqPeaks[i]) {
            m_eqPeaks[i] = m_eqBands[i];
            m_eqPeakHoldFrames[i] = peakHoldFrames;
        } else if (m_eqPeakHoldFrames[i] > 0) {
            m_eqPeakHoldFrames[i]--;
        } else {
            m_eqPeaks[i] = std::max(0.0f, m_eqPeaks[i] - peakFallPerFrame);
        }
    }

    emit updated();
    return true;
}

QVariantList FftProcessor::eqBandsList() const {
    QVariantList out;
    out.reserve(EQ_BAND_COUNT);
    for (int i = 0; i < EQ_BAND_COUNT; ++i) out.push_back(m_eqBands[i]);
    return out;
}

QVariantList FftProcessor::eqPeaksList() const {
    QVariantList out;
    out.reserve(EQ_BAND_COUNT);
    for (int i = 0; i < EQ_BAND_COUNT; ++i) out.push_back(m_eqPeaks[i]);
    return out;
}


QVariantList FftProcessor::bandsAndPeaks()
{
    float tmp[2 * BAND_COUNT];
    fillBandsAndPeaks(tmp);
    QVariantList out;
    out.reserve(2 * BAND_COUNT);
    for (int i = 0; i < 2 * BAND_COUNT; ++i) out.push_back(tmp[i]);
    return out;
}
