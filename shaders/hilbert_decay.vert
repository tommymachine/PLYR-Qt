// Full-screen-quad vertex stage shared by the Hilbert-rosette
// phosphor-decay pass and the final composite-to-screen pass. Generates
// a screen-covering triangle pair from a [-1,+1]^2 corner attribute and
// forwards a [0,1]^2 UV for the fragment stage to sample the source
// texture with.

#version 440

layout(location = 0) in  vec2 a_corner;
layout(location = 0) out vec2 v_uv;

layout(std140, binding = 0) uniform buf {
    float decayFactor;
    float _pad0;
    float _pad1;
    float _pad2;
    vec4  tint;
};

void main() {
    v_uv        = a_corner * 0.5 + vec2(0.5);
    gl_Position = vec4(a_corner, 0.0, 1.0);
}
