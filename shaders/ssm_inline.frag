// Inline SSM scrubber strip (Layer 4b, inline mode).
//
// Reads ONE row from the T x T matrix at v = playbackFrac. That row is
// the self-similarity of the current playback frame against every other
// frame in the song: dark cells = "different from now"; bright cells =
// "similar to now". Painting this strip as the background of the seek
// slider gives a visual map of repetition -- the listener can SEE where
// in the song this section recurs.
//
// Per-pixel:
//   u in [0, 1]  -- time axis. Sample source(u, playbackFrac).
//   y in [0, 1]  -- vertical, used only for an optional center-fade so
//                   the strip blends into the surrounding UI.
//   Apply IQ-magma; draw a thin vertical "now" line at u=playbackFrac.

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
    float u = qt_TexCoord0.x;
    // Sample the row at v = playbackFrac. v is NOT flipped here because
    // the texture is stored row-major (frame i = scanLine i) and "the
    // current frame's row" is just frame i (no orientation choice yet).
    float val = clamp(texture(source, vec2(u, playbackFrac)).r, 0.0, 1.0);

    // Same correlation-window remap + gamma as the 2D path.
    float t = clamp((val - 0.5) * 2.0, 0.0, 1.0);
    t = pow(t, 0.7);
    vec3 rgb = magma(t);

    // "Now" line at u = playbackFrac. Constant fraction of [0,1] UV
    // (avoids ESSL 100's missing textureSize() builtin).
    const float kLineHalfWidth = 0.004;
    float dx = abs(u - playbackFrac);
    float line = smoothstep(kLineHalfWidth, 0.0, dx);
    vec3 outColor = mix(rgb, tintColor.rgb, line);

    fragColor = vec4(outColor, 1.0) * qt_Opacity;
}
