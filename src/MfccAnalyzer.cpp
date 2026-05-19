#include "MfccAnalyzer.h"
#include "AudioFeatures.h"

extern "C" {
#include "kiss_fftr.h"
}

#include <QMutexLocker>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr float  kPi  = 3.14159265358979323846f;
constexpr double kPiD = 3.14159265358979323846;

// Mel filterbank covers [80 Hz, 8 kHz]. Below 80 Hz the rumble/DC blob
// dominates with low musical information; above 8 kHz pop-music content
// thins out quickly. Davis & Mermelstein 1980 used 300 Hz / 8 kHz for
// speech; for music we want bass-region resolution too, hence the 80 Hz
// lower edge.
constexpr float kMelFLow  = 80.0f;
constexpr float kMelFHigh = 8000.0f;

// Slaney's mel scale form: mel(f) = 2595 * log10(1 + f/700).
inline float hzToMel(float hz) {
    return 2595.0f * std::log10(1.0f + hz / 700.0f);
}

inline float melToHz(float mel) {
    return 700.0f * (std::pow(10.0f, mel / 2595.0f) - 1.0f);
}

}  // namespace


// --- FftImpl ---------------------------------------------------------------

struct MfccAnalyzer::FftImpl {
    kiss_fftr_cfg cfg = nullptr;
    std::vector<kiss_fft_cpx> spectrum;   // m_fftSize/2 + 1
    explicit FftImpl(int n) : spectrum(size_t(n / 2 + 1)) {
        cfg = kiss_fftr_alloc(n, 0, nullptr, nullptr);
    }
    ~FftImpl() {
        if (cfg) { kiss_fftr_free(cfg); kiss_fft_cleanup(); }
    }
};


// --- Construction ----------------------------------------------------------

MfccAnalyzer::MfccAnalyzer(QObject* parent)
    : QObject(parent)
    , m_pullL(FRAME_SAMPLES, 0.0f)
    , m_pullR(FRAME_SAMPLES, 0.0f)
    , m_windowed(FRAME_SAMPLES, 0.0f)
    , m_power(size_t(FRAME_SAMPLES / 2 + 1), 0.0f)
{
    static_assert(N_MEL_FILTERS == 40,  "filterbank assumes 40 mel bands");
    static_assert(N_COEFFS      == 13,  "DCT basis is sized for 13 coefficients");

    // Hann window matches the analysis window AudioFeatures itself uses
    // for the FFT path. We're not piggybacking on its windowed buffer
    // (it isn't exposed), but using the same window keeps the spectrum
    // we extract here aligned with what the band-energy / centroid code
    // sees on the same hop.
    m_hann.resize(FRAME_SAMPLES);
    for (int i = 0; i < FRAME_SAMPLES; ++i) {
        m_hann[i] = 0.5f * (1.0f - std::cos(2.0f * kPi * float(i) /
                                            float(FRAME_SAMPLES - 1)));
    }

    m_fft = std::make_unique<FftImpl>(m_fftSize);

    buildMelFilterbank();
    buildDctBasis();
}

MfccAnalyzer::~MfccAnalyzer() = default;


// --- Audio source binding --------------------------------------------------

void MfccAnalyzer::setAudioSource(AudioFeatures* s)
{
    if (m_source == s) return;
    if (m_source) {
        disconnect(m_source, &AudioFeatures::featuresUpdated,
                   this, &MfccAnalyzer::onFeaturesUpdated);
    }
    m_source = s;
    if (m_source) {
        connect(m_source, &AudioFeatures::featuresUpdated,
                this, &MfccAnalyzer::onFeaturesUpdated,
                Qt::DirectConnection);
    }
    emit audioSourceChanged();
}


void MfccAnalyzer::setActive(bool a)
{
    if (m_active == a) return;
    m_active = a;
    emit activeChanged();
}


void MfccAnalyzer::onFeaturesUpdated()
{
    if (!m_active) return;
    if (!m_source) return;
    if (!m_source->fillScopeStereo(m_pullL.data(), m_pullR.data(), FRAME_SAMPLES))
        return;
    // Mono-fold + window in one pass to avoid an extra read of the
    // 2048-sample buffer.
    for (int i = 0; i < FRAME_SAMPLES; ++i) {
        const float mono = 0.5f * (m_pullL[i] + m_pullR[i]);
        m_windowed[i]    = mono * m_hann[i];
    }
    processWindow();
}


// --- Mel filterbank construction ------------------------------------------

void MfccAnalyzer::buildMelFilterbank()
{
    // M filters cover [kMelFLow, kMelFHigh] on a mel scale. Filter f has
    // a triangular response peaking at frequency f_center[f] and reaching
    // zero at f_center[f-1] and f_center[f+1]. Following the canonical
    // Davis-Mermelstein construction we need M+2 frequency points
    // (boundaries) -- the first is the low edge, the last is the high
    // edge, the middle M are the peaks of the M filters.
    const float melLo = hzToMel(kMelFLow);
    const float melHi = hzToMel(kMelFHigh);

    std::array<float, N_MEL_FILTERS + 2> melPoints {};
    for (int i = 0; i < int(melPoints.size()); ++i) {
        const float t = float(i) / float(N_MEL_FILTERS + 1);
        melPoints[i]  = melLo + t * (melHi - melLo);
    }

    std::array<float, N_MEL_FILTERS + 2> hzPoints {};
    for (int i = 0; i < int(hzPoints.size()); ++i) {
        hzPoints[i] = melToHz(melPoints[i]);
    }

    // FFT bin -> frequency map. Bin k corresponds to k * fs / fftSize Hz.
    const int specLen = m_fftSize / 2 + 1;
    const double binHz = m_sampleRate / double(m_fftSize);

    for (int f = 0; f < N_MEL_FILTERS; ++f) {
        const float fLow  = hzPoints[f];
        const float fPeak = hzPoints[f + 1];
        const float fHigh = hzPoints[f + 2];

        // The triangular filter spans FFT bins whose center frequency
        // falls inside [fLow, fHigh]. Find the inclusive bin range, then
        // sample the triangle at every bin center.
        const int kLo = std::max(0,                 int(std::ceil(fLow  / binHz)));
        const int kHi = std::min(specLen - 1,       int(std::floor(fHigh / binHz)));

        m_filterStart[f]   = kLo;
        auto& w = m_filterWeights[f];
        w.clear();
        w.reserve(size_t(std::max(0, kHi - kLo + 1)));
        if (kHi < kLo) continue;  // filter is degenerate at the spectrum

        for (int k = kLo; k <= kHi; ++k) {
            const float fk = float(k) * float(binHz);
            float wt = 0.0f;
            if (fk < fPeak && fPeak > fLow) {
                wt = (fk - fLow) / (fPeak - fLow);
            } else if (fk >= fPeak && fHigh > fPeak) {
                wt = (fHigh - fk) / (fHigh - fPeak);
            }
            if (wt < 0.0f) wt = 0.0f;
            w.push_back(wt);
        }
    }
}


// --- DCT basis construction -----------------------------------------------

void MfccAnalyzer::buildDctBasis()
{
    // Type-II DCT, "ortho" normalization (matches librosa default):
    //   c[k] = scale[k] * sum_{m=0..M-1} x[m] * cos(pi * k * (m + 1/2) / M)
    //   scale[0] = sqrt(1/M);   scale[k>0] = sqrt(2/M).
    //
    // The basis is stored as a flat (N_COEFFS x N_MEL_FILTERS) row-major
    // matrix so a per-hop matrix-vector multiply collapses to one tight
    // loop with stride-1 reads.
    const float invM     = 1.0f / float(N_MEL_FILTERS);
    const float scale0   = std::sqrt(invM);
    const float scaleK   = std::sqrt(2.0f * invM);

    for (int k = 0; k < N_COEFFS; ++k) {
        const float s = (k == 0) ? scale0 : scaleK;
        for (int m = 0; m < N_MEL_FILTERS; ++m) {
            m_dct[k * N_MEL_FILTERS + m] =
                s * std::cos(kPi * float(k) * (float(m) + 0.5f) * invM);
        }
    }
}


// --- Per-hop pipeline -----------------------------------------------------

void MfccAnalyzer::processWindow()
{
    // 1. FFT (the windowing was folded into onFeaturesUpdated above).
    kiss_fftr(m_fft->cfg,
              m_windowed.data(),
              m_fft->spectrum.data());

    // 2. Power spectrum |X[k]|^2.
    const int specLen = m_fftSize / 2 + 1;
    for (int k = 0; k < specLen; ++k) {
        const float re = m_fft->spectrum[k].r;
        const float im = m_fft->spectrum[k].i;
        m_power[k] = re * re + im * im;
    }

    // 3. Mel filterbank.
    for (int f = 0; f < N_MEL_FILTERS; ++f) {
        const auto& w = m_filterWeights[f];
        const int   kLo = m_filterStart[f];
        float acc = 0.0f;
        for (int j = 0; j < int(w.size()); ++j) {
            acc += w[j] * m_power[kLo + j];
        }
        m_melEnergy[f] = acc;
    }

    // 4. log(max(mel, 1e-9)). The floor keeps the log finite on absolute
    // silence; below this level the coefficients are dominated by
    // numerical noise anyway.
    for (int f = 0; f < N_MEL_FILTERS; ++f) {
        m_melEnergy[f] = std::log(std::max(m_melEnergy[f], 1e-9f));
    }

    // 5. DCT: c[k] = sum_m dct[k,m] * log_mel[m].
    for (int k = 0; k < N_COEFFS; ++k) {
        float acc = 0.0f;
        const float* row = &m_dct[k * N_MEL_FILTERS];
        for (int m = 0; m < N_MEL_FILTERS; ++m) {
            acc += row[m] * m_melEnergy[m];
        }
        m_coeffs[k] = acc;
    }

    // 6. Publish under the output mutex. Copy the latest vector + push
    // into the circular buffer.
    {
        QMutexLocker lk(&m_outMutex);
        std::memcpy(m_latest.data(), m_coeffs.data(),
                    sizeof(float) * N_COEFFS);
        std::memcpy(&m_recent[m_recentWrite * N_COEFFS],
                    m_coeffs.data(),
                    sizeof(float) * N_COEFFS);
        m_recentWrite = (m_recentWrite + 1) % RECENT_ROWS;
        if (m_recentCount < RECENT_ROWS) ++m_recentCount;
        m_haveMfcc.store(true, std::memory_order_release);
    }

    emit mfccUpdated();
}


// --- Render-thread accessors ----------------------------------------------

bool MfccAnalyzer::fillLatestMfcc(float* out, int n)
{
    if (!out || n <= 0) return false;
    if (!m_haveMfcc.load(std::memory_order_acquire)) {
        std::memset(out, 0, sizeof(float) * size_t(n));
        return false;
    }
    QMutexLocker lk(&m_outMutex);
    const int copyN = std::min(n, N_COEFFS);
    std::memcpy(out, m_latest.data(), sizeof(float) * size_t(copyN));
    if (n > copyN)
        std::memset(out + copyN, 0, sizeof(float) * size_t(n - copyN));
    return true;
}


int MfccAnalyzer::fillRecentMfcc(float* out, int maxRows)
{
    if (!out || maxRows <= 0) return 0;
    QMutexLocker lk(&m_outMutex);
    const int avail = std::min(m_recentCount, maxRows);
    if (avail <= 0) return 0;

    // The circular buffer stores rows in chronological order, oldest at
    // m_recentWrite (the next slot to overwrite) when the buffer is full.
    // Caller wants oldest-first, newest-last. So we read forward from
    // (recentWrite - count) mod RECENT_ROWS, skipping rows beyond the
    // caller's window.
    const int total = m_recentCount;          // number of valid rows
    const int firstValid = (m_recentWrite + RECENT_ROWS - total) % RECENT_ROWS;
    const int skip = total - avail;           // rows we drop (older than maxRows)
    const int src0 = (firstValid + skip) % RECENT_ROWS;

    // Two contiguous spans at most -- one from src0 to RECENT_ROWS-1, one
    // from 0 to whatever's left.
    const int firstSpan = std::min(avail, RECENT_ROWS - src0);
    std::memcpy(out,
                &m_recent[src0 * N_COEFFS],
                sizeof(float) * size_t(firstSpan * N_COEFFS));
    if (firstSpan < avail) {
        std::memcpy(out + firstSpan * N_COEFFS,
                    &m_recent[0],
                    sizeof(float) * size_t((avail - firstSpan) * N_COEFFS));
    }
    return avail;
}


// --- Offline drive path (verification harness) ----------------------------

void MfccAnalyzer::debugProcessFrame(const float* mono, int n)
{
    if (!mono || n <= 0) return;
    // Window into m_windowed: if the frame is shorter than FRAME_SAMPLES,
    // zero-pad in front; if longer, take the most recent FRAME_SAMPLES.
    const int srcStart = std::max(0, n - FRAME_SAMPLES);
    const int copyN    = std::min(n, FRAME_SAMPLES);
    const int padN     = FRAME_SAMPLES - copyN;

    for (int i = 0; i < padN; ++i) m_windowed[i] = 0.0f;
    for (int i = 0; i < copyN; ++i)
        m_windowed[padN + i] = mono[srcStart + i] * m_hann[padN + i];

    processWindow();
}
