// Chladni vibrational-mode plate -- Layer 4d (stateless display).
//
// A vibrating rectangular plate has standing-wave eigenmodes
//     u_{m,n}(x, y) = cos(m*pi*x) * cos(n*pi*y)
// and a uniform pile of sand on the plate accumulates along the zero
// crossings of the superposed modes (sand grains slide downhill from
// every non-zero-displacement point). The classic "interference" form
// that produces the famous geometric figures is
//     u(x, y) = cos(n*pi*x) cos(m*pi*y) - cos(m*pi*x) cos(n*pi*y)
// for two integer mode numbers (m, n). The visible pattern is the zero
// set of u.
//
// Audio reactivity (no feedback buffer needed -- this is stateless):
//   * (m, n) drift slowly toward integer mode numbers derived from the
//     spectral centroid (low centroid -> low modes, high centroid ->
//     high modes). The host smooths the integer drift.
//   * bass_att introduces a gentle vertical perturbation (the
//     simulation is otherwise time-invariant, so without this the
//     pattern would freeze when the host parameters don't change).
//   * rms_att scales the overall pattern brightness so silence darkens
//     the screen.
//
// Reference (math only): Chladni's 1787 _Entdeckungen ueber die Theorie
// des Klanges_. Modern derivation: any introductory continuum-mechanics
// text. The closed-form (m,n)-mode superposition trick is folklore in
// the shader-art community (e.g. Helfman's Observable notebook); the
// math is uncopyrightable.

#version 440

layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4  qt_Matrix;
    float qt_Opacity;
};

layout(std140, binding = 1) uniform chladniBuf {
    float m;          // mode number (real, slow-drifted)
    float n;          // mode number (real, slow-drifted)
    float bass;       // bass_att for the audio-driven nudge
    float mid;
    float treb;
    float rms;        // rms_att for overall brightness
    float time;       // seconds since pane creation
    float lineWidth;  // controls how wide the rendered zero-crossings are
    vec4  lineColor;  // line / pattern colour
    vec4  bgColor;    // background fill
};

#define PI 3.14159265358979

void main()
{
    // Treat the plate as a unit square; v_uv is already in [0,1].
    vec2 p = v_uv;

    // Closed-form (m,n) standing-wave interference.
    float u = cos(n * PI * p.x) * cos(m * PI * p.y)
            - cos(m * PI * p.x) * cos(n * PI * p.y);

    // Subtle bass-driven time perturbation so the figure breathes during
    // sustained tones (otherwise the pattern is static between mode
    // changes). The frequency is BPM-like (~120 BPM = 2 Hz baseline) and
    // bass_att modulates amplitude.
    u += 0.05 * sin(time * 2.0 * PI * 2.0) * (bass + 0.05);

    // Treble adds high-spatial-frequency speckle along the existing zero
    // crossings -- looks like grains of sand bouncing on the plate.
    u += 0.02 * treb * cos(p.x * 32.0 * PI) * cos(p.y * 32.0 * PI);

    // Width of the zero-crossing band: tighter when lineWidth is small.
    // 0.06 is a good default; the lineWidth uniform scales it.
    float band = max(1e-3, 0.06 * lineWidth);
    float intensity = 1.0 - smoothstep(0.0, band, abs(u));
    intensity = pow(intensity, 2.5);     // sharpen edges -- pattern reads as line, not blur

    // RMS scales overall brightness so quiet music dims the figure.
    intensity *= (0.25 + 0.75 * rms);

    vec3 col = mix(bgColor.rgb, lineColor.rgb, intensity);
    fragColor = vec4(col, 1.0);
}
