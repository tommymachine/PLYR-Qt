// Passthrough vertex stage for the Polar Spectrum ShaderToy-style pane.
// All real work is per-pixel in the fragment shader; this just forwards
// qt_TexCoord0 and applies Qt Quick's qt_Matrix.
//
// The UBO layout below must mirror the matching fragment shader exactly —
// Qt Quick binds one UBO at binding = 0 across both stages, and std140
// packing fixes every member's offset by declaration order. Every shader
// in the Layer 3a pack uses the same uniform layout so a single QML
// ShaderEffect can swap fragmentShader URLs without remapping uniforms.

#version 440

layout(location = 0) in  vec4 qt_Vertex;
layout(location = 1) in  vec2 qt_MultiTexCoord0;
layout(location = 0) out vec2 qt_TexCoord0;

layout(std140, binding = 0) uniform buf {
    mat4  qt_Matrix;
    float qt_Opacity;
    float iTime;
    vec3  iResolution;
    vec4  iMouse;
    float bass;
    float mid;
    float treb;
    float rms;
    float cent;
    float flux;
};

void main() {
    qt_TexCoord0 = qt_MultiTexCoord0;
    gl_Position  = qt_Matrix * qt_Vertex;
}
