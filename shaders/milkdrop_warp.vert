// Milkdrop warp-mesh vertex stage — Layer 3b.
//
// Vertex attributes:
//   loc 0 — a_pos: clip-space mesh vertex in [-1, +1]^2. Fixed grid;
//           never changes across frames.
//   loc 1 — a_uv:  per-vertex sample UV for the previous-frame
//           texture, in [0, 1] (may exceed slightly — sampler clamps).
//           Recomputed every frame by MilkdropRuntime::runPerVertex.
//
// The UBO layout is duplicated in milkdrop_warp.frag — std140 packs
// member offsets purely from declaration order, so the two must match.
// The .cpp renderer uses the *exact same* C struct layout; see
// `WarpUbo` in src/MilkdropView.cpp.
//
// Reference: Geiss's MilkDrop preset spec, "Per-Vertex Equations":
// https://www.geisswerks.com/milkdrop/milkdrop_preset_authoring.html

#version 440

layout(location = 0) in  vec2 a_pos;
layout(location = 1) in  vec2 a_uv;
layout(location = 0) out vec2 v_uv;

layout(std140, binding = 0) uniform buf {
    mat4  qt_Matrix;        // 64 B
    float qt_Opacity;       //  4 B
    float decay;            //  4 B
    float darkenCenter;     //  4 B
    float _pad0;            //  4 B  — align next vec4 to 16
    vec4  waveColor;        // 16 B  — used as global tint when compositing
};

void main()
{
    // The warp pass renders into an off-screen RGBA8 texture sized to
    // the item; qt_Matrix is identity for this pass (we use clip-space
    // positions directly). Still apply it in case the caller wants to
    // transform the mesh — it's effectively free at vertex shader cost.
    v_uv        = a_uv;
    gl_Position = qt_Matrix * vec4(a_pos, 0.0, 1.0);
}
