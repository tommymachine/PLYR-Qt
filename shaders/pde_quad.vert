// Shared fullscreen passthrough vertex stage for the Layer 4d PDE
// renderer. Used by:
//
//   * pde_grayscott.frag         -- the Gray-Scott solver pass.
//   * pde_grayscott_display.frag -- the Gray-Scott visualizer pass.
//   * pde_chladni.frag           -- the (stateless) Chladni plate.
//
// One vertex shader keeps the .qsb bake list short. The UBO layout is
// kept minimal here (just qt_Matrix + qt_Opacity, which Qt would inject
// anyway) so each fragment shader is free to declare its own UBO at
// binding 1.

#version 440

layout(location = 0) in  vec2 a_pos;
layout(location = 0) out vec2 v_uv;

layout(std140, binding = 0) uniform buf {
    mat4  qt_Matrix;
    float qt_Opacity;
};

void main()
{
    v_uv        = a_pos * 0.5 + 0.5;       // clip-space [-1,+1] -> [0,1]
    gl_Position = qt_Matrix * vec4(a_pos, 0.0, 1.0);
}
