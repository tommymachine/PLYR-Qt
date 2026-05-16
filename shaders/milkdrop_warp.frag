// Milkdrop warp-mesh fragment stage — Layer 3b.
//
// Samples the previous-frame texture at the per-vertex-computed UV,
// applies `decay` (exponential fade per frame), and tints with
// `waveColor` to keep the buffer from going monochrome.
//
// Two things to note about Milkdrop colour math:
//
//   1. `decay` is multiplicative per frame. 1.0 = no fade (image
//      accumulates forever). 0.96 = ~93% retention after 30 frames /
//      half-second at 60 Hz. Below ~0.85 the image fades to black
//      visibly fast; above ~0.99 it doesn't really fade at all.
//
//   2. `waveColor` in this implementation is repurposed as a global
//      colour shift. In real MilkDrop it tints just the waveform line;
//      we don't render the waveform in v1, so we route the
//      wave_r/g/b/a slots into a subtle additive tint that follows
//      whatever the preset's wave-colour expression decided.
//
// Reference: Geiss spec, "Frame Equations / Composite Equations":
// https://www.geisswerks.com/milkdrop/milkdrop_preset_authoring.html

#version 440

layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4  qt_Matrix;
    float qt_Opacity;
    float decay;
    float darkenCenter;
    float _pad0;
    vec4  waveColor;
};

layout(binding = 1) uniform sampler2D prevFrame;

void main()
{
    // Wrap UVs to [0, 1] — Geiss's spec recommends wrap=1 / texwrap=1
    // for most presets; the sampler is set to ClampToEdge in the host
    // (Repeat would be more faithful, but introduces seams at the mesh
    // boundary when a preset has cx ≠ 0.5). We use fract() here to
    // emulate repeat-sampling regardless of sampler state — produces
    // the toroidal continuity Geiss's reference renderer shows.
    vec2 uv = fract(v_uv);

    vec4 prev = texture(prevFrame, uv);

    // Multiplicative decay. Per-channel keeps colour balance; if the
    // preset wants tinting it does it via waveColor.
    vec3 col = prev.rgb * decay;

    // darkenCenter: optional vignette that fades the centre pixels
    // faster. Geiss's docs describe this as a slow dark spot — the
    // exponent makes it falloff gently.
    if (darkenCenter > 0.5) {
        // sample's *destination* pixel position would be ideal; we
        // approximate from v_uv (which is the SOURCE), which gives
        // the same visual region (centre stays centre under symmetric
        // warps).
        vec2 d = v_uv - vec2(0.5);
        float r = dot(d, d) * 4.0;          // 0 at centre, 1 at corners
        float darken = mix(0.92, 1.0, smoothstep(0.0, 0.6, r));
        col *= darken;
    }

    // Subtle additive tint from waveColor. We don't render waveforms,
    // so the wave RGBA is used purely as a colour bias. The 0.02
    // scale keeps it imperceptible-but-present unless the preset
    // really cranks wave_a; that prevents the screen from drifting
    // entirely grey on long-decay presets.
    col += waveColor.rgb * waveColor.a * 0.02;

    fragColor = vec4(col, 1.0) * qt_Opacity;
}
