// Polar Spectrum — concentric audio-reactive rings drawn around a polar
// coordinate centred on screen. The first shader a non-musician sees in
// the ShaderToy pane, so it has to read at a glance: spectrum-as-flower,
// bass-as-pulse, treble-as-shimmer.
//
// Pipeline per pixel:
//   1. Convert to centred polar (r, theta).
//   2. For each band-ring (bass / mid / treble), sample iChannel0's
//      spectrum row at a band-localised slice of theta — bass occupies
//      the inner third of the spectrum (rows 0..170/512), mid the middle
//      third, treble the outer third. Each ring "blooms" outward by the
//      sampled magnitude, so a strong note in that band kicks the ring's
//      radius outward at the corresponding angle.
//   3. Soft-glow each ring via a smoothstep around its radius.
//   4. Tint each ring with a palette that rotates over iTime and is
//      tugged toward warmer/cooler by the centroid (spectral colour ↔
//      visual colour mapping).
//   5. Add a radial background gradient + a touch of film grain (hash
//      noise on screen-space + time) so the off-ring area doesn't read
//      as a flat black panel.
//
// Hash function: hash21 — a single-output 2D → 1D hash using the classic
// "fract(sin dot)" pattern (Dave Hoskins-style but kept short). Cheap,
// no branching, good enough for film-grain dither. NOT cryptographically
// random; do NOT use for procedural geometry.
//
// Palette: a cosine-based one (Inigo Quilez,
// https://iquilezles.org/articles/palettes/, CC0). a + b*cos(2*pi*(c*t+d)).

#version 440

#define TAU 6.28318530718

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

// 2D -> 1D hash. The .12345 / 78.233 constants are the classic ones from
// "Hash without Sine" discussions; the sin() is fine here because we only
// need cheap dither, not perfect uniformity.
float hash21(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

// IQ cosine palette. Returns a smooth, looping RGB triple for parameter t.
vec3 palette(float t, vec3 a, vec3 b, vec3 c, vec3 d) {
    return a + b * cos(TAU * (c * t + d));
}

// Sample the spectrum row (row 0 of iChannel0) at band-localised slice.
// startU/endU pick which third of the 512-bin spectrum belongs to this
// band. theta in [0, TAU) wraps around the ring.
float sampleBand(float theta, float startU, float endU) {
    float u = mix(startU, endU, theta / TAU);
    return texture(iChannel0, vec2(u, 0.0)).r;
}

void mainImage(out vec4 fc, in vec2 fc_input) {
    // Centred coords, scaled so the smaller screen dimension is ~1.
    vec2 uv = (2.0 * fc_input - iResolution.xy) / min(iResolution.x, iResolution.y);

    float r = length(uv);
    float theta = atan(uv.y, uv.x);
    if (theta < 0.0) theta += TAU;

    // ---------------- Background -----------------------------------------
    // Soft radial gradient: deep purple in the centre, near-black at the
    // edge. Slight RMS-driven brightness pulse so the whole frame breathes
    // with overall loudness.
    vec3 bg = mix(vec3(0.06, 0.04, 0.12), vec3(0.0), smoothstep(0.0, 1.2, r));
    bg *= 1.0 + 0.15 * rms;

    // ---------------- Rings ----------------------------------------------
    // Three rings — radii are stacked so they don't overlap at rest.
    // Each ring has a base radius and a band-driven "bloom" that pushes
    // the radius outward by up to ~0.18 units at full band energy.
    //
    //   bass:   inner third of spectrum, base radius 0.30
    //   mid:    middle third,            base radius 0.55
    //   treble: outer third,             base radius 0.80
    //
    // The radial sample at theta is what makes the ring jagged/spiky;
    // strong bins at that angle push the ring outward there.

    float bassSamp = sampleBand(theta, 0.00, 0.33);
    float midSamp  = sampleBand(theta, 0.33, 0.66);
    float trebSamp = sampleBand(theta, 0.66, 1.00);

    // bass squashes the radius — heavier basslines pull the whole flower
    // inward as they bloom outward; an interesting non-linear feel.
    float bassR = 0.30 + 0.18 * bassSamp - 0.04 * bass;
    float midR  = 0.55 + 0.16 * midSamp;
    float trebR = 0.80 + 0.14 * trebSamp;

    // Ring thickness scales with the env-followed band attack, so a kick
    // makes the bass ring fat for a moment then settles back.
    float bassW = 0.014 + 0.030 * bass;
    float midW  = 0.012 + 0.026 * mid;
    float trebW = 0.010 + 0.020 * treb;

    // Distance-to-ring smoothstep gives a 1-px AA ring outline plus a
    // wider glow halo. Glow falls off with 1/(d^2 + eps) for a phosphor-
    // ish feel.
    float bassD = abs(r - bassR);
    float midD  = abs(r - midR);
    float trebD = abs(r - trebR);

    float bassEdge = 1.0 - smoothstep(0.0, bassW,        bassD);
    float midEdge  = 1.0 - smoothstep(0.0, midW,         midD);
    float trebEdge = 1.0 - smoothstep(0.0, trebW,        trebD);

    float bassGlow = 0.05 / (bassD * bassD + 0.005);
    float midGlow  = 0.04 / (midD  * midD  + 0.005);
    float trebGlow = 0.03 / (trebD * trebD + 0.005);
    // Cap the glow contribution so a near-zero distance pixel doesn't
    // blow out to white.
    bassGlow = min(bassGlow, 4.0);
    midGlow  = min(midGlow,  4.0);
    trebGlow = min(trebGlow, 4.0);

    // ---------------- Palette --------------------------------------------
    // Rotates over time. Centroid biases the palette parameter t so a
    // bright-sounding piece sits warmer on the colour wheel. flux gives
    // a quick "snap" of palette shift on every onset.
    float t = iTime * 0.05 + 0.30 * cent + 0.15 * flux;
    vec3 paletteA = vec3(0.5, 0.5, 0.5);
    vec3 paletteB = vec3(0.5, 0.5, 0.5);
    vec3 paletteC = vec3(1.0, 1.0, 1.0);
    vec3 paletteD = vec3(0.00, 0.10, 0.20);

    vec3 cBass = palette(t,                 paletteA, paletteB, paletteC, paletteD);
    vec3 cMid  = palette(t + 0.33,          paletteA, paletteB, paletteC, paletteD);
    vec3 cTreb = palette(t + 0.66,          paletteA, paletteB, paletteC, paletteD);

    // Compose: edge contribution at saturated colour, glow contribution
    // at half intensity so it reads as a halo, not a second ring.
    vec3 col = bg;
    col += cBass * (bassEdge + 0.45 * bassGlow);
    col += cMid  * (midEdge  + 0.40 * midGlow);
    col += cTreb * (trebEdge + 0.35 * trebGlow);

    // ---------------- Film grain -----------------------------------------
    // Cheap hash-based dither. fragCoord + iTime keeps it animated; the
    // grain is barely visible (0.04 amplitude) so it doesn't fight the
    // rings, just kills banding in the bg gradient.
    float grain = hash21(fc_input + vec2(iTime * 7.0, iTime * 13.0));
    col += (grain - 0.5) * 0.04;

    // Mild vignette + final tonemap-ish curve.
    col *= 1.0 - 0.35 * smoothstep(0.6, 1.4, r);
    col = pow(max(col, 0.0), vec3(0.92));

    fc = vec4(col, 1.0);
}

void main() {
    // ShaderToy compatibility: write the body in mainImage, then have
    // main() forward to it. Pixel coordinates are derived from
    // qt_TexCoord0 since Qt's gl_FragCoord origin can vary across RHI
    // backends — qt_TexCoord0 is always [0,1]² with v flipped from
    // top to bottom.
    vec2 fragCoord = vec2(qt_TexCoord0.x, 1.0 - qt_TexCoord0.y) * iResolution.xy;
    vec4 outCol;
    mainImage(outCol, fragCoord);
    fragColor = outCol * qt_Opacity;
}
