// Phosphor-decay + composite fragment stage.
//
// Used twice in the per-frame pipeline:
//
//   1. Decay pass (tint = white, sample = previous accumulation texture)
//        Reads prevFrame, writes prevFrame * decayFactor into the new
//        accumulation buffer. This sets the "after-glow" curve before
//        the segment pass paints fresh beam intensity on top.
//
//   2. Composite pass (tint = beamColor, sample = accumulation texture)
//        Reads the freshly-additive-blended accumulation, multiplies by
//        the chosen beam color, writes to the visible color target.
//
// One shader handles both because the math is identical: out = sample
// · scalar · color. `decayFactor` is the scalar; `tint` is the color.

#version 440

layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4  qt_Matrix;
    float qt_Opacity;
    float decayFactor;
    vec4  tint;
};

layout(binding = 1) uniform sampler2D src;

void main() {
    vec4 s = texture(src, v_uv);
    // Premultiplied source — keep premultiplied semantics on output so
    // downstream additive blends behave naturally.
    fragColor = s * decayFactor * tint;
}
