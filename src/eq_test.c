// eq_test.c — standalone test harness for eq_engine.
//
// Generates an impulse response, then evaluates |H(f)| at each ISO band
// center via a direct DFT bin. No external dependencies.
//
// Build:
//   clang -O2 -std=c11 -Wall -Wextra src/eq_engine.c src/eq_test.c -lm -o eq_test
//   ./eq_test

#include "eq_engine.h"
#include "eq_presets.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
  #define M_PI 3.14159265358979323846
#endif

#define SR          48000.0
#define IR_LEN      16384
#define IMPULSE_AMP 0.1f      // low enough that boosted peaks stay under soft-clip

static double mag_db_at(const float* ir, size_t n, double sr, double f) {
    double w = 2.0 * M_PI * f / sr;
    double re = 0.0, im = 0.0;
    for (size_t i = 0; i < n; i++) {
        re += (double)ir[i] * cos(w * (double)i);
        im -= (double)ir[i] * sin(w * (double)i);
    }
    return 20.0 * log10(sqrt(re*re + im*im) + 1e-30);
}

// Settle smoothers at current targets, then capture the impulse response.
static void capture_ir(EqEngine* e, float* ir, size_t n) {
    float zeros[256] = {0};
    for (int i = 0; i < 512; i++) eq_process(e, zeros, zeros, 256);
    eq_reset(e);

    float impulse_in  = IMPULSE_AMP;
    float impulse_out = 0.0f;
    eq_process(e, &impulse_in, &impulse_out, 1);
    ir[0] = impulse_out;

    float z_in = 0.0f;
    for (size_t i = 1; i < n; i++) {
        float y = 0.0f;
        eq_process(e, &z_in, &y, 1);
        ir[i] = y;
    }
    // Normalize out the impulse amplitude so mag_db_at reports response in dB
    // relative to a unity impulse (0 dB = flat).
    for (size_t i = 0; i < n; i++) ir[i] /= IMPULSE_AMP;
}

static int approx(double a, double b, double tol) {
    return fabs(a - b) <= tol;
}

static int test_flat(void) {
    printf("\n[flat]  all bands 0 dB, preamp 0\n");
    EqEngine* e = eq_create(SR, 1);
    float ir[IR_LEN];
    capture_ir(e, ir, IR_LEN);
    int ok = 1;
    for (int b = 0; b < EQ_NUM_BANDS; b++) {
        double db = mag_db_at(ir, IR_LEN, SR, EQ_BAND_FREQUENCIES[b]);
        if (!approx(db, 0.0, 0.15)) ok = 0;
        printf("  %7.1f Hz  %+6.2f dB\n", EQ_BAND_FREQUENCIES[b], db);
    }
    eq_destroy(e);
    printf("  %s\n", ok ? "PASS" : "FAIL (expected ~0 dB across)");
    return ok;
}

static int test_single_band(int band, double gain, double tol) {
    printf("\n[single] band %d (%.1f Hz) %+0.1f dB\n",
           band, EQ_BAND_FREQUENCIES[band], gain);
    EqEngine* e = eq_create(SR, 1);
    eq_set_band_gain_db(e, band, gain);
    float ir[IR_LEN];
    capture_ir(e, ir, IR_LEN);
    double at_center = mag_db_at(ir, IR_LEN, SR, EQ_BAND_FREQUENCIES[band]);
    int ok = approx(at_center, gain, tol);
    for (int b = 0; b < EQ_NUM_BANDS; b++) {
        double db = mag_db_at(ir, IR_LEN, SR, EQ_BAND_FREQUENCIES[b]);
        const char* mark = (b == band) ? " <-- target" : "";
        printf("  %7.1f Hz  %+6.2f dB%s\n", EQ_BAND_FREQUENCIES[b], db, mark);
    }
    eq_destroy(e);
    printf("  %s (target band saw %+.2f dB, expected %+.1f ±%.1f)\n",
           ok ? "PASS" : "FAIL", at_center, gain, tol);
    return ok;
}

static int test_classical_preset(void) {
    // Informational: prints the actual frequency response curve for the
    // Classical preset. Individual band accuracy is verified in the single-
    // band tests; this one just sanity-checks the overall shape and that no
    // NaN/inf appears. At Q=1, adjacent cut bands overlap significantly —
    // the skirts of -7.2 dB cuts at 2k/4k/8k drag nearby "flat" bands down
    // too. That's the intended physics (research §6.2), not a filter bug.
    printf("\n[preset] Classical: 0 0 0 0 0 0 -7.2 -7.2 -7.2 -9.6 (informational)\n");
    EqEngine* e = eq_create(SR, 1);
    double classical[EQ_NUM_BANDS] = {
        0, 0, 0, 0, 0, 0, -7.2, -7.2, -7.2, -9.6
    };
    eq_apply_target(e, classical, 0.0);
    float ir[IR_LEN];
    capture_ir(e, ir, IR_LEN);
    int ok = 1;
    for (int b = 0; b < EQ_NUM_BANDS; b++) {
        double db = mag_db_at(ir, IR_LEN, SR, EQ_BAND_FREQUENCIES[b]);
        if (!isfinite(db)) ok = 0;
        printf("  %7.1f Hz  %+6.2f dB   (slider %+.1f)\n",
               EQ_BAND_FREQUENCIES[b], db, classical[b]);
    }
    eq_destroy(e);
    printf("  %s (curve finite; interior cut-chain reads deeper than slider "
           "— expected Q=1 overlap)\n", ok ? "PASS" : "FAIL");
    return ok;
}

static int test_bypass(void) {
    printf("\n[bypass] must be bit-identical passthrough\n");
    EqEngine* e = eq_create(SR, 1);
    for (int b = 0; b < EQ_NUM_BANDS; b++)
        eq_set_band_gain_db(e, b, (b % 2) ? 12.0 : -12.0);
    eq_set_preamp_db(e, 6.0);
    eq_set_bypass(e, 1);

    float in[64], out[64];
    for (int i = 0; i < 64; i++) in[i] = (float)(i % 11) * 0.07f - 0.3f;
    eq_process(e, in, out, 64);

    int ok = 1;
    for (int i = 0; i < 64; i++)
        if (in[i] != out[i]) { ok = 0; break; }
    eq_destroy(e);
    printf("  %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

static int test_low_freq_stability(void) {
    // 31.5 Hz at 192 kHz is the hardest case for single-precision biquads.
    printf("\n[stability] 31.5 Hz +12 dB at 192 kHz (high-Q low-f torture)\n");
    EqEngine* e = eq_create(192000.0, 1);
    eq_set_band_gain_db(e, 0, 12.0);
    eq_set_global_q(e, 2.5);
    float ir[IR_LEN];
    capture_ir(e, ir, IR_LEN);
    // Look for NaN or runaway.
    int ok = 1;
    double peak = 0.0;
    for (size_t i = 0; i < IR_LEN; i++) {
        if (!isfinite(ir[i])) { ok = 0; break; }
        if (fabs(ir[i]) > peak) peak = fabs(ir[i]);
    }
    eq_destroy(e);
    printf("  %s (peak |ir| = %.3f, all finite)\n", ok ? "PASS" : "FAIL", peak);
    return ok;
}

static int test_preset_table(void) {
    printf("\n[presets] Built-in preset table\n");
    const char* expect[] = { "flat", "classical", "jazz", "modern", "vinyl" };
    int expect_n = (int)(sizeof(expect)/sizeof(expect[0]));
    int ok = (eq_preset_count() == expect_n);
    if (!ok) printf("  FAIL: count=%d, want=%d\n", eq_preset_count(), expect_n);
    for (int i = 0; i < expect_n; i++) {
        const EqPreset* p = eq_preset_by_id(expect[i]);
        if (!p) { ok = 0; printf("  MISS: %s\n", expect[i]); continue; }
        printf("  %-10s  preamp %+4.1f  gains [",
               p->display_name, p->preamp_db);
        for (int b = 0; b < EQ_NUM_BANDS; b++)
            printf("%+5.1f%s", p->band_gains_db[b], b < EQ_NUM_BANDS-1 ? "," : "");
        printf("]\n");
    }
    // Unknown id should return NULL.
    if (eq_preset_by_id("nonesuch") != NULL) {
        ok = 0; printf("  FAIL: unknown id returned non-NULL\n");
    }
    printf("  %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

int main(void) {
    int passed = 0, total = 0;
    #define RUN(t) do { total++; if (t) passed++; } while (0)

    RUN(test_flat());
    RUN(test_single_band(5, +6.0, 0.4));   // 1 kHz
    RUN(test_single_band(0, +6.0, 0.5));   // 31.5 Hz — low-freq precision
    RUN(test_single_band(9, -6.0, 0.5));   // 16 kHz — near Nyquist
    RUN(test_classical_preset());
    RUN(test_bypass());
    RUN(test_low_freq_stability());
    RUN(test_preset_table());

    printf("\n=== %d / %d tests passed ===\n", passed, total);
    return (passed == total) ? 0 : 1;
}
