#include "HilbertAnalyzer.h"
#include "AudioFeatures.h"

#include <QMutexLocker>
#include <QVariantList>
#include <algorithm>
#include <cmath>
#include <cstring>


// ---------------------------------------------------------------------------
//  31-tap Type-III FIR Hilbert kernel
//
//  Design: ideal Hilbert frequency response h_id[k] = (2 / (pi*k)) *
//  sin^2(pi*k/2) for k != 0, with h_id[0] = 0; sample-shifted to a
//  causal 31-tap filter (k = -15..+15) and tapered by a Kaiser(N=31,
//  beta=6.0) window. Antisymmetric about k=0 (h[-k] = -h[k]); the DC
//  gain is identically zero. Group delay = 15 samples.
//
//  Coefficients precomputed offline by a tiny Python helper (Kaiser I0
//  series + the closed-form ideal kernel above). Baked here as a literal
//  std::array so the analyzer has zero startup cost.
//
//  Magnitude response (zero-phase, computed analytically):
//    omega/pi    |H|
//    -------    -----
//    0.02       0.294   (~440 Hz @ fs=44100 -- intentionally low; the
//                        bandpass in front strips most sub-passband
//                        energy, so this droop only affects the very
//                        lowest analysis band)
//    0.05       0.663
//    0.10       0.962
//    0.20       0.999
//    0.50       1.000
//    0.80       0.999
//    0.95       0.663
//    0.98       0.294
//
//  We compensate per-band by 1 / |H(2pi*f0/fs)| at construction so the
//  reported envelope matches the true bandpass amplitude.
// ---------------------------------------------------------------------------

const std::array<float, HilbertAnalyzer::HILBERT_TAPS>
HilbertAnalyzer::kHilbert = {
    -0.000631244032f,        // k = -15
     0.000000000000f,        // k = -14
    -0.003535806594f,        // k = -13
     0.000000000000f,        // k = -12
    -0.010418554248f,        // k = -11
     0.000000000000f,        // k = -10
    -0.023980622006f,        // k =  -9
     0.000000000000f,        // k =  -8
    -0.048507227406f,        // k =  -7
     0.000000000000f,        // k =  -6
    -0.093187818549f,        // k =  -5
     0.000000000000f,        // k =  -4
    -0.190009820480f,        // k =  -3
     0.000000000000f,        // k =  -2
    -0.628914692254f,        // k =  -1
     0.000000000000f,        // k =   0
     0.628914692254f,        // k =  +1
     0.000000000000f,        // k =  +2
     0.190009820480f,        // k =  +3
     0.000000000000f,        // k =  +4
     0.093187818549f,        // k =  +5
     0.000000000000f,        // k =  +6
     0.048507227406f,        // k =  +7
     0.000000000000f,        // k =  +8
     0.023980622006f,        // k =  +9
     0.000000000000f,        // k = +10
     0.010418554248f,        // k = +11
     0.000000000000f,        // k = +12
     0.003535806594f,        // k = +13
     0.000000000000f,        // k = +14
     0.000631244032f,        // k = +15
};


namespace {

constexpr float  kPi  = 3.14159265358979323846f;
constexpr double kPiD = 3.14159265358979323846;

// Two-time-constant envelope-follower per-sample coefficient.
// alpha = exp(-1 / (tau_sec * fs)).
inline float alphaForTau(double tauSec, double fs)
{
    return float(std::exp(-1.0 / (std::max(tauSec, 1e-6) * fs)));
}

// |H(omega)| of the windowed Hilbert kernel, evaluated by direct sum.
// Used at construction only -- N_BANDS evaluations, 31 taps each.
inline float hilbertGainAt(const std::array<float, HilbertAnalyzer::HILBERT_TAPS>& h,
                           double omega)
{
    double re = 0.0, im = 0.0;
    const int half = HilbertAnalyzer::HILBERT_DELAY;
    for (int i = 0; i < HilbertAnalyzer::HILBERT_TAPS; ++i) {
        const int k = i - half;                  // -15..+15
        const double phase = -omega * double(k);
        re += double(h[i]) * std::cos(phase);
        im += double(h[i]) * std::sin(phase);
    }
    return float(std::sqrt(re * re + im * im));
}

// Wrap a phase difference into [-pi, pi] for stable instantaneous
// frequency. dphi = wrap(phase_now - phase_prev); instFreq = dphi * fs/(2pi).
inline float wrapPhaseDiff(float dphi)
{
    while (dphi >  kPi) dphi -= 2.0f * kPi;
    while (dphi < -kPi) dphi += 2.0f * kPi;
    return dphi;
}

}  // namespace


// --- Construction ----------------------------------------------------------

HilbertAnalyzer::HilbertAnalyzer(QObject* parent)
    : QObject(parent)
    , m_pullL(FRAME_SAMPLES, 0.0f)
    , m_pullR(FRAME_SAMPLES, 0.0f)
    , m_mono (FRAME_SAMPLES, 0.0f)
{
    buildBands();
}

HilbertAnalyzer::~HilbertAnalyzer() = default;


// --- Band construction -----------------------------------------------------

void HilbertAnalyzer::buildBands()
{
    // Log-spaced centers: f_b = exp(linspace(log(80), log(8000), N_BANDS)).
    // Bandwidth = 2/3 octave per band -> BW = f * (2^(1/3) - 2^(-1/3)).
    constexpr double kLo     = 80.0;
    constexpr double kHi     = 8000.0;
    const double     kThirdL = std::log(kLo);
    const double     kThirdH = std::log(kHi);
    const float      bwScale = float(std::pow(2.0, 1.0/3.0) -
                                     std::pow(2.0, -1.0/3.0));   // ~0.5874

    // Two-time-constant follower at the sample rate (we step the
    // envelope once per audio sample, not once per hop).
    constexpr double kAttackTau  = 0.030;     // 30 ms
    constexpr double kReleaseTau = 0.100;     // 100 ms

    for (int b = 0; b < N_BANDS; ++b) {
        const double t  = double(b) / double(N_BANDS - 1);
        const double fb = std::exp(kThirdL + t * (kThirdH - kThirdL));
        const float  bw = float(fb) * bwScale;
        m_fc[b] = float(fb);
        m_bw[b] = bw;

        // RBJ bandpass biquad (constant 0-dB peak gain form). See
        // Bristow-Johnson's "Cookbook formulae for audio EQ biquad filter
        // coefficients", section on Bandpass (BPF).
        const double w0    = 2.0 * kPiD * fb / m_sampleRate;
        const double cw0   = std::cos(w0);
        const double sw0   = std::sin(w0);
        const double Q     = fb / double(bw);
        const double alpha = sw0 / (2.0 * Q);

        const double b0 =  alpha;
        const double b1 =  0.0;
        const double b2 = -alpha;
        const double a0 =  1.0 + alpha;
        const double a1 = -2.0 * cw0;
        const double a2 =  1.0 - alpha;

        BandState& s = m_bands[b];
        s.b0 = float(b0 / a0);
        s.b1 = float(b1 / a0);
        s.b2 = float(b2 / a0);
        s.a1 = float(a1 / a0);
        s.a2 = float(a2 / a0);

        s.x1 = s.x2 = s.y1 = s.y2 = 0.0f;
        s.ring.fill(0.0f);
        s.ringHead       = 0;
        s.lastAnalyticRe = 0.0f;
        s.lastAnalyticIm = 0.0f;
        s.lastPhase      = 0.0f;
        s.havePrevPhase  = false;
        s.env            = 0.0f;

        s.alphaAttack    = alphaForTau(kAttackTau,  m_sampleRate);
        s.alphaRelease   = alphaForTau(kReleaseTau, m_sampleRate);

        // Hilbert-gain compensation at this band's center.
        //
        //  Theory: ideal H has |H| = 1 across [0, pi]; the windowed FIR
        //  rolls off near DC and Nyquist (~0.66 at omega = 0.05*pi for
        //  the 31-tap Kaiser design). Compensating by 1/|H(omega_b)|
        //  recovers the nominal amplitude for content AT the band
        //  centre.
        //
        //  In practice we cap the gain at 1.0 -- two reasons:
        //    1. The bandpass skirts let some out-of-band energy through.
        //       At the lowest band (80 Hz) the FIR gain |H(0.0036*pi)|
        //       is tiny (~0.05); 1 / that is ~20. Any leak from
        //       elsewhere in the spectrum gets amplified twentyfold,
        //       which trashes the envelope. Capping at 1 keeps the
        //       low band's envelope a slight under-read for true 80 Hz
        //       content but eliminates the leak amplification.
        //    2. For the visualizer, relative order between bands is
        //       what matters; we don't need calibrated SPL.
        const float g = hilbertGainAt(kHilbert, 2.0 * kPiD * fb / m_sampleRate);
        s.hilbertGain  = (g > 0.5f) ? (1.0f / g) : 1.0f;
    }
}


// --- Audio source binding --------------------------------------------------

void HilbertAnalyzer::setAudioSource(AudioFeatures* s)
{
    if (m_source == s) return;
    if (m_source) {
        disconnect(m_source, &AudioFeatures::featuresUpdated,
                   this, &HilbertAnalyzer::onFeaturesUpdated);
    }
    m_source = s;
    if (m_source) {
        connect(m_source, &AudioFeatures::featuresUpdated,
                this, &HilbertAnalyzer::onFeaturesUpdated,
                Qt::DirectConnection);
    }
    emit audioSourceChanged();
}


// --- Hop handler -----------------------------------------------------------

void HilbertAnalyzer::onFeaturesUpdated()
{
    if (!m_source) return;
    if (!m_source->fillScopeStereo(m_pullL.data(),
                                   m_pullR.data(),
                                   FRAME_SAMPLES))
        return;

    // Mono-fold on the way into m_mono so the inner loop only touches
    // one buffer per sample.
    for (int i = 0; i < FRAME_SAMPLES; ++i)
        m_mono[i] = 0.5f * (m_pullL[i] + m_pullR[i]);

    processBlock(m_mono.data(), FRAME_SAMPLES);

    // processBlock already published the snapshot under m_outMutex. Just
    // flip the "have data" flag and wake the renderer.
    m_haveBands.store(true, std::memory_order_release);
    emit bandsUpdated();
}


// --- Per-block band pipeline ----------------------------------------------

void HilbertAnalyzer::processBlock(const float* mono, int n)
{
    if (!mono || n <= 0) return;

    // We need to keep separate per-sample state for each band, but we
    // want the loop to be cache-friendly (one band at a time keeps the
    // BandState struct hot in L1). 8 bands * ~3 kSamples = 24 kSamples
    // through ~200 flops each. Cheap.
    //
    //  Output staging -- we accumulate each band's final values in a
    //  local stack scratch then commit all 8 to the output snapshot
    //  under m_outMutex in a single short critical section. This
    //  prevents the renderer from observing a half-updated snapshot
    //  where (say) band 3 is from this hop but band 4 is still from
    //  the previous hop.
    std::array<float, N_BANDS> stagedEnv {}, stagedPhase {}, stagedFreq {};

    for (int b = 0; b < N_BANDS; ++b) {
        BandState& s = m_bands[b];

        // Snapshot biquad / envelope coefficients into locals -- frees
        // the compiler from re-reading them through the struct base on
        // every sample.
        const float b0 = s.b0, b1 = s.b1, b2 = s.b2;
        const float a1 = s.a1, a2 = s.a2;
        const float aAtt = s.alphaAttack;
        const float aRel = s.alphaRelease;
        const float hilGain = s.hilbertGain;
        float x1 = s.x1, x2 = s.x2;
        float y1 = s.y1, y2 = s.y2;
        float env = s.env;
        int   ringHead = s.ringHead;

        // Carry the previous block's last analytic sample as the
        // "previous" point so the first phase-diff across a block
        // boundary uses the actual neighbour, not (0,0).
        float zRe = s.lastAnalyticRe;
        float zIm = s.lastAnalyticIm;
        // Total unwrapped phase advance across this block. Updated each
        // sample by adding wrap(phase[n] - phase[n-1]); divided by n at
        // the end to get the mean phase advance per sample -> mean
        // instantaneous frequency. Block-averaging is much steadier
        // than the single-sample phase diff (~440 Hz with ~50 Hz jitter
        // vs ~ samples of stochastic noise).
        double phiAccum = 0.0;
        int    phiSamples = 0;
        float  lastPhase = s.havePrevPhase ? s.lastPhase
                                           : std::atan2(zIm, zRe);

        for (int i = 0; i < n; ++i) {
            // 1. RBJ biquad (Direct Form I -- one mul + add per
            //    coefficient, branch-free).
            const float x0 = mono[i];
            float yBP = b0 * x0 + b1 * x1 + b2 * x2
                                - a1 * y1 - a2 * y2;
            x2 = x1; x1 = x0;
            y2 = y1; y1 = yBP;

            // 2. Push bandpassed sample into the Hilbert delay ring.
            //    The ring is HILBERT_TAPS long; ringHead is the slot we
            //    just wrote into. The Hilbert convolution sums h[k] *
            //    ring[(ringHead - k + TAPS) % TAPS] for k = 0..TAPS-1
            //    (i.e. h[0] multiplies the *newest* sample, h[TAPS-1]
            //    the *oldest*). The aligned bandpassed sample (the one
            //    we add as the real part of z) is at offset HILBERT_DELAY
            //    into the ring -- the FIR delays by exactly that many
            //    samples.
            s.ring[ringHead] = yBP;
            ringHead = (ringHead + 1) % HILBERT_TAPS;

            // 3. Hilbert convolution + compensation.
            //
            //  Causal implementation. kHilbert[] is laid out so
            //  kHilbert[i] corresponds to coefficient h[i - 15] (the
            //  zero-phase tap at k = -15 sits at index 0; the tap at
            //  k = +15 sits at index 30). The standard convolution form
            //
            //      y[n - 15] = sum_{k=-15..+15} h[k] * x[(n - 15) - k]
            //
            //  rewrites with i = k + 15 to
            //
            //      y[n - 15] = sum_{i=0..30} kHilbert[i] * x[n - i]
            //
            //  i.e. the *newest* sample multiplies kHilbert[0] (= -h[+15]
            //  numerically; the antisymmetric kernel makes this and the
            //  oldest-sample tap equal-magnitude / opposite-sign).
            float hOut = 0.0f;
            int idx = (ringHead - 1 + HILBERT_TAPS) % HILBERT_TAPS;  // newest slot
            for (int i = 0; i < HILBERT_TAPS; ++i) {
                hOut += kHilbert[i] * s.ring[idx];
                idx -= 1;
                if (idx < 0) idx = HILBERT_TAPS - 1;
            }

            // 4. The real part of z is the bandpassed sample delayed by
            //    HILBERT_DELAY samples -- read directly from the ring at
            //    (ringHead - 1 - HILBERT_DELAY).
            int realIdx = (ringHead - 1 - HILBERT_DELAY + HILBERT_TAPS)
                          % HILBERT_TAPS;
            const float zReNew = s.ring[realIdx];
            const float zImNew = hOut * hilGain;

            // 5. Envelope follower (two-time-constant) on |z|.
            //    Both endpoints rescaled by hilGain so the channel that
            //    contributes to magnitude treats the real and imaginary
            //    parts symmetrically.
            const float zReComp = zReNew * hilGain;
            const float mag = std::sqrt(zReComp * zReComp + zImNew * zImNew);
            const float a   = (mag > env) ? aAtt : aRel;
            env = a * env + (1.0f - a) * mag;

            zRe = zReComp;  zIm = zImNew;

            // Per-sample phase advance. Use complex division via
            // z[n] * conj(z[n-1]) trick? No -- just two atan2 per
            // sample is ~30 ns; 8 bands * 2048 = 16k atan2/hop is well
            // under our 16 ms budget. Wrap into [-pi, pi] then
            // accumulate.
            const float p   = std::atan2(zIm, zRe);
            const float dpi = wrapPhaseDiff(p - lastPhase);
            phiAccum   += double(dpi);
            phiSamples += 1;
            lastPhase   = p;
        }

        // 6. Final-sample analytic state -> publish.
        //    Phase reported = the actual final-sample phase (this is
        //    what drives the dot's angular position).
        //    Instantaneous frequency = mean unwrapped phase advance per
        //    sample, scaled to Hz. Block-averaging removes the per-sample
        //    noise that would otherwise make the displayed freq jitter
        //    wildly.
        const float finalPhase = lastPhase;
        const float meanDphi   = (phiSamples > 0)
                               ? float(phiAccum / double(phiSamples))
                               : 0.0f;
        const float instFreq   = meanDphi * float(m_sampleRate) / (2.0f * kPi);

        s.x1 = x1; s.x2 = x2; s.y1 = y1; s.y2 = y2;
        s.env            = env;
        s.ringHead       = ringHead;
        s.lastAnalyticRe = zRe;
        s.lastAnalyticIm = zIm;
        s.lastPhase      = finalPhase;
        s.havePrevPhase  = true;

        // Stage to a per-band scratch (no lock yet) so the renderer
        // never sees a half-updated snapshot. We publish all 8 bands
        // atomically below.
        stagedEnv  [b] = std::clamp(env, 0.0f, 4.0f);
        stagedPhase[b] = finalPhase;
        stagedFreq [b] = instFreq;
    }

    // Publish all 8 bands at once -- single short critical section.
    {
        QMutexLocker lk(&m_outMutex);
        m_outEnv   = stagedEnv;
        m_outPhase = stagedPhase;
        m_outFreq  = stagedFreq;
    }
}


// --- Render-thread accessor -----------------------------------------------

bool HilbertAnalyzer::fillBandStates(float* env, float* phase, float* instFreq)
{
    if (!env || !phase || !instFreq) return false;
    if (!m_haveBands.load(std::memory_order_acquire)) {
        for (int b = 0; b < N_BANDS; ++b) {
            env[b]      = 0.0f;
            phase[b]    = 0.0f;
            instFreq[b] = 0.0f;
        }
        return false;
    }
    QMutexLocker lk(&m_outMutex);
    std::memcpy(env,      m_outEnv.data(),   sizeof(float) * N_BANDS);
    std::memcpy(phase,    m_outPhase.data(), sizeof(float) * N_BANDS);
    std::memcpy(instFreq, m_outFreq.data(),  sizeof(float) * N_BANDS);
    return true;
}


QVariantList HilbertAnalyzer::bandCenters() const
{
    QVariantList out;
    out.reserve(N_BANDS);
    for (int b = 0; b < N_BANDS; ++b)
        out.append(double(m_fc[b]));
    return out;
}


void HilbertAnalyzer::debugPushMono(const float* mono, int n)
{
    processBlock(mono, n);
    m_haveBands.store(true, std::memory_order_release);
}


void HilbertAnalyzer::debugReset()
{
    for (auto& s : m_bands) {
        s.x1 = s.x2 = s.y1 = s.y2 = 0.0f;
        s.ring.fill(0.0f);
        s.ringHead       = 0;
        s.lastAnalyticRe = 0.0f;
        s.lastAnalyticIm = 0.0f;
        s.lastPhase      = 0.0f;
        s.havePrevPhase  = false;
        s.env            = 0.0f;
    }
    {
        QMutexLocker lk(&m_outMutex);
        m_outEnv.fill(0.0f);
        m_outPhase.fill(0.0f);
        m_outFreq.fill(0.0f);
    }
    m_haveBands.store(false, std::memory_order_release);
}
