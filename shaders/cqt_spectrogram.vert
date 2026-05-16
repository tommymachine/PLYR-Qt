// Passthrough vertex stage for the CQT spectrogram waterfall (Layer 1c).
// Forwards qt_TexCoord0; all the work happens in cqt_spectrogram.frag.
// Uniform layout repeats the fragment block exactly — Qt Quick binds the
// same UBO (binding=0) to both stages.

#version 440

layout(location = 0) in  vec4 qt_Vertex;
layout(location = 1) in  vec2 qt_MultiTexCoord0;
layout(location = 0) out vec2 qt_TexCoord0;

layout(std140, binding = 0) uniform buf {
    mat4  qt_Matrix;
    float qt_Opacity;
    vec4  lowColor;
    vec4  highColor;
    float scrollOffset;
    float columns;
    float gamma;
    float useMagma;
};

void main() {
    qt_TexCoord0 = qt_MultiTexCoord0;
    gl_Position  = qt_Matrix * qt_Vertex;
}
