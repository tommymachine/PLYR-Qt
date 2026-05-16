// Vertex stage for the analytic-line-integral scope.
//
// One quad per segment between adjacent scope samples. The quad bounds
// the segment plus a 3·sigma margin so the fragment shader's Gaussian
// integral has room to fall off smoothly at every edge.
//
// Coordinate convention: per-segment endpoints (p0, p1) and the per-vertex
// expanded `v_pos` are all in pixel-space relative to the bottom-left of
// the accumulation render target. `viewportPx` carries that size so the
// fragment stage can convert pixel distances into a Gaussian σ in pixels.
// Clip space is computed from the pixel positions at the end of main().

#version 440

layout(location = 0) in  vec2 a_p0;        // segment start, px
layout(location = 1) in  vec2 a_p1;        // segment end,   px
layout(location = 2) in  vec2 a_corner;    // quad corner in [-1, +1]

layout(location = 0) out vec2 v_pos;       // px-space frag position
layout(location = 1) out vec2 v_p0;
layout(location = 2) out vec2 v_p1;
layout(location = 3) out float v_segLen;

layout(std140, binding = 0) uniform buf {
    mat4  qt_Matrix;        // unused; we build our own ortho projection
    float qt_Opacity;       // unused
    vec2  viewportPx;       // accumulation buffer size in px
    float sigma;            // Gaussian beam width, in px
    float intensity;        // overall gain (premultiplied by color in frag)
};

void main() {
    vec2  segDir  = a_p1 - a_p0;
    float segLen  = length(segDir);
    vec2  tangent = segLen > 1e-6 ? segDir / segLen : vec2(1.0, 0.0);
    vec2  normal  = vec2(-tangent.y, tangent.x);

    // Quad half-extents in segment-local coords, in pixel units. The
    // 3·σ margin is what gives the Gaussian its smooth roll-off at the
    // quad edges; without it, the beam looks rectangle-clipped.
    float margin    = 3.0 * sigma;
    float halfLen   = 0.5 * segLen + margin;
    float halfWidth = margin;

    vec2 center = 0.5 * (a_p0 + a_p1);
    vec2 posPx  = center
                + a_corner.x * halfLen   * tangent
                + a_corner.y * halfWidth * normal;

    // Pixel-space → NDC. (0,0) maps to (-1,-1); (W,H) maps to (+1,+1).
    vec2 ndc = (posPx / viewportPx) * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);

    v_pos    = posPx;
    v_p0     = a_p0;
    v_p1     = a_p1;
    v_segLen = segLen;
}
