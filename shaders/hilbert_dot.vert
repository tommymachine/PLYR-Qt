// Vertex stage for the Hilbert-rosette per-band Gaussian dot.
//
// One quad per band. Each vertex carries:
//   a_corner  -- pre-scaled corner offset in pixels (-halfWidth..+halfWidth)
//   a_center  -- dot center in pixel space
//   a_bandIdx -- which band (0..N_BANDS-1) this vertex belongs to.
//
// The CPU has already done the polar math (radius * (cos a, sin a)) and
// the corner pre-scaling, so this stage just emits clip-space and passes
// the offset through to the fragment shader as the local Gaussian
// coordinate.

#version 440

layout(location = 0) in  vec2 a_corner;     // pixel-scaled corner offset
layout(location = 1) in  vec2 a_center;     // dot center, px
layout(location = 2) in  float a_bandIdx;

layout(location = 0) out vec2  v_off;       // offset from dot center, px
layout(location = 1) out float v_bandIdx;

layout(std140, binding = 0) uniform buf {
    vec2  viewportPx;
    float sigma;
    float intensity;
    vec4  bandColor[8];
};

void main() {
    vec2 posPx = a_center + a_corner;
    vec2 ndc   = (posPx / viewportPx) * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);

    v_off     = a_corner;
    v_bandIdx = a_bandIdx;
}
