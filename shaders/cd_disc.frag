// CD disc surface shader.
//
// Renders the silvery, iridescent face of a compact disc. The
// iridescence model follows Alan Zucconi's diffraction-grating shader
// (alanzucconi.com/2017/07/15/cd-rom-shader-1/): for each pixel we
// treat the concentric track grooves as a 1-D diffraction grating,
// project the light direction onto the local groove tangent, and sum
// the visible-spectrum contributions across the first several
// diffraction orders. The wavelength→RGB conversion uses
// spectral_zucconi6 (two-bump-per-channel fit, no branches).
//
// Lighting model (Concerto-specific): instead of a single point light
// orbiting the rim, we have a diameter axis that rotates slowly
// around the disc center (axisAngle) and a "light" that slides back
// and forth along that diameter (lightOffset, signed). The whole disc
// effectively rotates with the axis, and the specular highlight
// crosses through the center each cycle.
//
// Border AA: every radius-based band transition uses smoothstep masks
// rather than `if` branches, so the hub / lead-in / data / rim edges
// stay smooth at any pixel scale. The 2x2 supersample on the
// iridescence covers the high-frequency fringes separately.

#version 440

layout(location = 0) in  vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

// Matrix-text mask. The CdDiscCanvas QML feeds in a ShaderEffectSource
// of the curved-text canvas (white-on-transparent glyphs with the
// radial wedge already applied). Its alpha channel tells this shader
// where inside the mirror band to render iridescence — so the etched
// text picks up the same V·T / light physics as the data region.
layout(binding = 1) uniform sampler2D matrixMask;

layout(std140, binding = 0) uniform buf {
    mat4  qt_Matrix;
    float qt_Opacity;
    float axisAngle;         // radians; the rotating diameter
    float lightRadius;       // fixed position of the light along the axis
    float viewTiltX;         // viewer-tilt vector x — small animated value
    float viewTiltY;         // viewer-tilt vector y — small animated value
    float readProgress;      // 0..1 over the user program area
    // CD anatomy radii (normalised to outer disc radius = 1.0). Real
    // CD outer = 60 mm, so 1.0 ≡ 60 mm. Boundaries are from ECMA-130
    // §8 (Information Area, clamping area, transition areas) and the
    // IFPI SID Code Implementation Guide v2.4 (lead-in start at 22.9
    // mm). From the centre outward:
    //   0 → hubR        : spindle hole
    //   hubR → clearOuter   : clear polycarbonate (transition + clamping)
    //   clearOuter → mirrorOuter : metallised mirror band, no pits — IFPI / matrix etch lives here
    //   mirrorOuter → dataOuter  : pit region (lead-in + program + lead-out)
    //   dataInner → dataOuter    : user program area subset (where read-fill maps)
    //   dataOuter → 1.0          : outer buffer + rim
    float hubR;
    float clearOuter;
    float mirrorOuter;
    float dataInner;
    float dataOuter;
    float fringeSpacing;
    float iridescenceMix;
    float specularStrength;
};

vec3 bump3y(vec3 x, vec3 yoffset) {
    vec3 y = vec3(1.0) - x * x;
    return clamp(y - yoffset, vec3(0.0), vec3(1.0));
}

vec3 spectral_zucconi6(float w) {
    float x = clamp((w - 400.0) / 300.0, 0.0, 1.0);
    const vec3 c1 = vec3(3.54585104, 2.93225262, 2.41593945);
    const vec3 x1 = vec3(0.69549072, 0.49228336, 0.27699880);
    const vec3 y1 = vec3(0.02312639, 0.15225084, 0.52607955);
    const vec3 c2 = vec3(3.90307140, 3.21182957, 3.96587128);
    const vec3 x2 = vec3(0.11748627, 0.86755042, 0.66077860);
    const vec3 y2 = vec3(0.84897130, 0.88445281, 0.73949448);
    return bump3y(c1 * (x - x1), y1) + bump3y(c2 * (x - x2), y2);
}

// Tiny hash used for sub-pixel jitter — breaks up the smooth grating
// bands so the result reads as "real CD shimmer" rather than vector
// art. Cheap one-line variant from the Shadertoy community.
float discHash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

// Iridescence at a specific uv. L is the per-pixel direction from the
// surface to the moving light position. Returns HDR (unclamped) values
// — peaks above 1.0 are where multiple diffraction orders constructively
// pile up, the "white-hot" hotspots of a real CD. The caller tonemaps
// after compositing.
vec3 iridescenceAt(vec2 uv) {
    float theta = atan(uv.y, uv.x);
    vec2  T = vec2(-sin(theta), cos(theta));

    vec2  axisDir  = vec2(cos(axisAngle), sin(axisAngle));
    vec2  lightPos = axisDir * lightRadius;
    vec2  toLight  = lightPos - uv;
    float lenL     = max(length(toLight), 1e-4);
    vec2  L        = toLight / lenL;

    // Per-pixel jitter (~1500 cycles/disc ≈ real CD groove order). Tiny
    // amplitude — just enough to disambiguate "live shimmer" from
    // "static stripe pattern".
    float jitter = (discHash(uv * 1500.0) - 0.5) * 0.025;

    // Full grating formula: u = |sin θ_L − sin θ_V|, expressed via the
    // tangent projection (Zucconi's form). The V tilt is a small 2D
    // vector that rotates in its own phase — at every pixel V·T turns
    // continuously, so the wavelength selection shifts even when nothing
    // else moves. This is what gives the iridescence its "alive,
    // breathing" quality, more than the light's motion does.
    vec2  V_inplane = vec2(viewTiltX, viewTiltY);
    float u = abs(dot(L, T) - dot(V_inplane, T)) + jitter;

    // Sum across 8 diffraction orders, but each weighted by a sinc²-
    // like envelope so high orders fall off smoothly instead of cliffing
    // at the loop bound. Equivalent in spirit to Stam '99 — high orders
    // are physically there but quickly subdominant. The 4.0 re-normalises
    // so the envelope doesn't dim the overall result.
    vec3 irid = vec3(0.0);
    for (int n = 1; n <= 8; ++n) {
        float fn  = float(n);
        float sn  = sin(3.14159 * fn * 0.18);
        float env = (sn * sn) / (fn * fn);
        float wavelength = u * fringeSpacing / fn;
        irid += env * spectral_zucconi6(wavelength);
    }
    return irid * 4.0;   // intentionally HDR — caller tonemaps
}

void main() {
    vec2 uv = qt_TexCoord0 * 2.0 - 1.0;
    float r = length(uv);

    // ------- Border AA masks ----------------------------------------
    // Per-pixel uv step — drives smoothstep falloffs at each band
    // boundary. fwidth() returns the screen-space derivative so the
    // mask width tracks the actual resolution / dpr.
    float aa = max(fwidth(r), 0.002);

    // Outer-rim alpha — wider transition than the internal bands
    // because the rim is the silhouette of the disc against the popup
    // background; a 2-pixel band there reads as stairsteps. ~5 pixels
    // is gentle enough to look smooth without visibly softening the
    // disc's outline.
    float rimAA = aa * 2.5;
    float discA = 1.0 - smoothstep(1.0 - rimAA, 1.0 + rimAA * 0.6, r);
    if (discA <= 0.0) {
        fragColor = vec4(0.0);
        return;
    }

    // Region masks — one per CD-anatomy zone (ECMA-130 §8). Each is 1
    // inside the zone, 0 outside, with smoothstep AA at the boundaries.
    //   hubA      : centre hole               (r < hubR)
    //   clearA    : clear polycarbonate area  (hubR..clearOuter)
    //   mirrorA   : metallised mirror band    (clearOuter..mirrorOuter)
    //   pitA      : pit region — lead-in,
    //               program, lead-out          (mirrorOuter..dataOuter)
    //   (outer buffer / rim handled by discA)
    float hubA = 1.0 - smoothstep(hubR - aa, hubR + aa, r);
    float clearA = smoothstep(hubR - aa, hubR + aa, r)
                 * (1.0 - smoothstep(clearOuter - aa, clearOuter + aa, r));
    float mirrorA = smoothstep(clearOuter - aa, clearOuter + aa, r)
                  * (1.0 - smoothstep(mirrorOuter - aa, mirrorOuter + aa, r));
    float pitA = smoothstep(mirrorOuter - aa, mirrorOuter + aa, r)
               * (1.0 - smoothstep(dataOuter - aa, dataOuter + aa, r));

    // ------- Silver base --------------------------------------------
    // Visible across mirror band, pit region, and outer rim. The clear
    // polycarbonate region (clearA) overrides this with dark plastic
    // at the end of main().
    float silverShade =
        mix(0.36, 0.62, smoothstep(mirrorOuter, dataOuter * 0.88, r));
    silverShade *= 1.0 - 0.35 * smoothstep(0.92, 1.0, r);
    vec3 disc = vec3(silverShade);

    // ------- Iridescence (diffraction grating, 2x2 supersampled) ----
    // HDR — supersampling clamped LDR rainbow does nothing, but
    // supersampling pre-tonemap HDR is what gives the smooth, alive
    // shimmer. We tonemap the composited disc at the end of main().
    vec2 du = vec2(aa * 0.5, 0.0);
    vec2 dv = vec2(0.0, aa * 0.5);
    vec3 irid = (
          iridescenceAt(uv - du - dv)
        + iridescenceAt(uv + du - dv)
        + iridescenceAt(uv - du + dv)
        + iridescenceAt(uv + du + dv)
    ) * 0.25;

    // Directional lobe envelope. Real CDs show rainbow concentrated as
    // two opposing lobes along the light's diameter, fading 90° away
    // — without this the iridescence looks uniformly distributed and
    // "static". `lobe` peaks on the axis, has a 0.25 floor 90° away.
    vec2  axis    = vec2(cos(axisAngle), sin(axisAngle));
    float axisDot = abs(dot(normalize(uv + vec2(1e-4)), axis));
    float lobe    = mix(0.25, 1.0, pow(axisDot, 1.5));

    // Iridescence in the pit region (lead-in + program + lead-out) and
    // in the mirror band wherever the matrix-text mask is non-zero.
    // The mirror band is metallised but unrecorded, *except* where
    // text has been laser-etched into the metal — those glyphs see the
    // same diffraction physics as the data area. Sample the mask once,
    // use its alpha as a per-pixel weight inside mirrorA.
    float maskA   = texture(matrixMask, qt_TexCoord0).a;
    float iridW   = pitA + mirrorA * maskA;
    disc = mix(disc, disc * 0.45 + irid, iridescenceMix * iridW * lobe);

    // ------- Specular streak ----------------------------------------
    // Light sits at fixed lightRadius along the rotating axis (no
    // back-and-forth crossing). Highlight is stretched along the axis
    // direction and narrow across it — a visual cue that the surface
    // has linear groove structure.
    vec2  specPos = axis * lightRadius;
    vec2  axisN   = vec2(-axis.y, axis.x);
    vec2  toSpec  = uv - specPos;
    float along   = dot(toSpec, axis);
    float across  = dot(toSpec, axisN);
    float spec    = exp(-(along * along * 28.0
                        + across * across * 220.0))
                  * specularStrength;
    disc += vec3(spec);

    // ------- Blue read-fill overlay (program area only) -------------
    // Read progress maps over the *user program area* (dataInner →
    // dataOuter), not the entire pit region — the lead-in and lead-out
    // bands aren't user-rip sectors. Area-correct mapping so the
    // leading edge advances with sectors-read.
    if (pitA > 0.0) {
        float r2         = r * r;
        float dataInner2 = dataInner * dataInner;
        float dataOuter2 = dataOuter * dataOuter;
        float radiusFrac = (r2 - dataInner2) / (dataOuter2 - dataInner2);
        float pastInner  = smoothstep(dataInner - aa, dataInner + aa, r);
        float radFwidth  = fwidth(radiusFrac) * 1.5;
        float readMask   = 1.0 - smoothstep(readProgress - radFwidth,
                                            readProgress + radFwidth,
                                            radiusFrac);
        vec3  readBlue   = mix(
            vec3(0.04, 0.20, 0.50),
            vec3(0.55, 0.82, 1.00),
            clamp(radiusFrac, 0.0, 1.0));
        disc = mix(disc, readBlue,
                   0.55 * readMask * pitA * pastInner);

        // Leading-edge glow at the current read radius — Gaussian.
        float currentR = sqrt(dataInner2
                              + readProgress * (dataOuter2 - dataInner2));
        float edgeDist = abs(r - currentR);
        float edgeGlow = exp(-edgeDist * edgeDist * 6000.0) * 0.5
                       * step(0.001, readProgress)
                       * step(readProgress, 0.999)
                       * pastInner;
        disc += vec3(edgeGlow);
    }

    // ------- Filmic tonemap -----------------------------------------
    // Iridescence + specular were rendered in HDR (peaks above 1.0).
    // ACES approximation rolls bright values down toward white instead
    // of clipping a single channel — that's what gives the white-hot
    // hotspots their natural feel where multiple diffraction orders
    // pile up. Cheap two-instruction form from Krzysztof Narkowicz.
    disc = (disc * (2.51 * disc + 0.03))
         / (disc * (2.43 * disc + 0.59) + 0.14);
    disc = clamp(disc, 0.0, 1.0);

    // ------- Clear polycarbonate regions (override silver) ---------
    // Two strips of bare clear plastic on a real CD: the inner hub area
    // (between spindle hole and clamping outer edge) and the outer rim
    // (outside the metallised region). Both render as near-black over
    // the popup background. The inner one also gets a thin highlight at
    // the stacking-ring edge.
    vec3 clearColor = vec3(0.045, 0.05, 0.07);
    float ringDist = abs(r - (clearOuter - 0.010));
    clearColor += vec3(exp(-ringDist * ringDist * 70000.0) * 0.18);
    disc = mix(disc, clearColor, clearA);

    float outerClearA = smoothstep(dataOuter - aa, dataOuter + aa, r);
    disc = mix(disc, vec3(0.045, 0.05, 0.07), outerClearA);

    // ------- Hub flat black -----------------------------------------
    disc = mix(disc, vec3(0.0), hubA);

    fragColor = vec4(disc, discA * qt_Opacity);
}
