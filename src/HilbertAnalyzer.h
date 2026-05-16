// HilbertAnalyzer -- per-band instantaneous amplitude + phase extractor
// for the Layer 4c "Hilbert-pair rosette" visualizer.
//
// Pipeline per AudioFeatures::featuresUpdated tick:
//   1. Pull the latest 2048-sample stereo scope frame, mono-fold to a
//      single float buffer.
//   2. For each of N_BANDS log-spaced subbands:
//        a. Stream the mono samples through a one-pole RBJ biquad
//           bandpass (state-preserved across hops -- continuous IIR,
//           no frame-boundary discontinuity).
//        b. Stream the bandpassed signal through a 31-tap Type-III FIR
//           Hilbert transformer, also state-preserved (continuous FIR
//           via a 31-deep circular delay line). The FIR has a 15-sample
//           group delay, so we align the bandpassed signal with the
//           Hilbert output by reading from index (head - 15) of a small
//           in-place delay line. The result is the analytic signal
//           z[n] = x_bp[n - 15] + j * H[x_bp][n].
//        c. From the latest analytic sample compute:
//             env = |z|
//             phase = atan2(Im, Re)            (wrapped to [-pi, pi])
//             instFreq = unwrap(phase[n] - phase[n-1]) * fs / (2 * pi)
//        d. Smooth `env` with a two-time-constant envelope follower
//           (~30 ms attack, 100 ms release) at the sample rate.
//   3. Stash N_BANDS triples (env_smoothed, phase, instFreq) atomically;
//      signal bandsUpdated() so the visualizer wakes the renderer.
//
// Implementation choice -- FIR Hilbert vs. FFT method
//   The FFT method (Marple, "Computing the Discrete-Time Analytic Signal
//   via FFT", IEEE TSP 1999) is simpler -- zero the negative-frequency
//   bins, double the positives, IFFT. But it operates frame-by-frame:
//   the resulting analytic signal has a phase discontinuity at every
//   frame boundary, which destroys the instantaneous-frequency estimate
//   that we feed into the dot's orbital motion. The 31-tap FIR (Oppenheim
//   & Schafer, "Discrete-Time Signal Processing", 3e Sec. 12.4) is a
//   continuous time-domain filter whose state crosses frame boundaries
//   cleanly. We pay 31 taps x 8 bands = 248 MACs per sample (~11 MMACs/s
//   @ 44.1 kHz mono) which is negligible.
//
// References (formulas only, no copied code):
//   - Oppenheim & Schafer (2010), "Discrete-Time Signal Processing", 3e,
//     Sec. 12.4 "Discrete-Time Hilbert Transformers" (Type-III/IV FIR
//     design via ideal frequency response + Kaiser window).
//   - Marple (1999), "Computing the Discrete-Time Analytic Signal via
//     FFT", IEEE Trans. Signal Process. 47(9), 2600-2603 -- the FFT
//     alternative we deliberately did NOT use, for the reason above.
//   - Robert Bristow-Johnson, "Cookbook formulae for audio EQ biquad
//     filter coefficients" (Audio EQ Cookbook) -- the RBJ bandpass.
//
// Threading: lives on the GUI thread, driven by featuresUpdated. The
// per-hop work is small (~3 kSamples * 8 bands * a few flops) and stays
// well under a render-frame budget. fillBandStates() takes a brief
// output mutex to snapshot under low contention.

#pragma once

#include <QMutex>
#include <QObject>
#include <QVariant>
#include <QVariantList>
#include <array>
#include <atomic>
#include <qqmlregistration.h>
#include <vector>

class AudioFeatures;

class HilbertAnalyzer : public QObject {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(AudioFeatures* audioSource READ audioSource WRITE setAudioSource
               NOTIFY audioSourceChanged)

public:
    static constexpr int N_BANDS         = 8;
    static constexpr int HILBERT_TAPS    = 31;     // Type-III FIR
    static constexpr int HILBERT_DELAY   = 15;     // (HILBERT_TAPS - 1) / 2
    static constexpr int FRAME_SAMPLES   = 2048;

    explicit HilbertAnalyzer(QObject* parent = nullptr);
    ~HilbertAnalyzer() override;

    AudioFeatures* audioSource() const { return m_source; }
    void           setAudioSource(AudioFeatures* s);

    // Render-thread callable accessor. Each output array must hold at
    // least N_BANDS floats. Returns false if no hop has run yet (in
    // which case the buffers are zeroed). Snapshots under m_outMutex;
    // uncontested in steady state.
    //
    // env       -- instantaneous amplitude after the two-time-constant
    //              envelope follower (~30 ms / ~100 ms), clipped to [0, 1].
    // phase     -- wrapped phase in [-pi, pi] of the latest analytic
    //              sample.
    // instFreq  -- instantaneous frequency in Hz, computed by phase
    //              differencing the latest two analytic samples
    //              (wrap-aware).
    bool fillBandStates(float* env, float* phase, float* instFreq);

    // The fixed log-spaced band centers (Hz). 8 elements, low to high.
    Q_INVOKABLE QVariantList bandCenters() const;

    // Offline drive path used by hilbertverify_cli. Feeds n mono samples
    // through the band pipelines in a single shot, then publishes the
    // final state. Internal filter state persists across calls so a
    // verification harness can drive multiple chunks and observe the
    // steady-state envelope.
    void debugPushMono(const float* mono, int n);

    // Reset all filter state to zero (useful between verification runs
    // to avoid bleed from a prior test signal).
    void debugReset();

    // Whether at least one hop has populated the output snapshot.
    bool haveBands() const { return m_haveBands.load(std::memory_order_acquire); }

signals:
    void audioSourceChanged();
    // Emitted at the end of each successful hop. Direct-connected
    // consumers (HilbertRosette) react synchronously on the GUI thread.
    void bandsUpdated();

private slots:
    void onFeaturesUpdated();

private:
    // Per-band biquad + Hilbert + envelope state.
    struct BandState {
        // RBJ biquad (Direct Form I) state.
        float b0 = 0.0f, b1 = 0.0f, b2 = 0.0f;
        float a1 = 0.0f, a2 = 0.0f;
        float x1 = 0.0f, x2 = 0.0f;       // input history
        float y1 = 0.0f, y2 = 0.0f;       // output history

        // 31-tap FIR Hilbert delay line. Stored as a circular buffer for
        // O(1) push; tapping uses a linear walk so we avoid a modulo
        // inside the inner loop (we copy into a flat scratch when the
        // ring wraps -- 31 floats, trivial).
        std::array<float, HILBERT_TAPS> ring {};
        int                             ringHead = 0;   // next-write index

        // Latest analytic sample (a single z = x + j*H[x]) and its
        // unwrapped phase from the previous hop (for instantaneous
        // frequency). We don't keep a full history of z -- we just need
        // the most-recent pair (n, n-1) to estimate dphi.
        float lastAnalyticRe = 0.0f;
        float lastAnalyticIm = 0.0f;
        float lastPhase      = 0.0f;     // [-pi, pi]
        bool  havePrevPhase  = false;

        // Envelope follower state (two-time-constant).
        float env            = 0.0f;
        float alphaAttack    = 0.0f;
        float alphaRelease   = 0.0f;

        // Hilbert-gain compensation: |H(omega)| of the windowed FIR is
        // not exactly 1.0 at every band center -- it rolls off near DC
        // and Nyquist. We precompute 1 / |H(2*pi*f0/fs)| at construction
        // so the published envelope reflects the true bandpassed
        // amplitude rather than the FIR's pass-band droop.
        float hilbertGain    = 1.0f;
    };

    AudioFeatures* m_source = nullptr;

    // Constants. m_h is the literal-baked 31-tap Hilbert kernel; m_fc
    // and m_bw are the per-band design points used to construct the
    // biquad coefficients at startup.
    static const std::array<float, HILBERT_TAPS> kHilbert;
    std::array<float, N_BANDS>                   m_fc {};   // center Hz
    std::array<float, N_BANDS>                   m_bw {};   // bandwidth Hz

    double                                       m_sampleRate = 44100.0;
    std::array<BandState, N_BANDS>               m_bands {};

    // GUI-thread pull buffer; reused across hops.
    std::vector<float>                           m_pullL;
    std::vector<float>                           m_pullR;
    std::vector<float>                           m_mono;

    // Output snapshot. Filled in processBlock, read by fillBandStates.
    QMutex                                       m_outMutex;
    std::array<float, N_BANDS>                   m_outEnv   {};
    std::array<float, N_BANDS>                   m_outPhase {};
    std::array<float, N_BANDS>                   m_outFreq  {};
    std::atomic<bool>                            m_haveBands { false };

    void buildBands();                       // populates m_fc/m_bw/coeffs
    void processBlock(const float* mono, int n);
};
