// Passthrough vertex stage for the 2D self-similarity heatmap (Layer 4b).
// All visualisation work happens in the fragment stage. The UBO layout
// below must match ssm_2d.frag exactly -- Qt Quick binds the same UBO at
// binding=0 to both stages, and std140 packing depends on declaration
// order.

#version 440

layout(location = 0) in  vec4 qt_Vertex;
layout(location = 1) in  vec2 qt_MultiTexCoord0;
layout(location = 0) out vec2 qt_TexCoord0;

layout(std140, binding = 0) uniform buf {
    mat4  qt_Matrix;
    float qt_Opacity;
    float playbackFrac;
    vec4  tintColor;
};

void main() {
    qt_TexCoord0 = qt_MultiTexCoord0;
    gl_Position  = qt_Matrix * qt_Vertex;
}
