// Gray-Scott reaction-diffusion solver pass -- Layer 4d.
//
// Solves the coupled PDE system
//     du/dt = Du * laplacian(u) - u*v^2 + F*(1-u)
//     dv/dt = Dv * laplacian(v) + u*v^2 - (F + k)*v
// on a 2D torus (texture wrap) with forward Euler integration. One
// fragment shader invocation = one cell update; the host runs many
// substeps per visible frame via texture ping-pong.
//
// Reference (math only, no copied code):
//   Pearson, J.E. "Complex Patterns in a Simple System", *Science* 261
//   (1993), pp. 189-192; based on Gray & Scott's 1984 derivation.
//   Karl Sims' write-up at https://www.karlsims.com/rd.html documents
//   the (F, k) parameter map this implementation targets.
//
// (F, k) regime quick-reference:
//   0.014 / 0.054 -- gliders ("Newton's-cradle" waves)
//   0.022 / 0.051 -- labyrinth stripes
//   0.030 / 0.062 -- mitosis spots         (default)
//   0.046 / 0.063 -- coral / bubble growth
//   0.060 / 0.062 -- worms
//
// 5-point Laplacian. The 9-point Sims variant has better grid-isotropy
// (corners weighted 0.05, edges 0.20, center -1.0); 5-point is cheaper
// per fragment and at 256-512 px grids the anisotropy is invisible.
//
// Input state texture: RGBA16F.
//   .r = u
//   .g = v
//   .ba unused

#version 440

layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4  qt_Matrix;
    float qt_Opacity;
};

layout(std140, binding = 1) uniform pdeBuf {
    vec2  invResolution;     // 1 / (gridW, gridH); used as neighbour offset
    float feed;              // F
    float kill;              // k
    float Du;                // u diffusion coefficient
    float Dv;                // v diffusion coefficient
    float dt;                // integration time step (per substep)
    float _pad0;
    vec4  fluxImpulse;       // xy = impulse center in [0,1], z = strength flag, w unused
};

layout(binding = 2) uniform sampler2D state;

void main()
{
    vec2 c = texture(state, v_uv).rg;
    float u = c.r;
    float v = c.g;

    // 5-point Laplacian. We sample neighbours via explicit UV offsets
    // instead of textureOffset() so the shader bakes cleanly across all
    // RHI backends (Qt's shader baker rejects textureOffset for cross-
    // target portability). The sampler is configured with Repeat
    // addressing host-side so the lookups wrap at the borders -- the
    // Gray-Scott PDE on a continuous medium is mathematically a torus.
    vec2 r  = c;
    vec2 dx = vec2(invResolution.x, 0.0);
    vec2 dy = vec2(0.0, invResolution.y);
    vec2 n0 = texture(state, v_uv + dx).rg;
    vec2 n1 = texture(state, v_uv - dx).rg;
    vec2 n2 = texture(state, v_uv + dy).rg;
    vec2 n3 = texture(state, v_uv - dy).rg;
    vec2 lap = n0 + n1 + n2 + n3 - 4.0 * r;

    float du = Du * lap.r - u * v * v + feed * (1.0 - u);
    float dv = Dv * lap.g + u * v * v - (feed + kill) * v;

    float u_new = u + dt * du;
    float v_new = v + dt * dv;

    // Optional flux impulse: a soft circular bump that bumps v upward.
    // .z is a "fire this step" flag; .xy is the impulse center in [0,1].
    // The radius is fixed in UV space at ~2% of the screen -- audible-
    // beat-sized perturbation that the diffusion will propagate over the
    // following ~100 frames.
    if (fluxImpulse.z > 0.5) {
        float d = distance(v_uv, fluxImpulse.xy);
        float bump = exp(-d * d * 2500.0);   // ~2% radius (sigma ~ 0.02)
        v_new = clamp(v_new + bump * 0.8, 0.0, 1.0);
        u_new = clamp(u_new - bump * 0.4, 0.0, 1.0);
    }

    // Clamp -- Gray-Scott on a continuous medium keeps u, v in [0,1] by
    // construction; numerical noise can push 16-bit half-floats slightly
    // outside, which becomes visible as flickering pixels at the edges.
    u_new = clamp(u_new, 0.0, 1.0);
    v_new = clamp(v_new, 0.0, 1.0);

    fragColor = vec4(u_new, v_new, 0.0, 1.0);
}
