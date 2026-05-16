// Fragment stage for the analytic-line-integral scope.
//
// For each segment quad, evaluates m1el's woscope closed-form integral
// of a Gaussian beam along the segment (see m1el.github.io/woscope-how).
// The 1/L factor encodes beam-velocity modulation — short segments (slow
// signal motion) appear brighter, long segments dimmer, just like a CRT
// phosphor that's been excited by a slow vs. fast electron beam.
//
//   intensity = (1/L) · (erf((L-t)/σ) - erf(-t/σ)) · exp(-d²/(2σ²))
//
// `erf` is the Abramowitz-Stegun 7.1.26 polynomial approximation —
// constant cost, ≤ 1.5e-7 max error, branch-free apart from the sign.
//
// Output is RGBA premultiplied by `intensity`. Additive blending (One,
// One) accumulates segments onto the persistent buffer.

#version 440

layout(location = 0) in  vec2 v_pos;
layout(location = 1) in  vec2 v_p0;
layout(location = 2) in  vec2 v_p1;
layout(location = 3) in float v_segLen;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4  qt_Matrix;
    float qt_Opacity;
    vec2  viewportPx;
    float sigma;
    float intensity;
};

float erfApprox(float x) {
    // Abramowitz-Stegun 7.1.26
    const float a1 =  0.254829592;
    const float a2 = -0.284496736;
    const float a3 =  1.421413741;
    const float a4 = -1.453152027;
    const float a5 =  1.061405429;
    const float p  =  0.3275911;
    float s = x < 0.0 ? -1.0 : 1.0;
    x = abs(x);
    float t = 1.0 / (1.0 + p * x);
    float y = 1.0 - (((((a5 * t + a4) * t) + a3) * t + a2) * t + a1) * t
                  * exp(-x * x);
    return s * y;
}

void main() {
    // Degenerate segments (two identical samples) collapse to a point;
    // approximate with a single isotropic Gaussian centered at p0.
    if (v_segLen <= 1e-4) {
        vec2 d = v_pos - v_p0;
        float g = exp(-dot(d, d) / (2.0 * sigma * sigma)) / max(1e-4, sigma);
        float val = intensity * g * 0.25;
        fragColor = vec4(val, val, val, val);
        return;
    }

    vec2  rel     = v_pos - v_p0;
    vec2  tangent = (v_p1 - v_p0) / v_segLen;
    float along   = dot(rel, tangent);            // signed projection
    vec2  perp    = rel - along * tangent;
    float dPerp   = length(perp);                 // distance to segment line

    float invSigma = 1.0 / max(1e-4, sigma);
    float lineW    = exp(-(dPerp * dPerp) * 0.5 * invSigma * invSigma);
    float endsW    = erfApprox((v_segLen - along) * invSigma)
                   - erfApprox((-along)            * invSigma);
    float val      = (intensity / v_segLen) * endsW * lineW;
    val = max(val, 0.0);

    // Premultiplied-alpha output. Color tint is applied by the host
    // pipeline via a blend constant or in the compositing step — here
    // we emit pure white scaled by intensity, the compositing pass
    // multiplies by beamColor.
    fragColor = vec4(val, val, val, val);
}
