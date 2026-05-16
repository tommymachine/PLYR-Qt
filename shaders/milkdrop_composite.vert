// Composite pass for the Milkdrop renderer — Layer 3b.
//
// A fullscreen-triangle pass that copies the current warp output
// texture onto the QQuickRhiItem's visible color target. Identity
// transform; the vertex buffer is a clip-space [-1, +1]^2 quad with
// UV [0, 1]^2.

#version 440

layout(location = 0) in  vec2 a_pos;
layout(location = 0) out vec2 v_uv;

layout(std140, binding = 0) uniform buf {
    mat4  qt_Matrix;
    float qt_Opacity;
    float gamma;
    float _pad0;
    float _pad1;
};

void main()
{
    v_uv        = a_pos * 0.5 + 0.5;     // clip-space → [0, 1]
    gl_Position = qt_Matrix * vec4(a_pos, 0.0, 1.0);
}
