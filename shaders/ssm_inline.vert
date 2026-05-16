// Passthrough vertex stage for the inline SSM scrubber strip (Layer 4b).
// One row of the matrix is extracted at the playback fraction; the
// fragment shader handles colour-mapping + the now-line.
//
// UBO layout must mirror ssm_inline.frag (binding=0 std140 packing).

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
