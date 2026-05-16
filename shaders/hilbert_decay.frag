// Phosphor-decay + composite fragment stage for the Hilbert-rosette.
//
// Used twice per frame:
//   1. Decay pass  -- tint = white, sample = previous accum.
//      Writes prevAccum * decayFactor into the new accumulation buffer.
//   2. Composite pass -- tint = white, decayFactor = 1, sample = fresh
//      accum. Writes the fully-rendered accumulation buffer onto the
//      visible color target with no further modulation.
//
//  out = sample * decayFactor * tint
//
//  Matches the ScopeRenderer pattern but with a simpler UBO (no
//  qt_Matrix needed; the vert shader skips the model transform).

#version 440

layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    float decayFactor;
    float _pad0;
    float _pad1;
    float _pad2;
    vec4  tint;
};

layout(binding = 1) uniform sampler2D src;

void main() {
    vec4 s = texture(src, v_uv);
    fragColor = s * decayFactor * tint;
}
