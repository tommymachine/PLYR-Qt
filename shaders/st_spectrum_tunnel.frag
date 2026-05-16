// Spectrum Tunnel — sphere-traced cylindrical tunnel whose wall radius
// is modulated by the FFT magnitude sampled at the distance along the
// tunnel axis. Imagine flying through the centre of a cathedral whose
// walls breathe with the music.
//
// SDF construction (Inigo Quilez SDF reference, CC0:
//   https://iquilezles.org/articles/distfunctions/):
//
//     d(p) = length(p.xy) - radius(p.z)
//     radius(z) = baseR + fftAmp * texture(iChannel0, vec2(fract(z * k), 0)).r
//
// The camera moves along +z over time; the FFT is sampled at fract(z * k)
// so the wall pattern wraps once per tunnel "period". A beat (flux burst)
// adds a forward shove to the camera so kicks "push you down the tunnel".
//
// Performance:
//   * 48 max raymarch steps (well under the 64-step budget).
//   * Tunnel SDF is convex; convergence is fast even with FFT bumps.
//   * Two texture taps per step (one for distance, one for shading) is
//     acceptable; the tunnel SDF only reads the spectrum row, not the
//     waveform row.
//
// Hash function: shared hash21 (fract(sin(dot)) — same as other Layer 3a
// shaders) — used only for the background star field.

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

// Sample the spectrum row (row 0) using only the lower-half of bins so
// the wall pattern is dominated by musically prominent frequencies.
// The 0.0625 stride means one full spectrum sweep per ~16 units of z.
float wallSample(float z) {
    return texture(iChannel0, vec2(fract(z * 0.0625), 0.0)).r;
}

// Tunnel SDF. baseR collapses with bass (so kicks "punch in" the walls)
// and expands again when the kick decays.
float sdf(vec3 p) {
    float baseR = 0.95 - 0.20 * bass;
    float fftAmp = 0.25 + 0.10 * mid;
    float r = baseR + fftAmp * wallSample(p.z);
    return length(p.xy) - r;
}

// Palette walk over time + treble pulls toward magenta. Standard IQ
// cosine palette.
vec3 palette(float t) {
    const float TAU = 6.28318530718;
    vec3 a = vec3(0.5, 0.4, 0.5);
    vec3 b = vec3(0.5, 0.5, 0.5);
    vec3 c = vec3(1.0, 1.0, 1.0);
    vec3 d = vec3(0.0, 0.33 + 0.10 * treb, 0.67);
    return a + b * cos(TAU * (c * t + d));
}

void mainImage(out vec4 fc, in vec2 fc_input) {
    // NDC-ish ray. Aspect-correct horizontal FoV with a slight wide-
    // angle feel (1.2 vertical compress).
    vec2 ndc = (2.0 * fc_input - iResolution.xy) / iResolution.y;

    // Camera: rides forward along +z with a flux-burst impulse, and
    // gently sways laterally so the tunnel doesn't feel rigid.
    float camZ = 1.2 * iTime + 2.0 * flux;
    vec3 ro = vec3(0.10 * sin(iTime * 0.3),
                   0.10 * cos(iTime * 0.4),
                   camZ);

    // Ray direction. Twist (roll) the ray as we go down the tunnel so
    // the wall pattern spirals — far more interesting than a straight
    // cylinder. The twist rate is gentle; centroid pulls it faster on
    // bright-sounding music.
    float roll = 0.20 * iTime * (0.5 + cent);
    float cs = cos(roll), sn = sin(roll);
    mat2 R = mat2(cs, -sn, sn, cs);
    vec2 ndcR = R * ndc;
    vec3 rd = normalize(vec3(ndcR, 1.3));

    // -------- Sphere trace ----------------------------------------------
    // Inside-out tunnel: the SDF is positive INSIDE the tube (camera
    // sits inside the wall surface). To trace OUTWARD, we flip the sign
    // and look for d == 0 from negative side; equivalently, we march
    // along rd accumulating |d| and stop when d crosses zero.
    float t = 0.0;
    float hitDist = -1.0;
    for (int i = 0; i < 48; ++i) {
        vec3 p = ro + rd * t;
        float d = sdf(p);
        // We're inside the tube, so d is negative until we hit the wall.
        // March by |d| — robust for either sign.
        if (d > -0.001) {
            hitDist = t;
            break;
        }
        t += -d * 0.9;        // step proportional to |d|; 0.9 for safety
        if (t > 30.0) break;  // far plane
    }

    // -------- Shading ----------------------------------------------------
    vec3 col;
    if (hitDist > 0.0) {
        vec3 p = ro + rd * hitDist;

        // Cylindrical wall normal points inward (toward axis). For
        // shading we use the angle around the axis to drive the colour
        // and a fall-off in z for fog.
        float ang = atan(p.y, p.x);
        float sampleAtHit = wallSample(p.z);

        // Base tunnel colour: palette walk along z + angular ripple.
        float ringT = p.z * 0.08 + iTime * 0.05;
        vec3 base = palette(ringT + 0.10 * ang);

        // Highlight bright FFT bins: wherever the wall is bumped out
        // (sampleAtHit ~ 1), tint toward the hot end of the palette.
        base = mix(base, vec3(1.0, 0.85, 0.55), sampleAtHit * 0.6);

        // Fog: distance-based fade to black at the far end of the trace.
        float fog = 1.0 - smoothstep(0.0, 25.0, hitDist);

        // Beat punch: flux brightens the whole hit region momentarily.
        float beatGlow = 1.0 + 0.5 * flux;

        col = base * fog * beatGlow;
    } else {
        // Sky / background — a thin starfield through the open end.
        // hash21 on quantised ray direction gives stationary points; the
        // 0.998 threshold keeps it sparse.
        vec2 starUV = floor(rd.xy * 200.0);
        float st = hash21(starUV);
        col = vec3(0.02, 0.02, 0.05);
        col += step(0.998, st) * vec3(0.9, 0.9, 1.0);
    }

    // Final touch: subtle radial vignette + RMS pulse.
    float r = length(ndc);
    col *= 1.0 - 0.25 * smoothstep(0.6, 1.4, r);
    col *= 0.90 + 0.20 * rms;

    fc = vec4(col, 1.0);
}

void main() {
    vec2 fragCoord = vec2(qt_TexCoord0.x, 1.0 - qt_TexCoord0.y) * iResolution.xy;
    vec4 outCol;
    mainImage(outCol, fragCoord);
    fragColor = outCol * qt_Opacity;
}
