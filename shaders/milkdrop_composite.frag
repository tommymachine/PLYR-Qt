// Composite fragment — Layer 3b.
//
// Reads the warp-pass output texture and applies an optional gamma
// correction before writing to the visible target. For v1 the gamma
// path is identity (gamma = 1.0); leaving the uniform in place so
// real-preset MilkDrop's `gamma` variable has somewhere to land
// without requiring a shader rebake.

#version 440

layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4  qt_Matrix;
    float qt_Opacity;
    float gamma;
    float _pad0;
    float _pad1;
};

layout(binding = 1) uniform sampler2D warpedTex;

void main()
{
    vec4 c = texture(warpedTex, v_uv);

    // Apply gamma if explicitly non-1.0. Cheap branch — predictable
    // because every fragment evaluates the same branch.
    if (abs(gamma - 1.0) > 1e-4) {
        c.rgb = pow(max(c.rgb, vec3(0.0)), vec3(1.0 / max(gamma, 1e-3)));
    }
    fragColor = vec4(c.rgb, 1.0) * qt_Opacity;
}
