// Domain Warp — fBm warped by another fBm, warped again by a third.
// Inigo Quilez's classic recipe from
//   https://iquilezles.org/articles/warp/      (CC0)
//
// Audio reactivity:
//   * bass     -> warp amplitude. Heavier bass = more violent distortion
//                 of the noise field; quiet passages settle into smooth
//                 fBm.
//   * treb     -> palette hue shift. Bright signal pulls the colour
//                 toward warm orange; dark signal sits in cool purple.
//   * flux     -> small uniform warp amplitude kick on every onset.
//   * iTime    -> slow base drift so even silence has motion.
//
// Hash function: hash21 — single-output 2D->1D hash, fract(sin(dot))-style.
// Good enough for value noise; not cryptographically random.
//
// Performance: fBm uses 5 octaves; total cost is ~3 fBm calls per pixel
// (for q.xy, r.xy, f) -> ~15 noise evaluations -> ~75 hash21 calls. Each
// hash21 is one sin + dot + fract; well under the 64-iter budget the
// spec calls out.

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

// 2D value noise. Bilinear interpolation between four hash21 corners of
// the integer lattice that contains p. Smoothstep on the fractional part
// gives a C1-continuous result (good enough for fBm without ringing).
float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);   // smoothstep curve
    float a = hash21(i + vec2(0.0, 0.0));
    float b = hash21(i + vec2(1.0, 0.0));
    float c = hash21(i + vec2(0.0, 1.0));
    float d = hash21(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// Standard 5-octave fBm. Each octave doubles frequency, halves amplitude.
// 5 octaves is a budget call: enough detail to look "natural", cheap
// enough to run multiple times per pixel.
float fbm(vec2 p) {
    float v = 0.0;
    float a = 0.5;
    for (int i = 0; i < 5; ++i) {
        v += a * noise(p);
        p *= 2.0;
        a *= 0.5;
    }
    return v;
}

void mainImage(out vec4 fc, in vec2 fc_input) {
    // Aspect-correct centred coords. iquilezles uses this normalisation
    // in all his warp examples.
    vec2 uv = (2.0 * fc_input - iResolution.xy) / iResolution.y;

    // Warp amplitude — base 0.4 so silence still distorts gently;
    // bass and flux stack on top.
    float warpAmt = 0.4 + 0.6 * bass + 0.25 * flux;

    // First-pass warp field. iTime drift so the whole image flows even
    // when audio is steady.
    vec2 q = vec2(fbm(uv + 0.3 * iTime),
                  fbm(uv + vec2(5.7, 1.3)));

    // Second-pass warp uses q as a domain offset. The fixed magic
    // numbers (1.7, 9.2 / 8.3, 2.8) are from Quilez's original — they
    // break the symmetry that would otherwise make r.x and r.y read
    // as copies of each other.
    vec2 r = vec2(fbm(uv + 4.0 * q + vec2(1.7, 9.2) + 0.2 * iTime),
                  fbm(uv + 4.0 * q + vec2(8.3, 2.8) + 0.3 * iTime));

    // Final field — bass-modulated warp amplitude on r.
    float f = fbm(uv + 4.0 * r * warpAmt);

    // ---------------- Palette --------------------------------------------
    // Three-stop gradient between deep purple (cold), warm yellow-orange
    // (mid), and a final pastel pull. Treble pushes the second stop's
    // hue toward red.
    vec3 cold = vec3(0.10, 0.05, 0.30);
    vec3 warm = mix(vec3(1.00, 0.70, 0.20),
                    vec3(1.00, 0.30, 0.20),
                    treb);
    vec3 pastel = vec3(0.80, 0.90, 1.00);

    vec3 col = mix(cold, warm, f);
    col = mix(col, pastel, q.x);

    // Subtle vignette from r.x — the warp field doubles as a luminance
    // mask, dimming areas where r.x is low. Reads like depth without
    // any actual depth math.
    col *= 0.5 + 0.5 * r.x;

    // RMS-driven overall brightness so the panel breathes with loudness.
    col *= 0.85 + 0.30 * rms;

    fc = vec4(col, 1.0);
}

void main() {
    vec2 fragCoord = vec2(qt_TexCoord0.x, 1.0 - qt_TexCoord0.y) * iResolution.xy;
    vec4 outCol;
    mainImage(outCol, fragCoord);
    fragColor = outCol * qt_Opacity;
}
