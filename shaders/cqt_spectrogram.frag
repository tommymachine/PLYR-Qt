// CQT spectrogram waterfall fragment stage (Layer 1c).
//
// Inputs:
//   source       — R8 ring texture, `columns` wide x `bins` tall. Each
//                  column is one CQT hop; the texture wraps horizontally
//                  via mod(u + scrollOffset/columns, 1.0). Row 0 is the
//                  lowest pitch (C1); row N-1 is the highest pitch.
//   scrollOffset — texel column the GUI thread will write next. Treated
//                  as the "newest" boundary; the previous column is the
//                  most-recent past, scrolling left visually.
//   columns      — texture width (1024 by default).
//   gamma        — perceptual brightness adjust. 0.5 => sqrt mapping,
//                  which compresses the bright tail so quiet harmonics
//                  read clearly without crushing transient peaks.
//   lowColor, highColor — fallback colormap endpoints when useMagma=0.
//   useMagma     — 1 => analytic magma polynomial (CC0, by Inigo Quilez).
//                  0 => linear interp between lowColor and highColor.
//
// Per-pixel:
//   u in [0,1] left-to-right = oldest-to-newest in wall time.
//   v in [0,1] bottom-to-top = lowest-to-highest pitch (we flip Qt's y).
//   Sample the ring with horizontal wraparound; apply gamma; colormap.

#version 440

layout(location = 0) in  vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4  qt_Matrix;
    float qt_Opacity;
    vec4  lowColor;
    vec4  highColor;
    float scrollOffset;
    float columns;
    float gamma;
    float useMagma;
};

layout(binding = 1) uniform sampler2D source;

// Magma colormap polynomial, CC0 (Inigo Quilez). t=0 -> near-black,
// t=0.5 -> reddish-purple, t=1 -> cream-yellow. The dark base lets quiet
// sections genuinely look quiet -- the key reason to prefer it over
// viridis for spectrograms.
vec3 magma(float t) {
    const vec3 c0 = vec3(-0.002136485053, -0.000749655052, -0.005386127855);
    const vec3 c1 = vec3( 0.251748934601,  0.677849206252, -0.034678055700);
    const vec3 c2 = vec3( 0.860514435000, -1.247214403260,  0.273895315120);
    const vec3 c3 = vec3(-1.027162779820,  0.999275579280, -2.954594386790);
    const vec3 c4 = vec3( 4.196793999940, -1.018995867680,  1.706935010860);
    const vec3 c5 = vec3(-3.487010980670,  3.094525043990, -1.075050940300);
    const vec3 c6 = vec3( 1.108144898210, -2.527116020080,  1.045566370010);
    return c0 + t*(c1 + t*(c2 + t*(c3 + t*(c4 + t*(c5 + t*c6)))));
}

void main() {
    // Qt's qt_TexCoord0.y starts at 0 at the TOP edge of the quad.
    // We want the lowest pitch (row 0 in the ring) at the BOTTOM of the
    // display, so we flip v here. u maps left-to-right (oldest-to-
    // newest) directly.
    float u = qt_TexCoord0.x;
    float v = 1.0 - qt_TexCoord0.y;

    // Wraparound: the ring's column 0 is just past the most recent
    // (write column) -- it's the oldest column. As more hops fire,
    // scrollOffset increases; the "newest" column in screen coordinates
    // (u = 1.0) should match texel column (scrollOffset - 1). So the
    // mapping is: u_ring = (u + scrollOffset/columns) mod 1.0, with
    // u=0 -> the column at index scrollOffset (the oldest), u=1 -> the
    // column at scrollOffset - 1 (the newest, since the writer just
    // bumped past it).
    float off    = scrollOffset / max(columns, 1.0);
    float u_ring = fract(u + off);

    // Sample. The ring is R8 in linear space; we interpret the channel
    // as already-encoded magnitude (-80 dB -> 0, 0 dB -> 255) -- a
    // linear-in-display mapping. Linear filtering across columns gives
    // a subtle smear that hides the discrete hop boundary; across rows
    // it interpolates between adjacent pitch bins (we trade a hair of
    // pitch sharpness for vertical smoothness).
    float mag = texture(source, vec2(u_ring, v)).r;

    // Perceptual gamma. 0.5 ≈ sqrt; lifts quiet harmonics so they read
    // without making loud peaks blow out.
    float mag_disp = pow(clamp(mag, 0.0, 1.0), gamma);

    vec3 rgb = (useMagma > 0.5)
             ? magma(mag_disp)
             : mix(lowColor.rgb, highColor.rgb, mag_disp);

    fragColor = vec4(rgb, 1.0) * qt_Opacity;
}
