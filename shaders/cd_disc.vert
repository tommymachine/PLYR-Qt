// Passthrough vertex shader for the CD disc surface. All work is done
// per-pixel in cd_disc.frag; this just forwards qt_TexCoord0 and the
// matrix/opacity Qt Quick provides.

#version 440

layout(location = 0) in  vec4 qt_Vertex;
layout(location = 1) in  vec2 qt_MultiTexCoord0;
layout(location = 0) out vec2 qt_TexCoord0;

// Must mirror cd_disc.frag's UBO layout exactly — Qt Quick shares one
// UBO (binding = 0) across stages, and std140 packing means every
// uniform's offset is fixed by the declaration order above it.
layout(std140, binding = 0) uniform buf {
    mat4  qt_Matrix;
    float qt_Opacity;
    float axisAngle;       // orientation of the light's diameter axis
    float lightRadius;     // fixed offset of the light along the axis
    float viewTiltX;       // animated viewer-tilt component (x)
    float viewTiltY;       // animated viewer-tilt component (y)
    float readProgress;
    // CD anatomy (radii normalised to disc-radius = 1.0, per ECMA-130
    // §8). Real CD outer radius is 60 mm, so 1.0 ≡ 60 mm.
    float hubR;            // 0.125 — spindle hole (real 7.5 mm)
    float clearOuter;      // 0.275 — clear polycarbonate ends (16.5 mm)
    float mirrorOuter;     // 0.382 — mirror band ends, pits begin (22.9 mm)
    float dataInner;       // 0.417 — program area starts (25 mm)
    float dataOuter;       // 0.967 — program area ends (58 mm)
    float fringeSpacing;
    float iridescenceMix;
    float specularStrength;
};

void main() {
    qt_TexCoord0 = qt_MultiTexCoord0;
    gl_Position  = qt_Matrix * qt_Vertex;
}
