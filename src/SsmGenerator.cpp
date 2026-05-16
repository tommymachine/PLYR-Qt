#include "SsmGenerator.h"
#include "FlacDecode.h"

extern "C" {
#include "kiss_fftr.h"
}

#include <QByteArray>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>

namespace {

constexpr float  kPi  = 3.14159265358979323846f;
constexpr char   kMagic[4] = { 'S', 'S', 'M', '1' };
constexpr uint32_t kHeaderBytes = 32;

// --- Slaney mel scale -----------------------------------------------------

inline float hzToMel(float hz) {
    return 2595.0f * std::log10(1.0f + hz / 700.0f);
}
inline float melToHz(float mel) {
    return 700.0f * (std::pow(10.0f, mel / 2595.0f) - 1.0f);
}


// --- Mfcc-by-window helper ------------------------------------------------
//
// Reusable across the per-hop loop. Encapsulates: precomputed Hann window,
// mel filterbank (kFrameSamples / 2 + 1 power-spec bins mapped to 40 mel
// energies), DCT basis (13 x 40). Builds once, then process() is called
// once per frame. Allocates nothing inside process().

class MfccPipeline {
public:
    MfccPipeline(int frameSamples, int nMel, int nCoeffs,
                 double sampleRate, float melFLow, float melFHigh)
        : m_frameSamples(frameSamples)
        , m_nMel(nMel)
        , m_nCoeffs(nCoeffs)
        , m_sampleRate(sampleRate)
        , m_hann(size_t(frameSamples), 0.0f)
        , m_windowed(size_t(frameSamples), 0.0f)
        , m_power(size_t(frameSamples / 2 + 1), 0.0f)
        , m_melEnergy(size_t(nMel), 0.0f)
        , m_filterStart(size_t(nMel), 0)
        , m_filterWeights(size_t(nMel))
        , m_dct(size_t(nCoeffs) * size_t(nMel), 0.0f)
    {
        for (int i = 0; i < frameSamples; ++i) {
            m_hann[i] = 0.5f * (1.0f - std::cos(2.0f * kPi * float(i) /
                                                float(frameSamples - 1)));
        }

        m_fft = kiss_fftr_alloc(frameSamples, 0, nullptr, nullptr);
        m_spectrum.resize(size_t(frameSamples / 2 + 1));

        buildMelFilterbank(melFLow, melFHigh);
        buildDctBasis();
    }

    ~MfccPipeline() {
        if (m_fft) {
            kiss_fftr_free(m_fft);
            kiss_fft_cleanup();
        }
    }

    MfccPipeline(const MfccPipeline&) = delete;
    MfccPipeline& operator=(const MfccPipeline&) = delete;

    // Process one frame's worth of mono samples. The frame is windowed,
    // FFT'd, power-summed through the mel filterbank, log-ed, and DCT'd
    // into outCoeffs (m_nCoeffs floats). If `n` is less than the configured
    // frame size, the frame is zero-padded on the left (matches the
    // analyzer's pre-roll behaviour during the first few hops).
    void process(const float* mono, int n, float* outCoeffs) {
        // Window into m_windowed: zero-pad on the left for short tails.
        const int srcStart = std::max(0, n - m_frameSamples);
        const int copyN    = std::min(n, m_frameSamples);
        const int padN     = m_frameSamples - copyN;

        for (int i = 0; i < padN; ++i) m_windowed[i] = 0.0f;
        for (int i = 0; i < copyN; ++i)
            m_windowed[padN + i] = mono[srcStart + i] * m_hann[padN + i];

        // FFT.
        kiss_fftr(m_fft, m_windowed.data(), m_spectrum.data());

        // Power spectrum.
        const int specLen = m_frameSamples / 2 + 1;
        for (int k = 0; k < specLen; ++k) {
            const float re = m_spectrum[k].r;
            const float im = m_spectrum[k].i;
            m_power[k] = re * re + im * im;
        }

        // Mel filterbank.
        for (int f = 0; f < m_nMel; ++f) {
            const auto& w   = m_filterWeights[f];
            const int   kLo = m_filterStart[f];
            float acc = 0.0f;
            for (int j = 0; j < int(w.size()); ++j) {
                acc += w[j] * m_power[kLo + j];
            }
            m_melEnergy[f] = std::log(std::max(acc, 1e-9f));
        }

        // DCT.
        for (int k = 0; k < m_nCoeffs; ++k) {
            float acc = 0.0f;
            const float* row = &m_dct[size_t(k) * size_t(m_nMel)];
            for (int m = 0; m < m_nMel; ++m) {
                acc += row[m] * m_melEnergy[m];
            }
            outCoeffs[k] = acc;
        }
    }

private:
    void buildMelFilterbank(float melFLow, float melFHigh) {
        const float melLo = hzToMel(melFLow);
        const float melHi = hzToMel(melFHigh);

        std::vector<float> melPoints(size_t(m_nMel + 2));
        for (int i = 0; i < int(melPoints.size()); ++i) {
            const float t = float(i) / float(m_nMel + 1);
            melPoints[i]  = melLo + t * (melHi - melLo);
        }
        std::vector<float> hzPoints(size_t(m_nMel + 2));
        for (int i = 0; i < int(hzPoints.size()); ++i)
            hzPoints[i] = melToHz(melPoints[i]);

        const int specLen = m_frameSamples / 2 + 1;
        const double binHz = m_sampleRate / double(m_frameSamples);

        for (int f = 0; f < m_nMel; ++f) {
            const float fLow  = hzPoints[f];
            const float fPeak = hzPoints[f + 1];
            const float fHigh = hzPoints[f + 2];

            const int kLo = std::max(0,             int(std::ceil (fLow  / binHz)));
            const int kHi = std::min(specLen - 1,   int(std::floor(fHigh / binHz)));

            m_filterStart[f] = kLo;
            auto& w = m_filterWeights[f];
            w.clear();
            if (kHi < kLo) continue;
            w.reserve(size_t(kHi - kLo + 1));
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

    void buildDctBasis() {
        const float invM   = 1.0f / float(m_nMel);
        const float scale0 = std::sqrt(invM);
        const float scaleK = std::sqrt(2.0f * invM);

        for (int k = 0; k < m_nCoeffs; ++k) {
            const float s = (k == 0) ? scale0 : scaleK;
            for (int m = 0; m < m_nMel; ++m) {
                m_dct[size_t(k) * size_t(m_nMel) + size_t(m)] =
                    s * std::cos(kPi * float(k) * (float(m) + 0.5f) * invM);
            }
        }
    }

    int    m_frameSamples = 0;
    int    m_nMel         = 0;
    int    m_nCoeffs      = 0;
    double m_sampleRate   = 0.0;

    std::vector<float> m_hann;
    std::vector<float> m_windowed;
    std::vector<float> m_power;
    std::vector<float> m_melEnergy;
    std::vector<int>   m_filterStart;
    std::vector<std::vector<float>> m_filterWeights;
    std::vector<float> m_dct;

    kiss_fftr_cfg                m_fft = nullptr;
    std::vector<kiss_fft_cpx>    m_spectrum;
};


// --- Hop selection --------------------------------------------------------
//
// Returns {hopSec, hopSamples, T} given total samples and sample rate.
// Decimates the hop to 2 s for tracks past kLongHopThreshSec, and clamps
// T to MAX_T by stretching the hop further if necessary. The last frame
// is the last hop whose end fits inside the audio (we drop trailing audio
// shorter than one hop -- the SSM doesn't need the bookends).
struct HopChoice {
    float hopSec     = 0.0f;
    int   hopSamples = 0;
    int   T          = 0;
};

HopChoice chooseHop(uint64_t totalFrames, double sampleRate) {
    HopChoice out;
    const double durSec = double(totalFrames) / sampleRate;
    const float baseHop = (durSec > SsmGenerator::kLongHopThreshSec)
                          ? SsmGenerator::kLongHopSec
                          : SsmGenerator::kShortHopSec;

    int hopSamples = std::max(1, int(std::round(double(baseHop) * sampleRate)));
    int T = int(totalFrames / uint64_t(hopSamples));
    if (T < 1) T = 1;

    // Cap at MAX_T by stretching the hop. We round up the divisor so the
    // resulting T sits at or below the cap.
    if (T > SsmGenerator::MAX_T) {
        const int factor = (T + SsmGenerator::MAX_T - 1) / SsmGenerator::MAX_T;
        hopSamples *= factor;
        T = int(totalFrames / uint64_t(hopSamples));
        if (T < 1) T = 1;
    }

    out.hopSec     = float(double(hopSamples) / sampleRate);
    out.hopSamples = hopSamples;
    out.T          = T;
    return out;
}


// --- Quantisation ---------------------------------------------------------
//
// Cosine similarity in [-1, +1] -> [0, 255] via half-range remap. We
// could clip to [0, 1] (negative correlations driven into 0), but the
// off-diagonal stripes for inverted-spectrum repeats (rare, but real)
// would be lost. The half-range remap preserves them as dark cells.
inline uint8_t quantCosSim(float cos) {
    const float t = 0.5f + 0.5f * cos;
    const float clipped = std::clamp(t, 0.0f, 1.0f);
    return uint8_t(std::round(clipped * 255.0f));
}


// --- Feature normalisation ------------------------------------------------
//
// Pre-normalise each frame's feature vector (dropping coefficient 0) so
// the per-cell inner product IS the cosine similarity, no per-row norm
// lookup needed. Frames with zero norm (pure silence) get a zero vector
// -- their similarity to anything else is 0 -> quantised to 128 (neutral).
//
// We project onto MFCC[1..12] (loudness coefficient dropped). The output
// is row-major (frame, feature) with FEATURE_DIM features per frame.
void normaliseFeatures(std::vector<float>& features /* size T*FEATURE_DIM */,
                       int T, int featureDim) {
    for (int i = 0; i < T; ++i) {
        float* row = &features[size_t(i) * size_t(featureDim)];
        double sq = 0.0;
        for (int k = 0; k < featureDim; ++k) sq += double(row[k]) * double(row[k]);
        const float n = float(std::sqrt(sq));
        if (n > 1e-9f) {
            const float inv = 1.0f / n;
            for (int k = 0; k < featureDim; ++k) row[k] *= inv;
        } else {
            for (int k = 0; k < featureDim; ++k) row[k] = 0.0f;
        }
    }
}


// --- Cosine SSM compute ---------------------------------------------------
//
// In-place: produces a T x T uint8 matrix from a normalised T x featureDim
// feature stack. Exploits symmetry (S[i,j] == S[j,i]) so we only compute
// the upper triangle and mirror. The diagonal is 255 by definition.
void computeSsm(const std::vector<float>& features,
                int T, int featureDim,
                std::vector<uint8_t>& matrix) {
    matrix.assign(size_t(T) * size_t(T), 0);
    for (int i = 0; i < T; ++i) {
        matrix[size_t(i) * size_t(T) + size_t(i)] = 255;
        const float* a = &features[size_t(i) * size_t(featureDim)];
        for (int j = i + 1; j < T; ++j) {
            const float* b = &features[size_t(j) * size_t(featureDim)];
            float dot = 0.0f;
            for (int k = 0; k < featureDim; ++k) dot += a[k] * b[k];
            const uint8_t q = quantCosSim(dot);
            matrix[size_t(i) * size_t(T) + size_t(j)] = q;
            matrix[size_t(j) * size_t(T) + size_t(i)] = q;
        }
    }
}


// --- Mono fold from interleaved 16-bit -----------------------------------

void monoFold(const std::vector<int16_t>& pcm, std::vector<float>& mono) {
    const size_t frames = pcm.size() / 2;
    mono.resize(frames);
    const float scale = 1.0f / 32768.0f;
    for (size_t i = 0; i < frames; ++i) {
        mono[i] = scale * 0.5f * (float(pcm[i * 2]) + float(pcm[i * 2 + 1]));
    }
}

} // namespace


// --- Public API -----------------------------------------------------------

SsmGenerator::Stats SsmGenerator::generateForFile(const QString& flacPath,
                                                  const QString& sidecarPath)
{
    Stats st;
    const QString outPath = sidecarPath.isEmpty()
                          ? (flacPath + QStringLiteral(".ssm"))
                          : sidecarPath;

    auto t0 = std::chrono::steady_clock::now();
    auto decoded = flacdecode::decodeFile(flacPath.toStdString());
    auto t1 = std::chrono::steady_clock::now();
    st.decodeSec = std::chrono::duration<double>(t1 - t0).count();
    if (!decoded) {
        st.error = QStringLiteral(
            "FLAC decode failed (file missing, unreadable, or not 16-bit "
            "stereo 44.1 kHz). %1").arg(flacPath);
        return st;
    }

    std::vector<float> mono;
    monoFold(decoded->pcm, mono);
    decoded->pcm.clear();
    decoded->pcm.shrink_to_fit();

    std::vector<uint8_t> matrix;
    Stats inner = generateFromMonoSamples(mono, double(decoded->info.sampleRate),
                                          matrix);
    st.T            = inner.T;
    st.hopSec       = inner.hopSec;
    st.mfccSec      = inner.mfccSec;
    st.similaritySec = inner.similaritySec;
    if (!inner.success) {
        st.error = inner.error;
        return st;
    }

    auto tWrite0 = std::chrono::steady_clock::now();
    const bool wrote = writeSidecar(outPath, st.T, st.hopSec, matrix.data());
    auto tWrite1 = std::chrono::steady_clock::now();
    st.writeSec = std::chrono::duration<double>(tWrite1 - tWrite0).count();
    if (!wrote) {
        st.error = QStringLiteral("could not write sidecar at %1").arg(outPath);
        return st;
    }
    st.success = true;
    return st;
}


SsmGenerator::Stats SsmGenerator::generateFromMonoSamples(
    const std::vector<float>& mono, double sampleRate,
    std::vector<uint8_t>& matrixOut)
{
    Stats st;
    if (mono.empty() || sampleRate <= 0.0) {
        st.error = QStringLiteral("empty input or invalid sample rate");
        return st;
    }

    const HopChoice hop = chooseHop(uint64_t(mono.size()), sampleRate);
    if (hop.T < 2) {
        st.error = QStringLiteral("track too short for SSM (T=%1)").arg(hop.T);
        return st;
    }
    st.T      = hop.T;
    st.hopSec = hop.hopSec;

    // Extract T MFCC vectors. We feed a sliding window of kFrameSamples
    // samples centered on the hop's MID-POINT, which matches what
    // librosa.feature.mfcc does with center=True. The choice keeps the
    // first frame from being mostly zero padding -- the analysis window
    // straddles t=0 so the early frames see the start of the signal.
    //
    // Layout of features: row-major (frame, coefficient), kNumCoeffs per row.
    // We drop coefficient 0 later for the cosine compute.
    std::vector<float> coeffsRaw(size_t(hop.T) * size_t(kNumCoeffs), 0.0f);

    MfccPipeline pipe(kFrameSamples, kNumMelFilters, kNumCoeffs,
                      sampleRate, kMelFLow, kMelFHigh);

    auto tMfcc0 = std::chrono::steady_clock::now();
    std::vector<float> windowScratch(size_t(kFrameSamples), 0.0f);
    const int half = kFrameSamples / 2;

    for (int t = 0; t < hop.T; ++t) {
        // Window center = (t + 0.5) * hopSamples. Window covers
        // [center - half, center + half).
        const int64_t center = int64_t(t) * int64_t(hop.hopSamples)
                             + int64_t(hop.hopSamples) / 2;
        const int64_t lo = center - int64_t(half);
        const int64_t hi = lo + int64_t(kFrameSamples);

        // Source range, clipped to the signal.
        const int64_t srcLo = std::max<int64_t>(0, lo);
        const int64_t srcHi = std::min<int64_t>(int64_t(mono.size()), hi);
        const int64_t copyN = std::max<int64_t>(0, srcHi - srcLo);

        // Destination offset in the window scratch.
        const int64_t dstLo = srcLo - lo;            // >= 0
        // Front padding [0, dstLo), back padding [dstLo + copyN, kFrameSamples)
        std::fill(windowScratch.begin(),
                  windowScratch.begin() + size_t(dstLo),
                  0.0f);
        if (copyN > 0) {
            std::memcpy(&windowScratch[size_t(dstLo)],
                        &mono[size_t(srcLo)],
                        size_t(copyN) * sizeof(float));
        }
        const int64_t backStart = dstLo + copyN;
        std::fill(windowScratch.begin() + size_t(backStart),
                  windowScratch.end(),
                  0.0f);

        pipe.process(windowScratch.data(), kFrameSamples,
                     &coeffsRaw[size_t(t) * size_t(kNumCoeffs)]);
    }
    auto tMfcc1 = std::chrono::steady_clock::now();
    st.mfccSec = std::chrono::duration<double>(tMfcc1 - tMfcc0).count();

    // Pack into (T, FEATURE_DIM) by dropping coefficient 0.
    std::vector<float> features(size_t(hop.T) * size_t(kFeatureDim), 0.0f);
    for (int t = 0; t < hop.T; ++t) {
        const float* src = &coeffsRaw[size_t(t) * size_t(kNumCoeffs)];
        float* dst       = &features[size_t(t) * size_t(kFeatureDim)];
        for (int k = 0; k < kFeatureDim; ++k) dst[k] = src[k + 1];
    }

    normaliseFeatures(features, hop.T, kFeatureDim);

    auto tSim0 = std::chrono::steady_clock::now();
    computeSsm(features, hop.T, kFeatureDim, matrixOut);
    auto tSim1 = std::chrono::steady_clock::now();
    st.similaritySec = std::chrono::duration<double>(tSim1 - tSim0).count();

    st.success = true;
    return st;
}


void SsmGenerator::debugMfccForWindow(const float* mono, int n,
                                      double sampleRate, float* outCoeffs)
{
    MfccPipeline pipe(kFrameSamples, kNumMelFilters, kNumCoeffs,
                      sampleRate, kMelFLow, kMelFHigh);
    pipe.process(mono, n, outCoeffs);
}


bool SsmGenerator::writeSidecar(const QString& sidecarPath,
                                int T, float hopSec,
                                const uint8_t* matrix)
{
    QFile f(sidecarPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;

    // Header. Strict little-endian on every platform Qt targets, which is
    // what we want -- we're shipping a binary on-disk format.
    char header[kHeaderBytes] = {0};
    std::memcpy(header + 0, kMagic, 4);
    const uint32_t version = 1;
    std::memcpy(header + 4,  &version, 4);
    const uint32_t Tu = uint32_t(T);
    std::memcpy(header + 8,  &Tu, 4);
    std::memcpy(header + 12, &hopSec, 4);
    // Bytes 16..31 are reserved and stay zero from the {0} initialiser.

    if (f.write(header, kHeaderBytes) != qint64(kHeaderBytes)) return false;

    const qint64 bodyBytes = qint64(T) * qint64(T);
    if (f.write(reinterpret_cast<const char*>(matrix), bodyBytes) != bodyBytes)
        return false;
    return true;
}


bool SsmGenerator::isValidSidecar(const QString& sidecarPath)
{
    QFile f(sidecarPath);
    if (!f.open(QIODevice::ReadOnly)) return false;
    char header[kHeaderBytes];
    if (f.read(header, kHeaderBytes) != qint64(kHeaderBytes)) return false;
    if (std::memcmp(header, kMagic, 4) != 0) return false;
    uint32_t version = 0;
    std::memcpy(&version, header + 4, 4);
    if (version != 1) return false;
    uint32_t T = 0;
    std::memcpy(&T, header + 8, 4);
    if (T == 0 || T > uint32_t(MAX_T)) return false;
    const qint64 expected = qint64(kHeaderBytes) + qint64(T) * qint64(T);
    if (f.size() != expected) return false;
    return true;
}


bool SsmGenerator::readSidecar(const QString& sidecarPath,
                               SidecarHeader& hdrOut,
                               std::vector<uint8_t>& matrixOut)
{
    QFile f(sidecarPath);
    if (!f.open(QIODevice::ReadOnly)) return false;
    char header[kHeaderBytes];
    if (f.read(header, kHeaderBytes) != qint64(kHeaderBytes)) return false;
    if (std::memcmp(header, kMagic, 4) != 0) return false;
    uint32_t version = 0, T = 0;
    float hopSec = 0.0f;
    std::memcpy(&version, header + 4, 4);
    std::memcpy(&T,       header + 8, 4);
    std::memcpy(&hopSec,  header + 12, 4);
    if (version != 1 || T == 0 || T > uint32_t(MAX_T)) return false;

    const qint64 bodyBytes = qint64(T) * qint64(T);
    matrixOut.resize(size_t(bodyBytes));
    if (f.read(reinterpret_cast<char*>(matrixOut.data()), bodyBytes) != bodyBytes)
        return false;
    hdrOut.version = version;
    hdrOut.T       = T;
    hdrOut.hopSec  = hopSec;
    return true;
}
