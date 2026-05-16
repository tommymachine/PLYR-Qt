// Waveform Heightmap — the 512-sample waveform row of iChannel0 is read
// as a 1D heightfield, then mapped onto a 3D landscape that the camera
// sweeps across. Each pixel on screen is a ray that intersects the
// landscape via a 2.5D height-march (cheap, no SDFs).
//
// The trick that makes this audio-reactive without a feedback buffer:
// rather than building a 2D heightmap of past frames, we tile the
// current waveform across the world's z (forward) axis, with a phase
// offset that drifts at a constant speed so older "ridges" appear to
// recede into the distance. This produces the illusion of "audio
// history rolling away from camera" using only the current frame.
//
// Audio reactivity:
//   * waveform itself drives the height. iChannel0 row 1 is signed-
//     centred around 128/255 = 0.5, so we subtract 0.5 and scale.
//   * bass        -> vertical exaggeration of the ridges.
//   * treb        -> sharpness of the ridge edges (smoothstep width).
//   * centroid    -> palette warm/cool bias.
//   * flux        -> momentary forward shove of the camera (same trick
//                    as the spectrum tunnel).
//
// Performance: a 2.5D height-march at ~32 steps along the ray's xz path,
// computing the analytic height at each step. No noise calls. Total
// cost is ~32 texture taps per pixel — well under any reasonable budget.
//
// Source attribution: 2.5D landscape style after Inigo Quilez's terrain
// articles (https://iquilezles.org/articles/terrainmarching/, CC0). The
// height function here is much simpler (1D from a texture, not procedural
// noise).

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

// Height at world (x, z). x indexes into the 512-sample waveform; z
// gives a phase offset so the camera-roll direction reveals "history".
// We multiply z by a phase factor 0.10 — 1 unit of z = 0.10 wavelength
// of waveform scan.
float heightAt(vec2 xz) {
    // Stretch x across [-1, 1] -> [0, 1]; then add z-driven phase.
    float u = 0.5 * (xz.x + 1.0) + 0.10 * xz.y;
    u = fract(u);
    // Waveform row of iChannel0 is row 1 — sample at v=1.0 (top row).
    // The R channel comes back in [0,1] with 0.5 = silence.
    float w = texture(iChannel0, vec2(u, 1.0)).r - 0.5;   // [-0.5, 0.5]
    // Bass exaggerates ridge amplitude; treb sharpens the ridge tops
    // via a soft tanh-ish curve.
    float amp = 0.40 + 0.60 * bass;
    float sharpen = mix(1.0, 1.8, treb);
    return amp * tanh(sharpen * 2.0 * w);
}

// Cheap two-tap finite-difference normal. eps tuned so the normal is
// stable at the height-march step size we use.
vec3 heightNormal(vec2 xz) {
    float eps = 0.015;
    float hL = heightAt(xz - vec2(eps, 0.0));
    float hR = heightAt(xz + vec2(eps, 0.0));
    float hD = heightAt(xz - vec2(0.0, eps));
    float hU = heightAt(xz + vec2(0.0, eps));
    return normalize(vec3(hL - hR, 2.0 * eps, hD - hU));
}

vec3 palette(float t) {
    const float TAU = 6.28318530718;
    // Centroid biases the palette phase so brighter signals run warmer.
    vec3 a = vec3(0.50, 0.45, 0.50);
    vec3 b = vec3(0.50, 0.55, 0.50);
    vec3 c = vec3(1.00, 1.00, 1.00);
    vec3 d = vec3(0.10, 0.20 + 0.30 * cent, 0.60);
    return a + b * cos(TAU * (c * t + d));
}

void mainImage(out vec4 fc, in vec2 fc_input) {
    vec2 ndc = (2.0 * fc_input - iResolution.xy) / iResolution.y;

    // ---- Camera --------------------------------------------------------
    // Slow forward sweep along +z. flux gives an instantaneous shove on
    // every onset, same trick the tunnel uses.
    float camZ = -1.5 - 0.30 * iTime - 0.4 * flux;
    vec3 ro = vec3(0.0, 0.85, camZ);
    // Camera pitches slightly down so the landscape recedes naturally.
    vec3 rd = normalize(vec3(ndc.x, ndc.y - 0.35, 1.0));

    // ---- Height-march --------------------------------------------------
    // Step along the ray in xz; at each step compare ray-y to the
    // surface height. When the ray dips below the surface, we've hit.
    float tHit = -1.0;
    vec3 hitP = vec3(0.0);

    // Stride along the ray. Adaptive — bigger steps far away, smaller
    // close in (similar to iquilezles terrain marching).
    float t = 0.0;
    for (int i = 0; i < 32; ++i) {
        vec3 p = ro + rd * t;
        float h = heightAt(p.xz);
        if (p.y < h) {
            tHit = t;
            hitP = p;
            break;
        }
        // Stride grows with distance to amortise far-range traversal.
        t += 0.10 + 0.04 * t;
        if (t > 12.0) break;
    }

    vec3 col;
    if (tHit > 0.0) {
        vec3 n = heightNormal(hitP.xz);

        // Directional sun light coming from upper-left. Standard Lambert
        // with a touch of ambient.
        vec3 lightDir = normalize(vec3(-0.6, 0.7, 0.3));
        float diff = max(0.0, dot(n, lightDir));
        vec3 amb = vec3(0.10, 0.08, 0.15);

        // Surface colour: palette walk along z (so each ridge is its
        // own colour band), modulated by height itself so peaks read
        // brighter than troughs.
        float palT = hitP.z * 0.10 + 0.05 * iTime;
        vec3 surface = palette(palT);
        surface *= 0.6 + 0.6 * smoothstep(-0.4, 0.4, heightAt(hitP.xz));

        // Distance fog so the far horizon dissolves into the sky.
        float fog = 1.0 - smoothstep(2.0, 11.0, tHit);

        col = (amb + diff * surface) * fog;

        // RMS pulse on the lit side.
        col *= 0.90 + 0.25 * rms;
    } else {
        // Sky: gradient from horizon (warm) to zenith (cool). The
        // horizon line is mid-ray; ndc.y already gives us a useful
        // height proxy.
        float skyY = clamp(0.5 - 0.5 * ndc.y, 0.0, 1.0);
        col = mix(vec3(0.35, 0.20, 0.30),   // horizon
                  vec3(0.05, 0.04, 0.12),   // zenith
                  skyY);
        // Subtle treble glow at the horizon so highs read as "light
        // pouring through" the landscape.
        col += treb * 0.20 * vec3(1.0, 0.6, 0.4)
                    * smoothstep(0.0, 0.30, 1.0 - skyY);
    }

    // Final overall tonemap-ish curve.
    col = pow(max(col, 0.0), vec3(0.95));

    fc = vec4(col, 1.0);
}

void main() {
    vec2 fragCoord = vec2(qt_TexCoord0.x, 1.0 - qt_TexCoord0.y) * iResolution.xy;
    vec4 outCol;
    mainImage(outCol, fragCoord);
    fragColor = outCol * qt_Opacity;
}
