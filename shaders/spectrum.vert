// Passthrough vertex stage for the spectrum analyzer. Forwards qt_TexCoord0
// to the fragment shader; per-pixel work is all in spectrum.frag.
//
// Must repeat the uniform layout used by the fragment stage — Qt Quick
// binds the UBO at the same point (binding = 0) for both stages.

#version 440

layout(location = 0) in  vec4 qt_Vertex;
layout(location = 1) in  vec2 qt_MultiTexCoord0;
layout(location = 0) out vec2 qt_TexCoord0;

layout(std140, binding = 0) uniform buf {
    mat4  qt_Matrix;
    float qt_Opacity;
    vec4  fillColorBottom;
    vec4  fillColorTop;
    vec4  peakColor;
    vec4  infPeakColor;
    vec4  fillTint;
    vec2  viewportPx;
    float dBMin;
    float dBMax;
    float showPeakHoldF;
    float showInfinitePeakF;
    float useViridis;
};

void main() {
    qt_TexCoord0 = qt_MultiTexCoord0;
    gl_Position  = qt_Matrix * qt_Vertex;
}
