// Janata-style 3D tonal torus, raymarched SDF (Layer 2b).
//
// The 24 major/minor keys live on a 2D torus surface:
//   u-axis (around the big ring) cycles through the 12 keys in
//     circle-of-fifths order: C, G, D, A, E, B, F#, C#, G#, D#, A#, F.
//     The major-root -> u mapping is u = ((root * 7) mod 12) / 12.
//     The inverse: root = (fifthIndex * 7) mod 12 (because 7*7 = 49 = 1
//     mod 12, so 7 is self-inverse).
//   v-axis (around the tube cross-section) is the mode axis: v = 0 for
//     major, v = 0.5 for minor. Minor's hemisphere sits on the opposite
//     side of the tube; the loop closes through unused (no-key) space.
//
// Render path:
//   1. Build a ray for each pixel from an orbiting eye through a fixed
//      look-at at the origin (orbit = 1 rev / 30 s).
//   2. Sphere-trace the torus SDF, max 64 steps, hit threshold 0.001.
//   3. At the hit point, compute (u, v), shade by:
//        - viridis-like gradient over v (major hemisphere warm, minor
//          cool) for the surface base color.
//        - additive glow around the smoothed weighted-centroid
//          (centroidU, centroidV) with Gaussian falloff.
//        - smaller secondary glows at the top-3 individual key
//          positions (topKey0..3) scaled by their softmax weight.
//        - faint meridian lines at the 12 fifth-cycle positions so the
//          viewer can read "what key is over there" without text.
//   4. Phong-ish lighting from a single fixed directional light, plus
//      a touch of rim lighting for definition.
//   5. Background: a subtle radial darkening so the torus has visual
//      pop without competing UI chrome.

#version 440

layout(location = 0) in  vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4  qt_Matrix;
    float qt_Opacity;
    vec2  viewportPx;
    float time;            // seconds since program start; drives the orbit
    float centroidU;       // smoothed weighted-centroid u in [0, 1]
    float centroidV;       // smoothed weighted-centroid v in [0, 1]
    // Top-4 individual key positions. .xy = (u, v), .z = softmax weight,
    // .w currently unused (reserved for per-key color tint).
    vec4  topKey0;
    vec4  topKey1;
    vec4  topKey2;
    vec4  topKey3;
    vec4  keyColor;        // primary glow color (warm amber by default)
    vec4  backColor;       // torus surface base (deep teal)
    vec4  ringColor;       // fifth-cycle meridian lines
};

const float TAU = 6.2831853071795864769;
const float PI  = 3.1415926535897932385;

// Torus radii. Major (big ring) 1.0, minor (tube cross-section) 0.35
// gives a tube about 1/3 the size of the ring -- thick enough to read
// the glow and the meridian lines, thin enough that the silhouette is
// recognizably a torus.
const vec2 TORUS_RADII = vec2(1.0, 0.35);

// Inigo Quilez's signed distance to a torus centred at the origin in
// the XZ plane. p = sample point in world space; t.x = ring radius,
// t.y = tube radius. https://iquilezles.org/articles/distfunctions/
float sdTorus(vec3 p, vec2 t) {
    vec2 q = vec2(length(p.xz) - t.x, p.y);
    return length(q) - t.y;
}

// SDF normal via central differences. Epsilon = 0.001 matches the hit
// threshold; using the same scale here keeps numerical noise consistent
// with the hit point precision.
vec3 sdfNormal(vec3 p) {
    vec2 e = vec2(0.001, 0.0);
    return normalize(vec3(
        sdTorus(p + e.xyy, TORUS_RADII) - sdTorus(p - e.xyy, TORUS_RADII),
        sdTorus(p + e.yxy, TORUS_RADII) - sdTorus(p - e.yxy, TORUS_RADII),
        sdTorus(p + e.yyx, TORUS_RADII) - sdTorus(p - e.yyx, TORUS_RADII)
    ));
}

// Build a per-pixel ray. uv is in [-1, 1] with aspect already corrected;
// eye is the camera position, lookAt the target, focal the focal length
// (1.0 ~= 53 deg FOV). Standard "look-from / look-at / up" frame.
void cameraRay(in  vec2 uv,
               in  vec3 eye,
               in  vec3 lookAt,
               in  vec3 up,
               in  float focal,
               out vec3 rayDir)
{
    vec3 ww = normalize(lookAt - eye);          // forward
    vec3 uu = normalize(cross(ww, up));         // right
    vec3 vv = normalize(cross(uu, ww));         // up (re-orthogonalised)
    rayDir  = normalize(uv.x * uu + uv.y * vv + focal * ww);
}

// Map a torus surface point back to (u, v) in [0, 1]. u is the angle
// around the big ring (atan2(z, x)); v is the angle around the tube
// cross-section (atan2(y, length(xz) - majorR)). Both wrapped via fract
// so values stay in [0, 1).
vec2 torusUV(vec3 p) {
    float u = atan(p.z, p.x) / TAU;
    float v = atan(p.y, length(p.xz) - TORUS_RADII.x) / TAU;
    return fract(vec2(u, v) + 1.0);
}

// Toroidal distance between two points in (u, v) space. Both axes wrap
// at 1.0, so the shortest distance is min(|du|, 1 - |du|) per axis. This
// is what makes the glow at u = 0.95 also visible from u = 0.05 (same
// place on the torus).
float toroidalDist(vec2 a, vec2 b) {
    vec2 d = abs(a - b);
    d = min(d, vec2(1.0) - d);
    return length(d);
}

// Twilight-ish v -> base color ramp. Warm side (v near 0, major
// hemisphere) is a deep teal-amber blend; cool side (v near 0.5, minor
// hemisphere) shifts toward a violet-blue. v wraps so the seam at 1.0
// matches the seam at 0.0 -- both are "major" by definition.
vec3 baseTint(float v) {
    // Distance from the major seam (v=0) along the shortest arc, in
    // [0, 0.5]. = 0 at major root, = 0.5 at minor root.
    float modeT = min(v, 1.0 - v) * 2.0;       // 0..1 over half the tube
    vec3 majorRGB = backColor.rgb;
    // Slightly cooler / more violet for the minor hemisphere -- distinct
    // enough from the major side that the eye can tell which one the
    // glow is on without reading a label.
    vec3 minorRGB = backColor.rgb * 0.7 + vec3(0.08, 0.05, 0.18);
    return mix(majorRGB, minorRGB, smoothstep(0.0, 1.0, modeT));
}

// Additive glow contribution at surface coord uv from a single source
// at (sourceU, sourceV) with the given weight. Gaussian falloff in
// toroidal-coord space; sigma 0.04 gives a soft disc roughly the size
// of one "key cell" on the torus.
vec3 glowAt(vec2 surfUV, vec2 sourceUV, float weight, vec3 color) {
    float d   = toroidalDist(surfUV, sourceUV);
    float sig = 0.04;
    float g   = exp(-(d * d) / (2.0 * sig * sig));
    return color * (g * weight);
}

void main() {
    // 1. Build a centred, aspect-corrected NDC: x in [-aspect, aspect],
    //    y in [-1, 1]. Qt's qt_TexCoord0 starts (0, 0) at the top-left;
    //    we flip y so positive is up.
    float aspect = viewportPx.x / max(viewportPx.y, 1.0);
    vec2  uv     = qt_TexCoord0 * 2.0 - 1.0;
    uv.y         = -uv.y;
    uv.x        *= aspect;

    // 2. Slow orbit camera. omega = TAU / 30 rad/s -> 1 revolution per
    //    30 seconds. Slight Y elevation so the torus reads as 3D rather
    //    than disc-on-edge. Distance 2.5 keeps the whole torus on screen
    //    with a focal of 1.5 (~37 deg FOV) at the widest layout.
    float orbitOmega = TAU / 30.0;
    float a          = time * orbitOmega;
    vec3  eye        = vec3(2.5 * cos(a), 0.8, 2.5 * sin(a));
    vec3  lookAt     = vec3(0.0);
    vec3  up         = vec3(0.0, 1.0, 0.0);
    vec3  rayDir;
    cameraRay(uv, eye, lookAt, up, 1.5, rayDir);

    // 3. Sphere-trace the SDF. Max 64 iterations is more than enough --
    //    the torus is convex from outside, so the ray usually hits in
    //    20-40 steps. Threshold 0.001 keeps the surface read sharp.
    float tHit     = 0.0;
    bool  hit      = false;
    const int  MAX_STEPS = 64;
    const float HIT_EPS  = 0.001;
    const float MAX_T    = 12.0;     // bail-out distance (rays past the
                                     // torus into empty space)
    for (int i = 0; i < MAX_STEPS; ++i) {
        vec3  p = eye + rayDir * tHit;
        float d = sdTorus(p, TORUS_RADII);
        if (d < HIT_EPS) { hit = true; break; }
        tHit += d;
        if (tHit > MAX_T) break;
    }

    if (!hit) {
        // Background: subtle radial gradient. Dark in the centre, a
        // shade lighter at the corners -- gives the torus visual lift
        // without distracting from it.
        float r  = length(uv);
        float bg = mix(0.012, 0.030, smoothstep(0.0, 1.5, r));
        fragColor = vec4(vec3(bg), qt_Opacity);
        return;
    }

    vec3  hitP   = eye + rayDir * tHit;
    vec3  N      = sdfNormal(hitP);
    vec2  surfUV = torusUV(hitP);

    // 4. Lighting -- single fixed directional, soft ambient floor,
    //    rim term for silhouette pop. Phong NdotL clamped to [0, 1].
    vec3  lightDir = normalize(vec3(1.0, 1.0, 0.7));
    float ndl      = clamp(dot(N, lightDir), 0.0, 1.0);
    vec3  viewDir  = normalize(eye - hitP);
    float rim      = pow(1.0 - clamp(dot(N, viewDir), 0.0, 1.0), 2.5);
    // Specular highlight for material definition; kept small so it
    // doesn't fight with the additive key glows.
    vec3  halfV    = normalize(lightDir + viewDir);
    float spec     = pow(clamp(dot(N, halfV), 0.0, 1.0), 32.0) * 0.25;

    // 5. Base color from the v-axis ramp, modulated by lighting.
    vec3 base = baseTint(surfUV.y);
    vec3 lit  = base * (0.30 + 0.70 * ndl) + vec3(spec);
    lit += base * rim * 0.35;            // rim glow tinted by hemisphere

    // 6. Fifth-cycle meridian lines. 12 evenly spaced bands around u
    //    so the viewer can locate "the C meridian, the G meridian, ..."
    //    by eye. We draw them as a sub-pixel-wide line at the integer
    //    multiples of 1/12; smoothstep gives an anti-aliased edge.
    float uTwelve = fract(surfUV.x * 12.0);
    float dist12  = min(uTwelve, 1.0 - uTwelve);
    float band    = 1.0 - smoothstep(0.005, 0.020, dist12);
    lit = mix(lit, ringColor.rgb, band * ringColor.a * 0.8);
    // A second, fainter set of bands on the v axis at the major/minor
    // seam (v = 0) and the relative-minor (v = 0.5) so the hemisphere
    // boundary is visible without text labels.
    float vTwo   = fract(surfUV.y * 2.0);
    float distV2 = min(vTwo, 1.0 - vTwo);
    float bandV  = 1.0 - smoothstep(0.005, 0.015, distV2);
    lit = mix(lit, ringColor.rgb, bandV * ringColor.a * 0.5);

    // 7. Additive primary glow at the centroid. Use the *primary* key
    //    color, scaled up so the centroid reads as the visual anchor.
    //    Weight = 1.0 here because the centroid is by definition the
    //    weighted mean of all keys, not a single key's weight.
    vec3 glow = glowAt(surfUV, vec2(centroidU, centroidV),
                       1.0, keyColor.rgb) * 1.8;

    // 8. Secondary glows at the top-N keys, scaled by softmax weight.
    //    These give the viewer a feel for "we're 60% in C major, 25%
    //    in A minor, 15% in F major" as three little dots beyond the
    //    primary glow disc.
    //
    //    Per-source weight is the .z of each topKey vec4. .w is unused
    //    (reserved). When the weight is zero we add nothing -- the
    //    glow function multiplies by weight, so zero is a no-op.
    glow += glowAt(surfUV, topKey0.xy, topKey0.z, keyColor.rgb) * 0.8;
    glow += glowAt(surfUV, topKey1.xy, topKey1.z, keyColor.rgb) * 0.6;
    glow += glowAt(surfUV, topKey2.xy, topKey2.z, keyColor.rgb) * 0.5;
    glow += glowAt(surfUV, topKey3.xy, topKey3.z, keyColor.rgb) * 0.4;

    // 9. Final composite. Plus blend is what gives the "hot spot at
    //    the current key" feel -- the centroid stacks on top of base +
    //    rings + lighting without losing them. Clamp at the end so a
    //    very bright glow doesn't go past 1.0 per channel.
    vec3 finalRgb = clamp(lit + glow, vec3(0.0), vec3(1.0));
    fragColor = vec4(finalRgb, qt_Opacity);
}
