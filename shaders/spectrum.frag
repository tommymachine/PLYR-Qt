// SPAN-style spectrum analyzer fragment stage.
//
// Three texture inputs (all 1×N R8, log-x-remapped CPU-side):
//   source       — live curve, [0,1] in display-normalized magnitude
//   peakSource   — decaying peak-hold trace
//   infPeakSource— never-decaying peak-hold trace
//
// Pipeline per pixel (u = x in [0,1], v = 0 at bottom):
//   1. Sample mag at u. Fill the column up to mag with a viridis-mapped
//      colour ramp; smoothstep the top edge for 1.5-pixel AA.
//   2. Draw the peak-hold and infinite-peak traces as thin horizontal
//      lines at their sampled heights.
//   3. Multiply by a subtle bottom-to-top brightness ramp so the bars
//      look like they sit in a dark panel rather than floating.
//
// Viridis polynomial from Inigo Quilez's CC0 ShaderToy
// https://www.shadertoy.com/view/WlfXRN — six-term cubic fit to the
// matplotlib viridis colormap. Cheap enough for per-pixel use.

#version 440

layout(location = 0) in  vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4  qt_Matrix;
    float qt_Opacity;
    vec4  fillColorBottom;     // gradient end (override of viridis bottom)
    vec4  fillColorTop;        // gradient end (override of viridis top)
    vec4  peakColor;
    vec4  infPeakColor;
    vec4  fillTint;            // multiplied into the final fill color
    vec2  viewportPx;          // pixel size of the rendered quad
    float dBMin;               // reserved — currently CPU-side only
    float dBMax;
    float showPeakHoldF;       // 0/1
    float showInfinitePeakF;   // 0/1
    float useViridis;          // 0 = use fillColorBottom/Top gradient,
                               // 1 = use the analytic viridis polynomial
};

layout(binding = 1) uniform sampler2D source;
layout(binding = 2) uniform sampler2D peakSource;
layout(binding = 3) uniform sampler2D infPeakSource;

// Inigo Quilez viridis approximation, CC0. RGB at t=0 ≈ dark purple,
// t=0.5 ≈ green-teal, t=1 ≈ yellow.
vec3 viridis(float t) {
    const vec3 c0 = vec3( 0.2777273272,  0.0054757293,  0.3340700258);
    const vec3 c1 = vec3( 0.1050930431,  1.4046887543,  1.3845596316);
    const vec3 c2 = vec3(-0.3308618287,  0.2148501522,  0.0921353310);
    const vec3 c3 = vec3(-4.6340124486, -5.7991465065, -19.33222751 );
    const vec3 c4 = vec3( 6.2287323709, 14.179847918,   56.69022586 );
    const vec3 c5 = vec3( 4.7763755125,-13.745343033, -65.35305766 );
    const vec3 c6 = vec3(-5.4356886070,  4.6458142200,  26.31254094 );
    return c0 + t*(c1 + t*(c2 + t*(c3 + t*(c4 + t*(c5 + t*c6)))));
}

void main() {
    // Qt forwards y=0 at the TOP of the quad; flip so v=0 sits at the
    // bottom edge of the panel (the "floor" of the analyzer).
    float u = qt_TexCoord0.x;
    float v = 1.0 - qt_TexCoord0.y;

    // Sample the three 1×N curves. We only use the .r channel — these
    // are R8 textures.
    float mag     = texture(source,        vec2(u, 0.5)).r;
    float peakV   = texture(peakSource,    vec2(u, 0.5)).r;
    float infPkV  = texture(infPeakSource, vec2(u, 0.5)).r;

    // Pixel-size in normalized-v units; controls the AA width.
    float pxV = 1.0 / max(1.0, viewportPx.y);

    // ----- Fill ---------------------------------------------------------
    // smoothstep across mag ± 1.5 px gives one pixel of AA on the bar top.
    float fillAlpha = 1.0 - smoothstep(mag - 1.5*pxV, mag + 1.5*pxV, v);

    // Gradient color: either the polynomial viridis (mastering default)
    // or a linear interp between user-supplied endpoints. We tint by v
    // (vertical position in the bar) rather than mag itself; that way a
    // saturated bar reading 1.0 still has a visible color ramp inside it.
    vec3 grad = (useViridis > 0.5)
              ? viridis(v)
              : mix(fillColorBottom.rgb, fillColorTop.rgb, clamp(v, 0.0, 1.0));

    grad *= fillTint.rgb;

    // Sit the bars in a dark panel. The bottom is slightly dimmed so the
    // floor doesn't read as a hard mid-gray block on quiet signals.
    grad *= mix(0.78, 1.0, v);

    vec4 fill = vec4(grad, 1.0) * fillAlpha;

    // ----- Peak-hold line ----------------------------------------------
    // 1-pixel-wide horizontal stripe at v = peakV. We use a small distance-
    // to-line falloff so the line stays crisp even at non-integer pixel
    // coordinates.
    float peakDist  = abs(v - peakV) / max(1.0*pxV, 1e-6);
    float peakAlpha = (showPeakHoldF * (1.0 - smoothstep(0.75, 1.5, peakDist)))
                    * peakColor.a;
    // Hide peak-hold below the live mag — visually we read the peak as
    // floating ABOVE the bar.
    peakAlpha *= step(mag, peakV);

    // Infinite peak: same shape, distinct color, drawn ON TOP of the
    // ordinary peak-hold so a frozen amber line wins where they coincide.
    float infDist   = abs(v - infPkV) / max(1.0*pxV, 1e-6);
    float infAlpha  = (showInfinitePeakF * (1.0 - smoothstep(0.75, 1.5, infDist)))
                    * infPeakColor.a;
    infAlpha *= step(mag, infPkV);

    // ----- Compose -----------------------------------------------------
    // Composite is straight-alpha over: fill first, peak line atop, infinite-
    // peak atop that. We pre-multiply outputs so a Qt Quick BlendMode source-
    // over compositing behaves naturally.
    vec3  out_rgb  = fill.rgb;
    float out_a    = fill.a;

    out_rgb = mix(out_rgb, peakColor.rgb,    peakAlpha);
    out_a   = max(out_a, peakAlpha);

    out_rgb = mix(out_rgb, infPeakColor.rgb, infAlpha);
    out_a   = max(out_a, infAlpha);

    fragColor = vec4(out_rgb * out_a, out_a) * qt_Opacity;
}
