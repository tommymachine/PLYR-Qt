#include "eq_engine.h"

#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(__x86_64__) || defined(_M_X64)
  #include <xmmintrin.h>
  #include <pmmintrin.h>
#endif

#ifndef M_PI
  #define M_PI 3.14159265358979323846
#endif

#define EQ_MAX_CHANNELS       2
#define PARAM_SMOOTH_TAU_SEC  0.020
#define SOFT_CLIP_THRESHOLD   0.891   // ~ -1 dBFS

const double EQ_BAND_FREQUENCIES[EQ_NUM_BANDS] = {
    31.5, 63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0
};

typedef struct { double b0, b1, b2, a1, a2; } BiquadCoeffs;
typedef struct { double s1, s2;             } BiquadState;

struct EqEngine {
    double sample_rate;
    int    num_channels;

    _Atomic int    target_bypass;
    _Atomic double target_preamp_db;
    _Atomic double target_global_q;
    _Atomic double target_band_gain_db   [EQ_NUM_BANDS];
    _Atomic double target_band_q_override[EQ_NUM_BANDS];  // NaN = inherit global

    double smoothed_preamp_db;
    double smoothed_global_q;
    double smoothed_band_gain_db[EQ_NUM_BANDS];
    double smoothed_band_q      [EQ_NUM_BANDS];

    BiquadCoeffs coeffs[EQ_NUM_BANDS];
    BiquadState  state [EQ_MAX_CHANNELS][EQ_NUM_BANDS];
};

static inline double clampd(double x, double lo, double hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

static inline double db_to_linear(double db) {
    return pow(10.0, db / 20.0);
}

// Linear below ±threshold, tanh-smoothed above. Kicks in only near clipping.
static inline double soft_clip(double x) {
    const double t    = SOFT_CLIP_THRESHOLD;
    const double room = 1.0 - t;
    if (x >  t) return  (t + tanh(( x - t) / room) * room);
    if (x < -t) return -(t + tanh((-x - t) / room) * room);
    return x;
}

// Thread-local so each audio thread sets the flags exactly once.
static void ensure_denormal_flags(void) {
    static _Thread_local int done = 0;
    if (done) return;
    done = 1;
#if defined(__x86_64__) || defined(_M_X64)
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#elif defined(__aarch64__)
    uint64_t fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= (1ULL << 24);  // FPCR.FZ — flush denormals to zero
    __asm__ volatile("msr fpcr, %0" :: "r"(fpcr));
#endif
}

// RBJ peaking EQ. See docs/EQ_RESEARCH.md §2.3.
static void rbj_peaking(double Fs, double f0, double dBgain, double Q,
                        BiquadCoeffs* out) {
    double A     = pow(10.0, dBgain / 40.0);
    double w0    = 2.0 * M_PI * f0 / Fs;
    double cos_w = cos(w0);
    double sin_w = sin(w0);
    double alpha = sin_w / (2.0 * Q);

    double b0 =  1.0 + alpha * A;
    double b1 = -2.0 * cos_w;
    double b2 =  1.0 - alpha * A;
    double a0 =  1.0 + alpha / A;
    double a1 = -2.0 * cos_w;
    double a2 =  1.0 - alpha / A;

    double inv_a0 = 1.0 / a0;
    out->b0 = b0 * inv_a0;
    out->b1 = b1 * inv_a0;
    out->b2 = b2 * inv_a0;
    out->a1 = a1 * inv_a0;
    out->a2 = a2 * inv_a0;
}

static inline double effective_target_q(const EqEngine* e, int band) {
    double override = atomic_load(&e->target_band_q_override[band]);
    return isnan(override) ? atomic_load(&e->target_global_q) : override;
}

static void update_smoothed(EqEngine* e, size_t nframes) {
    double alpha = 1.0 - exp(-(double)nframes /
                             (PARAM_SMOOTH_TAU_SEC * e->sample_rate));

    double pre_t = atomic_load(&e->target_preamp_db);
    e->smoothed_preamp_db += (pre_t - e->smoothed_preamp_db) * alpha;

    double gq_t = atomic_load(&e->target_global_q);
    e->smoothed_global_q += (gq_t - e->smoothed_global_q) * alpha;

    for (int b = 0; b < EQ_NUM_BANDS; b++) {
        double g_t = atomic_load(&e->target_band_gain_db[b]);
        e->smoothed_band_gain_db[b] += (g_t - e->smoothed_band_gain_db[b]) * alpha;

        double q_t = effective_target_q(e, b);
        e->smoothed_band_q[b] += (q_t - e->smoothed_band_q[b]) * alpha;
    }
}

static void recompute_coeffs(EqEngine* e) {
    for (int b = 0; b < EQ_NUM_BANDS; b++) {
        rbj_peaking(e->sample_rate,
                    EQ_BAND_FREQUENCIES[b],
                    e->smoothed_band_gain_db[b],
                    e->smoothed_band_q[b],
                    &e->coeffs[b]);
    }
}

static void init_defaults(EqEngine* e) {
    atomic_store(&e->target_bypass, 0);
    atomic_store(&e->target_preamp_db, 0.0);
    atomic_store(&e->target_global_q, 1.0);
    for (int b = 0; b < EQ_NUM_BANDS; b++) {
        atomic_store(&e->target_band_gain_db[b], 0.0);
        atomic_store(&e->target_band_q_override[b], (double)NAN);
    }
    e->smoothed_preamp_db = 0.0;
    e->smoothed_global_q  = 1.0;
    for (int b = 0; b < EQ_NUM_BANDS; b++) {
        e->smoothed_band_gain_db[b] = 0.0;
        e->smoothed_band_q[b]       = 1.0;
    }
    recompute_coeffs(e);
    memset(e->state, 0, sizeof(e->state));
}

EqEngine* eq_create(double sample_rate, int num_channels) {
    if (num_channels < 1 || num_channels > EQ_MAX_CHANNELS) return NULL;
    if (sample_rate < 8000.0 || sample_rate > 384000.0)     return NULL;

    EqEngine* e = (EqEngine*)calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->sample_rate  = sample_rate;
    e->num_channels = num_channels;
    init_defaults(e);
    return e;
}

void eq_destroy(EqEngine* e) { free(e); }

void eq_set_sample_rate(EqEngine* e, double sample_rate) {
    if (!e || sample_rate < 8000.0 || sample_rate > 384000.0) return;
    e->sample_rate = sample_rate;
    recompute_coeffs(e);
    memset(e->state, 0, sizeof(e->state));
}

void eq_reset(EqEngine* e) {
    if (!e) return;
    memset(e->state, 0, sizeof(e->state));
}

void eq_set_bypass(EqEngine* e, int bypass) {
    if (!e) return;
    atomic_store(&e->target_bypass, bypass ? 1 : 0);
}

void eq_set_preamp_db(EqEngine* e, double db) {
    if (!e) return;
    atomic_store(&e->target_preamp_db, clampd(db, -12.0, 12.0));
}

void eq_set_band_gain_db(EqEngine* e, int band, double db) {
    if (!e || band < 0 || band >= EQ_NUM_BANDS) return;
    atomic_store(&e->target_band_gain_db[band], clampd(db, -12.0, 12.0));
}

void eq_set_global_q(EqEngine* e, double q) {
    if (!e) return;
    atomic_store(&e->target_global_q, clampd(q, 0.5, 2.5));
}

void eq_set_band_q(EqEngine* e, int band, double q) {
    if (!e || band < 0 || band >= EQ_NUM_BANDS) return;
    atomic_store(&e->target_band_q_override[band], clampd(q, 0.3, 4.0));
}

void eq_clear_band_q(EqEngine* e, int band) {
    if (!e || band < 0 || band >= EQ_NUM_BANDS) return;
    atomic_store(&e->target_band_q_override[band], (double)NAN);
}

void eq_apply_target(EqEngine* e,
                     const double band_gains_db[EQ_NUM_BANDS],
                     double preamp_db) {
    if (!e || !band_gains_db) return;
    for (int b = 0; b < EQ_NUM_BANDS; b++)
        atomic_store(&e->target_band_gain_db[b],
                     clampd(band_gains_db[b], -12.0, 12.0));
    atomic_store(&e->target_preamp_db, clampd(preamp_db, -12.0, 12.0));
}

void eq_process(EqEngine* e, const float* in, float* out, size_t nframes) {
    if (!e || !in || !out || nframes == 0) return;

    ensure_denormal_flags();

    if (atomic_load(&e->target_bypass)) {
        if (in != out)
            memcpy(out, in, nframes * (size_t)e->num_channels * sizeof(float));
        return;
    }

    update_smoothed(e, nframes);
    recompute_coeffs(e);

    const double preamp = db_to_linear(e->smoothed_preamp_db);
    const int    nch    = e->num_channels;

    for (size_t n = 0; n < nframes; n++) {
        for (int c = 0; c < nch; c++) {
            double x = (double)in[n * nch + c] * preamp;
            for (int b = 0; b < EQ_NUM_BANDS; b++) {
                const BiquadCoeffs* k = &e->coeffs[b];
                BiquadState*        s = &e->state[c][b];
                double y  = k->b0 * x + s->s1;
                s->s1 = k->b1 * x - k->a1 * y + s->s2;
                s->s2 = k->b2 * x - k->a2 * y;
                x = y;
            }
            out[n * nch + c] = (float)soft_clip(x);
        }
    }
}
