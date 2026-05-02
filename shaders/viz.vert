// Passthrough vertex shader for the PLYR visualizer.
// All the work is done per-pixel in viz.frag; this just forwards
// qt_TexCoord0 and applies the matrix/opacity Qt Quick provides.

#version 440

layout(location = 0) in  vec4 qt_Vertex;
layout(location = 1) in  vec2 qt_MultiTexCoord0;
layout(location = 0) out vec2 qt_TexCoord0;

// Must include every uniform the fragment shader uses, in the same
// order, because Qt Quick shares one UBO (binding = 0) across stages.
layout(std140, binding = 0) uniform buf {
    mat4  qt_Matrix;
    float qt_Opacity;
    vec4  b0;
    vec4  b1;
    vec4  b2;
    vec4  b3;
    vec4  p0;
    vec4  p1;
    vec4  p2;
    vec4  p3;
    vec4  oceanColor;
    vec4  skyColor;
    float bandDecay;
    float segDecay;
    float barGap;
    float segGap;
    float taperK;
    float segCountF;
    float bandCountF;
};

void main() {
    qt_TexCoord0 = qt_MultiTexCoord0;
    gl_Position  = qt_Matrix * qt_Vertex;
}
