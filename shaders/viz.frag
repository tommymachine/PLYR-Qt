// PLYR visualizer, fragment stage.
//
// For every pixel we invert two geometric series:
//   1. Which band slot (0..BAND-1) contains our u?
//      Slot i starts at (1 - bandDecay^i) / bandDenom.
//   2. Which segment slot (0..SEG-1) contains our v (y=0 at bottom)?
//      Slot j starts at (1 - segDecay^j) / segDenom.
//
// Then we ask three questions:
//   - Are we inside the drawn portion of this segment (not the gap)?
//   - Is this segment lit (segment mid ≤ band magnitude) OR is it the
//     current peak-hold for this band?
//   - Are we within the tapered bar width at this height?
//
// If all three are yes, we color by a segment-index → ocean/sky ramp
// (peak segments grayscale the result). Otherwise the pixel is black.
//
// This keeps the same geometry and dynamics as the original Canvas
// version but moves the per-segment work off the CPU — the full 16×110
// grid is resolved in one parallel pass on the GPU.

#version 440

layout(location = 0) in  vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4  qt_Matrix;
    float qt_Opacity;
    vec4  b0;
    vec4  b1;
    vec4  b2;
    vec4  b3;
    vec4  p0;
    vec4  p1;
    vec4  p2;
    vec4  p3;
    vec4  oceanColor;
    vec4  skyColor;
    float bandDecay;
    float segDecay;
    float barGap;
    float segGap;
    float taperK;
    float segCountF;
    float bandCountF;
};

float getBand(int i) {
    if (i < 4)  return b0[i];
    if (i < 8)  return b1[i - 4];
    if (i < 12) return b2[i - 8];
    return b3[i - 12];
}
float getPeak(int i) {
    if (i < 4)  return p0[i];
    if (i < 8)  return p1[i - 4];
    if (i < 12) return p2[i - 8];
    return p3[i - 12];
}

void main() {
    // qt_TexCoord0.y = 0 is top of the quad; we want v = 0 at bottom.
    float u = qt_TexCoord0.x;
    float v = 1.0 - qt_TexCoord0.y;

    // ----- which band slot contains `u`? -----
    float bandDenom  = 1.0 - pow(bandDecay, bandCountF);
    float bandArg    = 1.0 - u * bandDenom;
    if (bandArg <= 0.0) { fragColor = vec4(0.0); return; }
    float fi         = log(bandArg) / log(bandDecay);
    int   bi         = int(clamp(floor(fi), 0.0, bandCountF - 1.0));

    float bDi        = pow(bandDecay, float(bi));
    float slotLeft   = (1.0 - bDi) / bandDenom;
    float slotW      = bDi * (1.0 - bandDecay) / bandDenom;
    float slotCenter = slotLeft + slotW * 0.5;

    // ----- which segment slot contains `v`? -----
    float segDenom = 1.0 - pow(segDecay, segCountF);
    float segArg   = 1.0 - v * segDenom;
    if (segArg <= 0.0) { fragColor = vec4(0.0); return; }
    float fj       = log(segArg) / log(segDecay);
    int   si       = int(clamp(floor(fj), 0.0, segCountF - 1.0));

    float sDi   = pow(segDecay, float(si));
    float sDi1  = pow(segDecay, float(si + 1));
    float segBottom  = (1.0 - sDi)  / segDenom;
    float segTop     = (1.0 - sDi1) / segDenom;
    float segH       = segTop - segBottom;
    float segFillTop = segBottom + segH * (1.0 - segGap);
    float segMid     = (segBottom + segTop) * 0.5;

    // Vertical gap between segment caps — inside, we're transparent.
    if (v >= segFillTop) { fragColor = vec4(0.0); return; }

    // Lit OR peak-marker at this slot?
    float mag = getBand(bi);
    float pk  = getPeak(bi);
    bool lit    = (segMid <= mag);
    bool isPeak = (pk > 0.001) && (pk >= segBottom) && (pk < segTop);
    if (!lit && !isPeak) { fragColor = vec4(0.0); return; }

    // Horizontal taper — narrow bars at the top, full width at the bottom.
    float yNorm = 1.0 - segMid;            // = 1 at bottom, → 0 at top
    float taper = exp(-taperK * (1.0 - yNorm));
    float halfW = slotW * (1.0 - barGap) * 0.5 * taper;
    if (abs(u - slotCenter) > halfW) { fragColor = vec4(0.0); return; }

    // Color by vertical position: ocean (bottom) → sky (top).
    float t    = float(si) / (segCountF - 1.0);
    vec3  base = mix(oceanColor.rgb, skyColor.rgb, t);
    if (isPeak) {
        // Peak-hold ticks are grayscale (same luma as the gradient here).
        float Y = 0.2126 * base.r + 0.7152 * base.g + 0.0722 * base.b;
        base = vec3(Y);
    }
    fragColor = vec4(base * qt_Opacity, qt_Opacity);
}
