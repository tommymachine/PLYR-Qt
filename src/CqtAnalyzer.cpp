#include "CqtAnalyzer.h"
#include "AudioFeatures.h"

extern "C" {
#include "kiss_fftr.h"
}

#include <QDebug>
#include <QMutexLocker>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

namespace {

constexpr float  kPi  = 3.14159265358979323846f;
constexpr double kPiD = 3.14159265358979323846;

// Sparse-kernel threshold from Schorkhuber-Klapuri (SMC 2010): drop any
// FFT'd-kernel entry whose magnitude is below 0.54% of the row max.
// Typical sparsity: ~3% of bins retained per row.
constexpr float kSparseThresholdRatio = 0.0054f;

// Display range used by fillRow(): -80 dB -> 0, 0 dB -> 255. Matches the
// kDbMin/kDbMax pair the spectrogram QML exposes.
constexpr float kDbFloor   = -80.0f;
constexpr float kDbCeiling =   0.0f;
constexpr float kDbSpan    = kDbCeiling - kDbFloor;

}  // namespace


// --- FftImpl -----------------------------------------------------------------

struct CqtAnalyzer::FftImpl {
    kiss_fftr_cfg cfg = nullptr;
    explicit FftImpl(int n) {
        cfg = kiss_fftr_alloc(n, 0, nullptr, nullptr);
    }
    ~FftImpl() {
        if (cfg) { kiss_fftr_free(cfg); kiss_fft_cleanup(); }
    }
};


// --- Decimator ---------------------------------------------------------------

void CqtAnalyzer::Decimator::reset()
{
    std::fill(state.begin(), state.end(), 0.0f);
    writeIdx = 0;
}


// --- Construction ------------------------------------------------------------

CqtAnalyzer::CqtAnalyzer(QObject* parent,
                         int binsPerOctave,
                         int nOctaves,
                         double fMin,
                         double sampleRate,
                         int fftSize)
    : QObject(parent)
    , m_binsPerOctave(binsPerOctave)
    , m_nOctaves(nOctaves)
    , m_fMin(fMin)
    , m_sampleRate(sampleRate)
    , m_fftSize(fftSize)
    , m_outputBins(binsPerOctave * nOctaves)
{
    Q_ASSERT(m_binsPerOctave > 0);
    Q_ASSERT(m_nOctaves      > 0);
    Q_ASSERT(m_fftSize       > 0 && (m_fftSize % 2) == 0);
    Q_ASSERT(m_outputBins   <= MAX_BINS);

    // Hann window for the top-octave inputs. Each lower-octave FFT also
    // uses the same window because the kernel rows are themselves
    // windowed Morlets; double-windowing is slight (Hann * Hann is a
    // Hann^2 which broadens the main lobe by ~1.4 in bin units) but
    // smoothes the kernel sidelobes nicely.
    m_window.resize(m_fftSize);
    for (int i = 0; i < m_fftSize; ++i) {
        m_window[i] = 0.5f * (1.0f - std::cos(2.0f * kPi * float(i) /
                                              float(m_fftSize - 1)));
    }

    m_fft = std::make_unique<FftImpl>(m_fftSize);
    m_fftInput.assign(m_fftSize, 0.0f);
    m_fftSpectrum.assign(m_fftSize / 2 + 1, cpx(0.0f, 0.0f));

    // Per-octave audio rings (oversized for ringing-out / startup slack).
    const int ringSize = m_fftSize * RING_MULTIPLIER;
    m_octRing.assign(m_nOctaves, std::vector<float>(ringSize, 0.0f));
    m_octRingWrite.assign(m_nOctaves, 0);
    m_octRingCount.assign(m_nOctaves, 0);

    // One decimator per octave drop. Index 0 is unused (no upstream
    // octave to decimate from) but kept for index symmetry.
    m_decimators.resize(m_nOctaves);

    buildDecimator();
    buildKernel();
}


CqtAnalyzer::~CqtAnalyzer() = default;


// --- Audio source binding ----------------------------------------------------

void CqtAnalyzer::setAudioSource(AudioFeatures* s)
{
    if (m_source == s) return;
    if (m_source) {
        disconnect(m_source, &AudioFeatures::featuresUpdated,
                   this, &CqtAnalyzer::onFeaturesUpdated);
    }
    m_source = s;
    if (m_source) {
        connect(m_source, &AudioFeatures::featuresUpdated,
                this, &CqtAnalyzer::onFeaturesUpdated,
                Qt::DirectConnection);
    }
    emit audioSourceChanged();
}


void CqtAnalyzer::setActive(bool a)
{
    if (m_active == a) return;
    m_active = a;
    emit activeChanged();
}


void CqtAnalyzer::onFeaturesUpdated()
{
    if (!m_active) return;
    if (!m_source) return;
    // Pull the last 2048 samples of L+R from AudioFeatures, mono-fold,
    // push to our top-octave ring, then run a hop. This is the standard
    // "60 Hz tick" path; offline test code calls pushSamples/computeHop
    // directly.
    constexpr int kPullN = 2048;
    if (!m_source->fillScopeStereo(m_pullL.data(), m_pullR.data(), kPullN))
        return;
    for (int i = 0; i < kPullN; ++i)
        m_pullMono[i] = 0.5f * (m_pullL[i] + m_pullR[i]);
    pushSamples(m_pullMono.data(), kPullN);
    computeHop();
}


// --- Kernel construction -----------------------------------------------------

void CqtAnalyzer::buildKernel()
{
    // Topmost octave: bin k has frequency f_k = f_top * 2^(k/B) where
    // f_top is the lowest pitch in that top octave. Because the octave-
    // recursive scheme processes the bottom octave on a (n-1)x-decimated
    // signal, the "top octave" of the kernel covers the frequency band
    // [fs/2^n_oct * 2^(n_oct-1), fs/2^n_oct * 2^n_oct) relative to the
    // original sample rate -- but ALL octaves use the same kernel
    // applied to their own (downsampled) signal, so the kernel itself
    // is parameterized only by binsPerOctave + sampleRate, not by which
    // octave we're processing.
    //
    // Convention used here: build the kernel for the topmost actual
    // octave of the original signal -- i.e., centers from
    // (f_min * 2^(n_oct-1)) to just below (f_min * 2^n_oct) -- so that
    // when we recurse and apply the SAME kernel to a 2x-decimated
    // signal, the bins land an octave lower (because the decimation
    // halves the effective sample rate). This matches Schorkhuber-
    // Klapuri's eq. 5 in the SMC 2010 paper.

    const int    B  = m_binsPerOctave;
    const double fs = m_sampleRate;

    // Bin centers for the TOP octave (highest pitches). f_top_base is
    // the lowest frequency in the top octave; the n_oct-th octave above
    // f_min.
    const double fTopBase = m_fMin * std::pow(2.0, double(m_nOctaves - 1));

    // Q factor: 1 / (2^(1/B) - 1). For B=24 this is ~34.13.
    const double Q = 1.0 / (std::pow(2.0, 1.0 / double(B)) - 1.0);

    m_sparseKernel.assign(B, {});

    // Per-row dense complex kernel, FFT'd, then thresholded into the
    // sparse representation. The dense buffer is only alive during
    // construction.
    std::vector<float>          rowReal(m_fftSize, 0.0f);
    std::vector<float>          rowImag(m_fftSize, 0.0f);
    std::vector<kiss_fft_cpx>   tmpFreqReal(m_fftSize / 2 + 1);
    std::vector<kiss_fft_cpx>   tmpFreqImag(m_fftSize / 2 + 1);

    kiss_fftr_cfg fwd = kiss_fftr_alloc(m_fftSize, 0, nullptr, nullptr);
    Q_ASSERT(fwd != nullptr);

    // For "kernel normalization": at the end we want a pure sinusoid at
    // frequency f_k to produce a magnitude near 1.0 for bin k. We
    // approximate that target by dividing every kernel value by the row
    // sum of |kernel|. This is a per-row normalization; some references
    // use sum-square instead, but row-sum gives steadier dB readings on
    // a sinusoid (see kernel-correctness test in the analyzer notes).
    for (int k = 0; k < B; ++k) {
        const double fk = fTopBase * std::pow(2.0, double(k) / double(B));
        // Window length L_k = ceil(Q * fs / f_k). The lower the bin
        // frequency, the longer the window (constant-Q). For the top
        // octave at fs=44.1k, f_k ranges over [~4186 Hz, ~8372 Hz);
        // L_k is in [~180, ~360] samples -- comfortably below fftSize.
        const int Lk = int(std::ceil(Q * fs / fk));
        if (Lk <= 1 || Lk > m_fftSize) continue;

        // Center the windowed Morlet at index N/2 so the kernel's phase
        // response is zero at the bin center. This matches Schorkhuber-
        // Klapuri's convention (and matches constant-q-cpp).
        const int start = (m_fftSize - Lk) / 2;

        // Build the windowed complex sinusoid in the time domain.
        // Coefficients filled in the dense buffer, then FFT'd below.
        // We split real/imag into two real-valued FFTs because kissfft's
        // r2c interface is what we have on hand. The complex-valued kernel
        // K = (window * cos) + i*(window * sin); FFT linearity lets us FFT
        // each part separately and recombine.
        for (int i = 0; i < m_fftSize; ++i) {
            rowReal[i] = 0.0f;
            rowImag[i] = 0.0f;
        }
        for (int n = 0; n < Lk; ++n) {
            // Hann window of length Lk.
            const float w = 0.5f * (1.0f - std::cos(2.0f * kPi *
                                                     float(n) /
                                                     float(Lk - 1)));
            // The complex sinusoid sits inside the window. Note that
            // because we pad with zeros outside the window, the time-
            // domain phase is anchored at index `start` (the start of the
            // window). Schorkhuber-Klapuri use this same anchoring.
            const double phase = 2.0 * kPiD * fk * double(n) / fs;
            rowReal[start + n] = w * float(std::cos(phase));
            rowImag[start + n] = w * float(std::sin(phase));
        }

        kiss_fftr(fwd, rowReal.data(), tmpFreqReal.data());
        kiss_fftr(fwd, rowImag.data(), tmpFreqImag.data());

        // K_freq[j] = (real_freq[j].r + i*real_freq[j].i)
        //           + i * (imag_freq[j].r + i*imag_freq[j].i)
        //           = (real_freq[j].r - imag_freq[j].i)
        //           + i*(real_freq[j].i + imag_freq[j].r).
        const int specLen = m_fftSize / 2 + 1;
        std::vector<cpx> dense(specLen);
        float rowMax = 0.0f;
        for (int j = 0; j < specLen; ++j) {
            const float re = tmpFreqReal[j].r - tmpFreqImag[j].i;
            const float im = tmpFreqReal[j].i + tmpFreqImag[j].r;
            dense[j] = cpx(re, im);
            rowMax = std::max(rowMax, std::abs(dense[j]));
        }
        if (rowMax <= 0.0f) continue;

        // Sparse threshold: keep entries above 0.54% of the row max.
        const float thr = rowMax * kSparseThresholdRatio;
        auto& row = m_sparseKernel[k];
        row.reserve(64);  // typical retained count is ~3% of (fftSize/2)
        float rowAbsSum = 0.0f;
        for (int j = 0; j < specLen; ++j) {
            if (std::abs(dense[j]) >= thr) {
                row.push_back({j, dense[j]});
                rowAbsSum += std::abs(dense[j]);
            }
        }
        // Per-row normalization: divide every retained kernel value by
        // the sum of |kernel| along this row. This makes a pure-tone
        // input at f_k read close to 1.0 in linear magnitude (see
        // verifySinusoid() in the test code).
        if (rowAbsSum > 0.0f) {
            const float invSum = 1.0f / rowAbsSum;
            for (auto& e : row) e.value *= invSum;
        }
    }

    kiss_fftr_free(fwd);
    kiss_fft_cleanup();

    // Report kernel sparsity to the debug log. Useful when tuning the
    // threshold; in steady state we just want to confirm the storage
    // footprint is in the expected ~3% retained range.
    int retained = 0;
    for (const auto& row : m_sparseKernel) retained += int(row.size());
    const int dense = B * (m_fftSize / 2 + 1);
    if (dense > 0) {
        const double pct = 100.0 * double(retained) / double(dense);
        qInfo("CqtAnalyzer: kernel sparsity = %d / %d retained (%.2f%%) "
              "[B=%d, fftSize=%d]",
              retained, dense, pct, B, m_fftSize);
    }

    // The kissfft real FFT splits the energy of a real signal between
    // bins 1..N/2-1 (each carrying half the energy of the corresponding
    // complex DFT bin) and bins 0 / N/2 (which are real). To recover the
    // "single-sided" interpretation we use during the per-hop inner
    // loop, we multiply the kernel entries at bins 1..N/2-1 by 2. We do
    // this once here so the inner loop stays trivial.
    //
    // Why this works: for a real-valued input x[n], the full DFT is
    // conjugate-symmetric (X[N-k] = X[k]*). kissfft only emits the
    // non-redundant half (bins 0..N/2). A linear functional over the
    // full DFT that, by construction, isolates positive frequencies
    // (which our complex kernels do) sees twice the energy when we sum
    // over only the non-redundant half. So scale by 2 for bins 1..N/2-1.
    const int lastBin = m_fftSize / 2;
    for (auto& row : m_sparseKernel) {
        for (auto& e : row) {
            if (e.bin > 0 && e.bin < lastBin) e.value *= 2.0f;
        }
    }
}


// --- Decimator construction --------------------------------------------------

void CqtAnalyzer::buildDecimator()
{
    // Polyphase 32-tap windowed-sinc low-pass at f_pass = 0.45 * (fs/2) /
    // 2 = 0.225 * fs (i.e., -3 dB just below Nyquist/4). The cutoff is
    // chosen so the post-decimation alias band is at least 6 dB down --
    // adequate for music visualization; sharper rejection would only
    // matter if we were measuring noise floors.
    //
    // Why 32 taps: tradeoff between linear-phase length (8 samples one-
    // sided latency at the top octave; doubles per octave down) and
    // stop-band rejection. 32 taps with a Blackman-Harris window puts
    // the stop-band at ~-90 dB by the time the alias band kicks in --
    // well below the dB display floor (-80 dB).
    //
    // The kernel itself is the same for every octave; the per-octave
    // state (delay line) lives in m_decimators[i].state, sized to one
    // tap less than the kernel length so write-and-read indices don't
    // alias.

    constexpr int kTaps = 32;

    // Design parameters.
    const double fc = 0.225;             // normalized cutoff (cycles/sample)
    const double n  = double(kTaps);
    std::vector<float> h(kTaps, 0.0f);
    for (int i = 0; i < kTaps; ++i) {
        const double t = double(i) - (n - 1.0) * 0.5;
        // Sinc.
        double sinc;
        if (std::abs(t) < 1e-9) {
            sinc = 2.0 * fc;
        } else {
            sinc = std::sin(2.0 * kPiD * fc * t) / (kPiD * t);
        }
        // Blackman-Harris window for ~92 dB stop-band.
        const double a0 = 0.35875, a1 = 0.48829;
        const double a2 = 0.14128, a3 = 0.01168;
        const double x  = 2.0 * kPiD * double(i) / (n - 1.0);
        const double w  = a0 - a1 * std::cos(x) + a2 * std::cos(2.0 * x)
                                                - a3 * std::cos(3.0 * x);
        h[i] = float(sinc * w);
    }
    // Renormalize so the DC gain is exactly 1.0 -- guarantees an octave
    // of decimation doesn't bias the signal toward zero.
    float sum = std::accumulate(h.begin(), h.end(), 0.0f);
    if (sum > 0.0f) for (auto& v : h) v /= sum;

    for (int i = 1; i < m_nOctaves; ++i) {
        m_decimators[i].taps  = h;        // same kernel for every octave
        m_decimators[i].state.assign(kTaps, 0.0f);
        m_decimators[i].writeIdx = 0;
    }
}


// --- Sample ingestion --------------------------------------------------------

void CqtAnalyzer::pushSamples(const float* data, int n)
{
    if (!data || n <= 0) return;
    QMutexLocker lk(&m_ringMutex);

    // Octave 0: write directly.
    {
        auto& ring = m_octRing[0];
        const int cap = int(ring.size());
        int w = m_octRingWrite[0];
        for (int i = 0; i < n; ++i) {
            ring[w] = data[i];
            if (++w == cap) w = 0;
        }
        m_octRingWrite[0] = w;
        m_octRingCount[0] += n;
    }

    // Octaves 1..n: decimate from the previous octave by 2.
    decimateInto(1, data, n);
}


void CqtAnalyzer::decimateInto(int octave, const float* in, int inN)
{
    if (octave >= m_nOctaves) return;

    Decimator& dec = m_decimators[octave];
    const int taps = int(dec.taps.size());
    auto& ring = m_octRing[octave];
    const int cap = int(ring.size());
    int wr  = m_octRingWrite[octave];

    // Outputs are produced for every other input sample (decimate-by-2).
    // We process samples one at a time so the per-decimator state cleanly
    // tracks the phase, even when inN is odd. Output is captured into a
    // scratch buffer because we then need to recurse to the next octave
    // with that decimated stream.

    // Worst-case: every input sample produces zero outputs in the wrong
    // phase + every other produces an output. So output count <= inN/2 + 1.
    // Push into a small static-ish scratch on the stack via vector
    // (constructed inside the call but bounded so the heap allocation is
    // small and only at hop time; in steady state it's still allocation
    // per call). For tighter realtime we could keep a per-decimator
    // scratch member; in practice this is invoked a handful of times per
    // hop with inN <= 2048, so the allocation cost is negligible.
    //
    // No allocations on the per-hop hot path: pre-allocate one persistent
    // scratch buffer the first time we see a worst-case size, then reuse.
    static thread_local std::vector<float> outScratch;
    if (int(outScratch.size()) < (inN / 2 + 1))
        outScratch.assign(inN / 2 + 1, 0.0f);

    int outCount = 0;
    for (int i = 0; i < inN; ++i) {
        // Write into state (the delay line is a simple ring of length
        // `taps`). After each input we may or may not emit an output;
        // emit on every other sample to halve the rate.
        dec.state[dec.writeIdx] = in[i];
        dec.writeIdx++;
        if (dec.writeIdx == taps) dec.writeIdx = 0;

        // Phase: output is produced on every second input. (Don't name
        // the local "emit" -- that's a Qt keyword.) We sequence on the
        // local counter to keep the path predictable.
        if ((i & 1) == 0) continue;

        // FIR dot product over the current state. Indexing is laid out
        // so that `state[(writeIdx + k) % taps]` reads the oldest
        // sample at offset k -- which lines up with tap[k]. Branchless
        // modulo by precomputing the unwrapped start index.
        float acc = 0.0f;
        int   idx = dec.writeIdx;  // points to the oldest sample
        for (int k = 0; k < taps; ++k) {
            acc += dec.taps[k] * dec.state[idx];
            if (++idx == taps) idx = 0;
        }
        outScratch[outCount++] = acc;
    }

    if (outCount == 0) return;

    // Write the decimated samples into octave `octave`'s ring.
    for (int i = 0; i < outCount; ++i) {
        ring[wr] = outScratch[i];
        if (++wr == cap) wr = 0;
    }
    m_octRingWrite[octave] = wr;
    m_octRingCount[octave] += outCount;

    // Recurse one octave deeper.
    decimateInto(octave + 1, outScratch.data(), outCount);
}


// --- Per-hop CQT -------------------------------------------------------------

bool CqtAnalyzer::computeHop()
{
    // Gate only on the TOP octave -- the one running at full sample
    // rate. Lower octaves are inherently slow to accumulate samples
    // (the lowest octave gets one sample per 2^(n_oct-1) inputs, which
    // means its full analysis window needs ~12 seconds of audio for a
    // 4096-sample window at 44.1 kHz). Rather than wait for all octaves
    // to fill, we let lower-octave windows include their initial zero
    // pad: pitches in those octaves will read a smeared/low magnitude
    // for the first few seconds, but the higher octaves -- which carry
    // most of the musical content -- are correct from hop one.
    if (m_octRingCount[0] < m_fftSize) return false;

    QMutexLocker lk(&m_ringMutex);

    // Process octaves from the lowest pitch (most-decimated) to the
    // highest. Output layout: index 0 = lowest pitch (C1), index 191 =
    // highest. Octave i (0-indexed from low) lives on m_octRing[
    // n_oct - 1 - i] -- because we decimated the highest index the most.
    for (int oct = 0; oct < m_nOctaves; ++oct) {
        const int ringIdx = m_nOctaves - 1 - oct;
        const auto& ring = m_octRing[ringIdx];
        const int cap = int(ring.size());
        const int w = m_octRingWrite[ringIdx];

        // Pull the most recent fftSize samples and apply the analysis
        // window. Linearize the ring into m_fftInput.
        int src = w - m_fftSize;
        if (src < 0) src += cap;
        for (int i = 0; i < m_fftSize; ++i) {
            m_fftInput[i] = ring[(src + i) % cap] * m_window[i];
        }

        // Real FFT.
        kiss_fftr(m_fft->cfg,
                  m_fftInput.data(),
                  reinterpret_cast<kiss_fft_cpx*>(m_fftSpectrum.data()));

        // Apply the sparse kernel. The kernel was built for the top
        // octave but, by the Schorkhuber-Klapuri octave-recursive
        // identity, the SAME kernel reads correctly on each octave
        // after the audio has been decimated to that octave's rate.
        // Output goes into m_workCqt starting at oct * B.
        applyKernel(m_fftSpectrum.data(),
                    m_fftSize / 2 + 1,
                    oct * m_binsPerOctave);
    }

    // Publish under the output mutex.
    {
        QMutexLocker outLk(&m_outMutex);
        std::memcpy(m_lastCqt.data(), m_workCqt.data(),
                    sizeof(float) * m_outputBins);
        for (int i = 0; i < m_outputBins; ++i) {
            const float mag = std::max(m_lastCqt[i], 1e-10f);
            const float db  = 20.0f * std::log10(mag);
            const float t   = std::clamp((db - kDbFloor) / kDbSpan, 0.0f, 1.0f);
            m_lastCqtBytes[i] = uint8_t(std::clamp(t * 255.0f, 0.0f, 255.0f));
        }
        // Zero the tail beyond outputBins.
        for (int i = m_outputBins; i < MAX_BINS; ++i)
            m_lastCqtBytes[i] = 0;
        m_haveHop.store(true, std::memory_order_release);
    }

    emit hopComplete();
    return true;
}


void CqtAnalyzer::applyKernel(const cpx* spectrum, int /*specLen*/, int outOffset)
{
    const int B = m_binsPerOctave;
    for (int k = 0; k < B; ++k) {
        const auto& row = m_sparseKernel[k];
        cpx acc(0.0f, 0.0f);
        for (const auto& e : row) {
            // Complex multiply-accumulate: K_freq[k,j] * X[j].
            acc += e.value * spectrum[e.bin];
        }
        m_workCqt[outOffset + k] = std::abs(acc);
    }
}


// --- Render-thread accessor --------------------------------------------------

bool CqtAnalyzer::fillRow(uint8_t* out, int n)
{
    if (!out || n <= 0) return false;
    if (!m_haveHop.load(std::memory_order_acquire)) {
        std::memset(out, 0, n);
        return false;
    }
    QMutexLocker lk(&m_outMutex);
    const int copyN = std::min(n, m_outputBins);
    std::memcpy(out, m_lastCqtBytes.data(), copyN);
    if (n > copyN) std::memset(out + copyN, 0, n - copyN);
    return true;
}


bool CqtAnalyzer::fillRowFloat(float* out, int n)
{
    if (!out || n <= 0) return false;
    if (!m_haveHop.load(std::memory_order_acquire)) {
        std::memset(out, 0, sizeof(float) * size_t(n));
        return false;
    }
    QMutexLocker lk(&m_outMutex);
    const int copyN = std::min(n, m_outputBins);
    std::memcpy(out, m_lastCqt.data(), sizeof(float) * size_t(copyN));
    if (n > copyN) std::memset(out + copyN, 0, sizeof(float) * size_t(n - copyN));
    return true;
}
