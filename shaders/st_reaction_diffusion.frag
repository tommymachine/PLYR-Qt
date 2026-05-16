// Reaction-Diffusion (stateless approximation) — fake Gray-Scott-like
// spots-and-stripes patterns without a feedback buffer. Layer 4d (planned)
// implements the real Gray-Scott PDE via ping-pong textures; this shader
// is the standalone "looks similar" approximation that fits the Layer 3a
// budget of "one fragment shader, no extra render targets".
//
// Trick: combine three octaves of warped fBm noise where each octave's
// AMPLITUDE is gated by a frequency band. Then blend between two
// thresholding rules driven by spectral flux — low flux = "spots"
// regime (steep step), high flux = "stripes" regime (broader band).
// The visual signature reads as a true reaction-diffusion field
// converging/diverging with the music, even though no PDE is solved.
//
// Audio reactivity:
//   * bass  -> low-frequency / large-scale blobs.
//   * mid   -> mid-frequency / medium swirls.
//   * treb  -> high-frequency / fine speckle.
//   * flux  -> spots <-> stripes regime blend.
//   * iTime -> field drift along (x, y).
//
// Hash function: shared hash21 (fract(sin(dot))-style 2D->1D hash).
//
// CAVEAT: This is NOT a Gray-Scott reaction-diffusion simulation. True
// Gray-Scott requires:
//     u_{t+1} = u_t + Du * laplacian(u) - u*v^2 + F*(1-u)
//     v_{t+1} = v_t + Dv * laplacian(v) + u*v^2 - (F+k)*v
// which is fundamentally a feedback PDE — each frame depends on the
// previous frame's u and v fields. Implementing that in QML/RHI needs
// a ping-pong pair of render targets, which is Layer 4d territory.

#version 440

layout(location = 0) in  vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4  qt_Matrix;
    float qt_Opacity;
    float iTime;
    vec3  iResolution;
    vec4  iMouse;
    float bass;
    float mid;
    float treb;
    float rms;
    float cent;
    float flux;
};

layout(binding = 1) uniform sampler2D iChannel0;

float hash21(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = hash21(i + vec2(0.0, 0.0));
    float b = hash21(i + vec2(1.0, 0.0));
    float c = hash21(i + vec2(0.0, 1.0));
    float d = hash21(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

float fbm(vec2 p) {
    float v = 0.0;
    float a = 0.5;
    for (int i = 0; i < 4; ++i) {   // 4 octaves; the band gating gives
                                    // us additional spectral richness so
                                    // we don't need a 5th here.
        v += a * noise(p);
        p *= 2.0;
        a *= 0.5;
    }
    return v;
}

// Palette: cosine palette in the magenta-cyan range so the patterns
// read as biological / chemical rather than landscape.
vec3 palette(float t) {
    const float TAU = 6.28318530718;
    vec3 a = vec3(0.45, 0.30, 0.55);
    vec3 b = vec3(0.55, 0.55, 0.45);
    vec3 c = vec3(1.00, 1.00, 1.00);
    vec3 d = vec3(0.00, 0.20, 0.50);
    return a + b * cos(TAU * (c * t + d));
}

void mainImage(out vec4 fc, in vec2 fc_input) {
    vec2 uv = (2.0 * fc_input - iResolution.xy) / iResolution.y;

    // Apply a slow rotation to the coords so the field doesn't read
    // as "noise on a stationary grid". 0.04 rad/s — barely perceptible
    // but enough to keep the pattern alive during quiet passages.
    float rot = 0.04 * iTime;
    float cs = cos(rot), sn = sin(rot);
    vec2 ruv = mat2(cs, -sn, sn, cs) * uv;

    // Three band-gated noise octaves. Each has its own scale and drift
    // direction so they don't all sweep the same way.
    float p = 0.0;
    p += (0.10 + 0.90 * bass) * fbm(ruv * 1.0 + vec2(0.50, 0.10) * iTime);
    p += (0.10 + 0.90 * mid ) * fbm(ruv * 4.0 + vec2(-0.30, 0.40) * iTime);
    p += (0.10 + 0.90 * treb) * fbm(ruv * 12.0 + vec2(0.20, -0.60) * iTime);

    // Normalise. The "0.10 + 0.90 * band" form keeps a baseline so
    // silence still has a visible (if dim) pattern; full energy
    // ramps each octave to its max amplitude.
    p /= (1.7 + 0.9 * (bass + mid + treb));

    // Spots vs stripes via a flux-driven blend. Spots regime is a
    // narrow threshold band (smoothstep 0.45..0.55 - very steep step);
    // stripes regime is a broader band on a shifted, scaled p
    // (smoothstep 0.30..0.70 with p*2 - 0.5 driving it - reads as
    // multiple "stripes" of cells).
    float spots   = smoothstep(0.45, 0.55, p);
    float stripes = smoothstep(0.30, 0.70, fract(p * 2.0 - 0.5 + 0.5));
    float pattern = mix(spots, stripes, clamp(flux * 2.0, 0.0, 1.0));

    // Outline: bright thin line where the pattern crosses 0.5. Reads
    // like the "membrane" edge of a Gray-Scott spot.
    float outline = exp(-pow(pattern - 0.5, 2.0) * 80.0);

    // Compose. Palette walks slowly over time so the field drifts
    // through colour space. Centroid bias adds a separate hue tug.
    float palT = pattern + 0.08 * iTime + 0.20 * cent;
    vec3 col = palette(palT) * (0.2 + 0.8 * pattern);
    col += outline * vec3(1.0, 0.9, 0.7) * (0.5 + 0.5 * rms);

    // Slight vignette + tonemap.
    float r = length(uv);
    col *= 1.0 - 0.20 * smoothstep(0.7, 1.5, r);
    col = pow(max(col, 0.0), vec3(0.95));

    fc = vec4(col, 1.0);
}

void main() {
    vec2 fragCoord = vec2(qt_TexCoord0.x, 1.0 - qt_TexCoord0.y) * iResolution.xy;
    vec4 outCol;
    mainImage(outCol, fragCoord);
    fragColor = outCol * qt_Opacity;
}
