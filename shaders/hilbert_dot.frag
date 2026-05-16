// Fragment stage for the Hilbert-rosette dot splats.
//
// Computes a Gaussian intensity from the pixel-space offset to the dot
// center, looks up the per-band hue from the UBO color array, and emits
// premultiplied-alpha RGBA so the additive accumulate (one * one)
// composes cleanly. The vertex stage already corner-clipped at
// 3*sigma; here we just evaluate exp(-d^2 / (2*sigma^2)).
//
// Color tip: the dot's hue is the band's hue (HSV around the wheel) and
// the brightness is the Gaussian intensity. Saturated splats reading
// near +1 in any single channel let the per-band hue stay legible even
// when multiple dots overlap.

#version 440

layout(location = 0) in  vec2  v_off;
layout(location = 1) in  float v_bandIdx;

layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    vec2  viewportPx;
    float sigma;
    float intensity;
    vec4  bandColor[8];
};

void main() {
    float r2 = dot(v_off, v_off);
    float k  = max(sigma * sigma, 1.0);
    float g  = exp(-r2 / (2.0 * k));

    int   idx = int(clamp(v_bandIdx + 0.5, 0.0, 7.0));   // round to nearest
    vec4  col = bandColor[idx];

    float a   = intensity * g;
    fragColor = vec4(col.rgb * a, a);
}
