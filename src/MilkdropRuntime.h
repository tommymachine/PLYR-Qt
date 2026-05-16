// MilkdropRuntime — Layer 3b. A thin C++ wrapper around the
// MIT-licensed projectm-eval expression evaluator
// (https://github.com/projectM-visualizer/projectm-eval), exposing just
// enough of the MilkDrop / NullSoft NS-EEL2 surface area to drive an
// audio-reactive warp-mesh visualizer.
//
// Design notes:
//
//   * Non-QObject by design. The renderer (MilkdropView, a QQuickRhiItem)
//     owns one runtime instance and pumps it from the render thread. We
//     do not want signal/slot machinery or Qt threading state polluting
//     the hot per-frame path. The only Qt include here is <QString> /
//     <QByteArray> for paths and the parser input.
//
//   * The MilkDrop preset language is, at its core, three concatenated
//     ns-eel2 expressions:
//
//         per_frame_init  — run once when the preset loads
//         per_frame       — run once per video frame (typically 60 Hz)
//         per_vertex      — run once per warp-mesh vertex (32×24 = 768/frame)
//
//     Each is a sequence of "statement;" assignments operating on a
//     shared pool of named scalar variables. Variables that exist in the
//     ns-eel2 namespace get a `PRJM_EVAL_F*` (= `double*`) pointer
//     handed out by projectm-eval; the wrapper keeps that pointer so the
//     host can write live values (bass/mid/treb/time/etc.) before each
//     evaluation pass and read derived values (decay/zoom/cx/cy/q1-q8/etc.)
//     afterwards. Wiring is case-insensitive on the projectm-eval side.
//
//   * Geiss's MilkDrop preset spec
//     (https://www.geisswerks.com/milkdrop/milkdrop_preset_authoring.html)
//     is the source of truth for variable semantics. We wire the most
//     commonly-used subset — see MilkdropRuntime::VarSet for the exact
//     list. Anything in a real .milk preset that we don't recognise
//     becomes a private ns-eel2 variable, fine to write but doesn't
//     affect rendering.
//
//   * No per-frame allocation. The variable-pointer arrays are sized
//     once at preset load; runPerFrame/runPerVertex only execute
//     projectm-eval-compiled bytecode, which is itself allocation-free.

#pragma once

#include <QByteArray>
#include <QString>

#include <array>
#include <memory>
#include <unordered_map>

struct projectm_eval_context;
struct projectm_eval_code;

class MilkdropRuntime {
public:
    // Warp-mesh dimensions are fixed at the renderer level; the runtime
    // is told the dimensions at evaluation time. 32×24 = 768 vertices
    // matches Geiss's "low-detail" preset default. Higher resolutions
    // are fine for the projectm-eval evaluator (it's allocation-free per
    // call), but the renderer's vertex/index buffers are sized for this.
    static constexpr int kDefaultMeshW = 32;
    static constexpr int kDefaultMeshH = 24;

    MilkdropRuntime();
    ~MilkdropRuntime();

    MilkdropRuntime(const MilkdropRuntime&) = delete;
    MilkdropRuntime& operator=(const MilkdropRuntime&) = delete;

    // Load a preset from a file (path may be a filesystem path or a
    // qrc:/ URL — we resolve both via QFile). On success the per-frame-
    // init expression is executed once; on failure lastError() is
    // populated and the prior preset (if any) is wiped. Calling load*
    // again replaces whatever was previously loaded.
    bool loadPresetFromFile(const QString& path);
    bool loadPresetFromString(const QByteArray& contents);

    // Feed live audio. All six are in the MilkDrop "1.0 = average" scale:
    // bass/mid/treb are the raw values, _att are envelope-followed. Call
    // before runPerFrame().
    void setAudio(float bass, float mid, float treb,
                  float bass_att, float mid_att, float treb_att);

    // Advance internal time/frame/progress. dt is in seconds since the
    // last advance() call. `progress` is set externally only if the host
    // wants a real progress value; otherwise it stays at 0.
    void advance(float dt);

    // Run per-frame. Writes derived state (decay/zoom/cx/cy/q1..q8/etc.)
    // into the bound variable storage; per_vertex reads from there.
    void runPerFrame();

    // Run per-vertex. For each of mesh_w * mesh_h grid points, computes
    // the texture-coordinate offset and writes (u, v) into outUv (which
    // must hold 2 * mesh_w * mesh_h floats; (u,v) pairs interleaved,
    // row-major from top-left).
    //
    // The (u, v) values are nominally in [0, 1] but may exceed it
    // slightly — the renderer clamps via sampler wrap mode. Each call
    // resets the per-vertex `dx/dy/zoom/rot/cx/cy/sx/sy` defaults to
    // their per-frame values before evaluating the per-vertex
    // expression; that's the canonical MilkDrop behaviour
    // (per-frame writes the defaults, per-vertex perturbs them).
    void runPerVertex(int meshW, int meshH, float* outUv);

    // ---- Frame-level outputs (read after runPerFrame) ----------------
    // All in MilkDrop variable semantics; see Geiss's spec.

    float decay()      const;       // exponential pixel decay 0..1 (0.96 typical)
    float gamma()      const;       // final gamma multiplier
    float echo_zoom()  const;       // echo-layer zoom factor
    float echo_alpha() const;       // echo-layer alpha
    float wave_r()     const;       // wave color RGBA (we use as global tint)
    float wave_g()     const;
    float wave_b()     const;
    float wave_a()     const;
    float darken_center() const;    // 0 = off, 1 = on; nudges centre fade
    float brighten()   const;       // boolean-ish brighten toggle
    float darken()     const;       // boolean-ish darken toggle
    float invert()     const;       // boolean-ish invert toggle

    // Convenient read-back of q1..q8. The full q1..q32 are wired
    // internally; we expose the first eight that real presets actually
    // use. Indices are 1-based to mirror MilkDrop naming.
    float q(int i) const;

    QString lastError() const { return m_lastError; }
    bool    hasPreset() const { return m_hasPreset; }

    // Test / debugging accessor — returns a pointer to a registered
    // variable's storage so callers can read or pre-set it (e.g. a
    // headless verification harness checking that `q1` updated as
    // expected). nullptr if the name isn't wired. Case-insensitive
    // match against the wired list.
    double* variable(const char* name);

    // The wired-variable enum. Public because the implementation's
    // static name table is built table-driven from this list (one
    // entry per enumerator, see MilkdropRuntime.cpp's kVars[]). Keep
    // it in sync with the kVars[] table — adding a new entry here
    // also requires a kVars[] entry, and vice versa.
    enum Var : int {
        // Time / frame state.
        VAR_TIME = 0,
        VAR_FRAME,
        VAR_FPS,
        VAR_PROGRESS,
        // Mesh / resolution metadata (read-only from preset's POV,
        // we update before each call).
        VAR_MESHX, VAR_MESHY,
        VAR_PIXELSX, VAR_PIXELSY,
        VAR_ASPECTX, VAR_ASPECTY,
        // Audio inputs.
        VAR_BASS, VAR_MID, VAR_TREB,
        VAR_BASS_ATT, VAR_MID_ATT, VAR_TREB_ATT,
        // Per-frame outputs the renderer consumes.
        VAR_DECAY,
        VAR_GAMMA, VAR_GAMMAADJ,
        VAR_ECHO_ZOOM, VAR_ECHO_ALPHA, VAR_ECHO_ORIENT,
        VAR_WAVE_R, VAR_WAVE_G, VAR_WAVE_B, VAR_WAVE_A,
        VAR_WAVE_X, VAR_WAVE_Y,
        VAR_WAVE_MYSTERY, VAR_WAVE_MODE, VAR_WAVE_THICK,
        VAR_WAVE_SMOOTHING, VAR_WAVE_BRIGHTEN, VAR_WAVE_DOTS,
        VAR_DARKEN_CENTER,
        VAR_BRIGHTEN, VAR_DARKEN, VAR_SOLARIZE, VAR_INVERT,
        VAR_WRAP, VAR_TEXWRAP,
        VAR_MV_X, VAR_MV_Y, VAR_MV_DX, VAR_MV_DY,
        VAR_MV_L, VAR_MV_R, VAR_MV_G, VAR_MV_B, VAR_MV_A,
        // Defaults that per-vertex reads + modifies. Set in per-frame,
        // read in per-vertex; per-vertex output drives the warp UV.
        VAR_ZOOM, VAR_ZOOMEXP, VAR_ROT, VAR_CX, VAR_CY, VAR_SX, VAR_SY,
        VAR_WARP, VAR_WARPANIMSPEED, VAR_WARPSCALE,
        // Per-vertex IO.
        VAR_X, VAR_Y, VAR_RAD, VAR_ANG, VAR_DX, VAR_DY,
        // q1..q32 — preset state registers.
        VAR_Q1, VAR_Q2, VAR_Q3, VAR_Q4, VAR_Q5, VAR_Q6, VAR_Q7, VAR_Q8,
        VAR_Q9, VAR_Q10, VAR_Q11, VAR_Q12, VAR_Q13, VAR_Q14, VAR_Q15,
        VAR_Q16, VAR_Q17, VAR_Q18, VAR_Q19, VAR_Q20, VAR_Q21, VAR_Q22,
        VAR_Q23, VAR_Q24, VAR_Q25, VAR_Q26, VAR_Q27, VAR_Q28, VAR_Q29,
        VAR_Q30, VAR_Q31, VAR_Q32,
        // t1..t8 — per-vertex scratch.
        VAR_T1, VAR_T2, VAR_T3, VAR_T4,
        VAR_T5, VAR_T6, VAR_T7, VAR_T8,
        // Misc.
        VAR_RAND, VAR_MONITOR,
        VAR_COUNT
    };

private:
    // Reset/dispose any previously-loaded preset and its compiled code
    // (per_frame_init / per_frame / per_vertex) but keep the eval
    // context and bound variable pointers — those are stable across
    // preset swaps.
    void disposeCompiledPrograms();

    // Allocate the projectm-eval context and register every MilkDrop
    // variable we care about. Idempotent — only runs once, at the
    // first preset load.
    void ensureContext();

    // Reset every wired variable to its default. Called before
    // executing per_frame_init so an unloaded preset doesn't see
    // stale state from the previous preset.
    void resetVariables();

    // Apply the few "header" scalar settings the .milk parser extracts
    // (fDecay, zoom, rot, cx, cy, zoomExp). They become the initial
    // value of the corresponding ns-eel2 variable, which the per-frame
    // expression then has the option of overwriting.
    void applyInitialScalars(const std::unordered_map<QString, double>& kv);

    // Resolve a variable pointer (case-insensitive). Returns nullptr if
    // not wired. Used internally by variable().
    double* lookup(const char* name);

    projectm_eval_context* m_ctx = nullptr;
    projectm_eval_code*    m_codeFrameInit = nullptr;
    projectm_eval_code*    m_codePerFrame  = nullptr;
    projectm_eval_code*    m_codePerVertex = nullptr;

    // Cached pointers into projectm-eval-allocated variable storage.
    // Indices match the Var enum. m_var[VAR_BASS] is the double the
    // preset reads when it says `bass`.
    std::array<double*, VAR_COUNT> m_var{};

    // Per-vertex defaults snapshot — captured after per_frame runs, so
    // per_vertex resets to these at the start of each vertex eval.
    double m_pvDefault_zoom    = 1.0;
    double m_pvDefault_zoomexp = 1.0;
    double m_pvDefault_rot     = 0.0;
    double m_pvDefault_cx      = 0.5;
    double m_pvDefault_cy      = 0.5;
    double m_pvDefault_sx      = 1.0;
    double m_pvDefault_sy      = 1.0;
    double m_pvDefault_warp    = 0.0;

    QString m_lastError;
    bool    m_hasPreset = false;

    // Internal time state.
    double m_time     = 0.0;     // seconds
    long   m_frameNo  = 0;
    double m_fps      = 60.0;
};
