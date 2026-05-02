// eq_engine.h — 10-band graphic EQ for Concerto.
//
// Pure C, real-time safe. The audio thread calls eq_process(); the UI
// thread writes targets via eq_set_* functions. Parameters are smoothed
// over ~20 ms and coefficients are recomputed at block rate.
//
// Design reference: docs/EQ_RESEARCH.md
//   - RBJ peaking biquads, Transposed Direct Form II, double state
//   - ISO octave centers: 31.5, 63, 125, 250, 500, 1k, 2k, 4k, 8k, 16k
//   - ±12 dB per band, ±12 dB preamp, Q ∈ [0.5, 2.5] global / [0.3, 4.0] per-band
//   - FTZ/DAZ set on first audio-thread call
//   - tanh soft-clip safety net on output

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EQ_NUM_BANDS 10

extern const double EQ_BAND_FREQUENCIES[EQ_NUM_BANDS];

typedef struct EqEngine EqEngine;

// Lifecycle
EqEngine* eq_create        (double sample_rate, int num_channels);
void      eq_destroy       (EqEngine*);
void      eq_set_sample_rate(EqEngine*, double sample_rate);
void      eq_reset         (EqEngine*);

// Parameter control (UI thread → atomic store → audio thread reads).
// All inputs are clamped internally.
void      eq_set_bypass      (EqEngine*, int bypass);
void      eq_set_preamp_db   (EqEngine*, double db);
void      eq_set_band_gain_db(EqEngine*, int band, double db);
void      eq_set_global_q    (EqEngine*, double q);
void      eq_set_band_q      (EqEngine*, int band, double q);
void      eq_clear_band_q    (EqEngine*, int band);

void      eq_apply_target    (EqEngine*,
                              const double band_gains_db[EQ_NUM_BANDS],
                              double preamp_db);

// Audio thread. In-place safe (in == out). Interleaved float samples.
void      eq_process         (EqEngine*,
                              const float* in,
                              float*       out,
                              size_t       nframes);

#ifdef __cplusplus
}
#endif
