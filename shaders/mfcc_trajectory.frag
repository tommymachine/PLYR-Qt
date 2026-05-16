// Fragment stage for the MFCC trajectory visualizer (Layer 2c).
//
// Per pixel:
//   1. Discard filler-segment quads (v_age < 0).
//   2. AA edge: |lateral| approaches 1 at the line edge; smoothstep
//      shapes the fall-off so the line has soft borders.
//   3. Age-based fade: alpha = sqrt(age) * exp(-(1-age) * 0.6) so the
//      newest segments are bright and the oldest fade smoothly to zero.
//   4. Color mix from tailColor (age=0) to headColor (age=1).
//   5. Depth fog (clip-space z) so far-side curves recede into the
//      background instead of overlapping the foreground.
//   6. Output premultiplied-alpha RGBA -- the host pipeline blends as
//      One/OneMinusSrcAlpha.

#version 440

layout(location = 0) in  float v_age;
layout(location = 1) in  float v_lateral;
layout(location = 2) in  float v_clipZ;

layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4  viewProj;
    vec2  viewportPx;
    float lineWidth;
    float maxAge;
    vec4  headColor;
    vec4  tailColor;
    float fogStart;
    float fogEnd;
};

void main() {
    if (v_age < 0.0) discard;

    // Soft edge AA. v_lateral spans -1..+1 across the extruded quad.
    // The line proper sits in roughly +- (lineWidth / (lineWidth + 1.5)),
    // and the remainder is the AA roll-off band.
    float aaWidth = clamp(1.5 / (lineWidth + 1.5), 0.0, 0.9);
    float edge    = 1.0 - abs(v_lateral);
    float aa      = smoothstep(0.0, aaWidth, edge);

    // Age fade. The exponent gives the recent trail more contrast against
    // the older tail; pow(0.5) on age also lifts mid-range alphas so a
    // ~50%-old segment is still legible.
    float ageT = clamp(v_age / max(maxAge, 1e-6), 0.0, 1.0);
    float fade = pow(ageT, 0.5);
    // Extra "newest is brightest" boost: small exponential bias toward
    // the recent end of the trail so the head reads as a glowing tip.
    fade *= mix(0.55, 1.0, ageT);

    // Color gradient.
    vec3 col = mix(tailColor.rgb, headColor.rgb, ageT);

    // Depth fog. v_clipZ is NDC z in [-1, 1]; we treat 0 as "near" and
    // higher as "far" (matches our right-handed view-z-forward camera).
    // For the typical 2.8-unit orbit we want the back-side curves to lose
    // ~40% brightness so the spatial structure reads correctly.
    float fogT = clamp((v_clipZ * 0.5 + 0.5 - fogStart) / max(fogEnd - fogStart, 1e-6),
                      0.0, 1.0);
    float fog = mix(1.0, 0.45, fogT);

    float alpha = aa * fade * fog;

    // Premultiplied alpha. Host pipeline blends as src + dst*(1-src.a).
    fragColor = vec4(col * alpha, alpha);
}
