// 2D self-similarity matrix heatmap (Layer 4b, wide mode).
//
// Inputs:
//   source        -- R8 texture, T x T. Cell (i, j) = quantised cosine
//                    similarity between frame i and frame j. The diagonal
//                    is 255 (identical). Block patterns on the diagonal
//                    are contiguous similar segments (verses, choruses).
//                    Off-diagonal stripes are repeats of earlier material.
//   playbackFrac  -- current playback position normalised to [0, 1].
//   tintColor     -- accent tint applied to the playback crosshair.
//
// Per-pixel:
//   u, v in [0, 1]^2 -- sample the texture, apply the IQ-magma colormap
//   for a perceptually-uniform spectrogram-style palette, then overlay a
//   thin vertical AND horizontal line at the playback fraction. The
//   intersection of the two lines is the current frame's self-similarity
//   cell, which is by definition the brightest cell on the visible row.

#version 440

layout(location = 0) in  vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4  qt_Matrix;
    float qt_Opacity;
    float playbackFrac;
    vec4  tintColor;
};

layout(binding = 1) uniform sampler2D source;

// IQ magma. CC0. t=0 -> near-black; t=0.5 -> reddish-purple; t=1 ->
// cream-yellow. Same polynomial used by the CQT spectrogram waterfall
// (Layer 1c). Dark base makes off-diagonal noise read as quiet.
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
    // Qt's qt_TexCoord0.y starts at 0 at the TOP of the quad. We want
    // frame 0 at the BOTTOM-LEFT (canonical SSM orientation -- time runs
    // left to right and bottom to top on the diagonal), so we flip v.
    float u = qt_TexCoord0.x;
    float v = 1.0 - qt_TexCoord0.y;

    // The matrix stores [0, 255]; we read it as [0, 1] linear. The dark
    // floor at value=0.5 (anti-correlated) becomes mid-gray under magma;
    // 0.5 is exactly the "everyday off-diagonal noise floor" the eye
    // should ignore. Apply a gamma to compress the bright tail, same
    // trick the CQT waterfall uses.
    float v_sample = clamp(texture(source, vec2(u, v)).r, 0.0, 1.0);
    // Map [0.5, 1.0] to [0, 1] so the visible range emphasises positive
    // correlation. Cells under 0.5 (anti-correlated, rare in music) clip
    // to zero and stay black.
    float t = clamp((v_sample - 0.5) * 2.0, 0.0, 1.0);
    t = pow(t, 0.7);
    vec3 rgb = magma(t);

    // Playback crosshair: thin vertical + horizontal line at
    // u == playbackFrac and v == playbackFrac. Half-width is a constant
    // fraction of the [0,1] UV range so the line is visible at any zoom
    // level. ESSL 100 has no textureSize(), so we don't derive width from
    // texel count -- a fixed 0.4% of UV span reads as ~2 px on a typical
    // 500 px scrubber.
    const float kLineHalfWidth = 0.004;
    float dx = abs(u - playbackFrac);
    float dy = abs(v - playbackFrac);
    float lineU = smoothstep(kLineHalfWidth, 0.0, dx);
    float lineV = smoothstep(kLineHalfWidth, 0.0, dy);
    float line  = max(lineU, lineV) * 0.8;

    // Composite: heatmap base + tint-coloured crosshair, mixed by line.
    vec3 outColor = mix(rgb, tintColor.rgb, line);

    fragColor = vec4(outColor, 1.0) * qt_Opacity;
}
