// Passthrough vertex stage for the Janata tonal-space torus (Layer 2b).
// Forwards qt_TexCoord0 to the fragment stage which does the full
// raymarched-SDF work per pixel. Uniform block must mirror tonal_torus.
// frag's exactly -- Qt Quick binds one std140 UBO (binding=0) to both
// stages.

#version 440

layout(location = 0) in  vec4 qt_Vertex;
layout(location = 1) in  vec2 qt_MultiTexCoord0;
layout(location = 0) out vec2 qt_TexCoord0;

layout(std140, binding = 0) uniform buf {
    mat4  qt_Matrix;
    float qt_Opacity;
    vec2  viewportPx;
    float time;
    float centroidU;
    float centroidV;
    vec4  topKey0;
    vec4  topKey1;
    vec4  topKey2;
    vec4  topKey3;
    vec4  keyColor;
    vec4  backColor;
    vec4  ringColor;
};

void main() {
    qt_TexCoord0 = qt_MultiTexCoord0;
    gl_Position  = qt_Matrix * qt_Vertex;
}
