// Vertex stage for the MFCC trajectory visualizer (Layer 2c).
//
// One quad per inter-point segment. Each segment carries the two 3D
// endpoints, the two endpoint ages, and a corner identifier in
// [-1, +1]^2. The shader:
//   1. Projects both endpoints to clip space via viewProj.
//   2. Performs the perspective divide to get NDC.
//   3. Computes a screen-space perpendicular to the segment.
//   4. Expands each corner outward by lineWidth/viewportPx so the
//      segment shows up as a thick line on screen, with constant pixel
//      width regardless of camera distance.
//
// Degenerate segments (oldest filler frames before the buffer has data,
// or two coincident points) collapse to a zero-area quad at the origin;
// the negative ages we use for filler segments tell the fragment shader
// to discard them.

#version 440

layout(location = 0) in  vec3  a_p0;
layout(location = 1) in  vec3  a_p1;
layout(location = 2) in  float a_ageA;
layout(location = 3) in  float a_ageB;
layout(location = 4) in  vec2  a_corner;     // (-1..+1, -1..+1)

layout(location = 0) out float v_age;        // interpolated along segment
layout(location = 1) out float v_lateral;    // -1..+1 across line width
layout(location = 2) out float v_clipZ;      // for fog

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
    // Filler segment (no MFCC data yet) -- emit a degenerate quad at the
    // origin. The negative age propagates to the fragment shader, which
    // discards the pixel.
    if (a_ageA < 0.0) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);   // outside NDC
        v_age = -1.0;
        v_lateral = 0.0;
        v_clipZ = 0.0;
        return;
    }

    vec4 clipA = viewProj * vec4(a_p0, 1.0);
    vec4 clipB = viewProj * vec4(a_p1, 1.0);

    // Pick which endpoint this corner belongs to: corner.x = -1 -> A,
    // corner.x = +1 -> B. The 'cornerEnd' clip-space position is the
    // anchor; the perpendicular extrusion uses both endpoints' screen
    // positions so the line is straight even when the perspective makes
    // the two NDC positions different distances from the camera.
    vec4 clipEnd = (a_corner.x < 0.0) ? clipA : clipB;
    float ageEnd = (a_corner.x < 0.0) ? a_ageA : a_ageB;

    // NDC positions of both endpoints in screen space (pixel units).
    vec2 ndcA = clipA.xy / max(clipA.w, 1e-6);
    vec2 ndcB = clipB.xy / max(clipB.w, 1e-6);
    vec2 pxA = (ndcA * 0.5 + 0.5) * viewportPx;
    vec2 pxB = (ndcB * 0.5 + 0.5) * viewportPx;

    vec2 dir = pxB - pxA;
    float len = length(dir);
    vec2 tangent = (len > 1e-6) ? dir / len : vec2(1.0, 0.0);
    vec2 normal  = vec2(-tangent.y, tangent.x);

    // Lateral offset in pixels. corner.y = -1 / +1 picks the two sides
    // of the line. lineWidth includes a 1-pixel AA margin -- we extrude
    // slightly more than lineWidth/2 so the fragment shader can fade
    // smoothly to zero at the edge.
    float halfW = 0.5 * (lineWidth + 1.5);
    vec2  offPx = a_corner.y * halfW * normal;

    // Add a tiny tangential extension at the segment caps so the corner
    // quads from neighbouring segments overlap and we don't see gaps on
    // sharp turns. 0.5 px on each side is enough.
    offPx += a_corner.x * 0.5 * tangent;

    // Project the offset back into clip space at this endpoint's depth.
    // Multiply the NDC offset by clipEnd.w so the perspective divide
    // recovers the original pixel offset.
    vec2 offNdc = (offPx / viewportPx) * 2.0;
    vec2 cornerNdc = (a_corner.x < 0.0 ? ndcA : ndcB) + offNdc;

    // Reassemble clip position. Multiply by the anchor w so the
    // post-vertex perspective divide reproduces our cornerNdc.
    gl_Position = vec4(cornerNdc * clipEnd.w, clipEnd.z, clipEnd.w);

    // Interpolant outputs.
    v_age     = ageEnd;
    v_lateral = a_corner.y;
    // Use clip-space z linearly normalized (-1..+1 in NDC); we shift to
    // 0..1 in the fragment shader for the fog computation.
    v_clipZ   = clipEnd.z / max(clipEnd.w, 1e-6);
}
