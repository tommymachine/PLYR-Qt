// MilkdropRuntime — see MilkdropRuntime.h for the design intent.
//
// projectm-eval API recap (from third_party/projectm-eval/ReadMe.md):
//
//   1. projectm_eval_context_create(NULL, NULL) — opens a context with
//      the built-in gmegabuf and reg00..reg99 storage. We pass NULL for
//      both because every preset gets its own context (no cross-context
//      sharing), and the global memory is automatically freed at
//      shutdown when projectm_eval_memory_global_destroy() is called.
//   2. projectm_eval_context_register_variable(ctx, "name") returns a
//      `double*` (PRJM_EVAL_F* with the default 8-byte float build) that
//      will be read/written by compiled code referencing that name. The
//      pointer is stable for the lifetime of the context.
//   3. projectm_eval_code_compile(ctx, source) returns an opaque handle.
//      Returns NULL on parse error; projectm_eval_get_error() then yields
//      the message + line/column.
//   4. projectm_eval_code_execute(handle) runs the bytecode. Allocation-
//      free at this point (the bytecode is final after compile).
//   5. projectm_eval_code_destroy(handle) / projectm_eval_context_destroy(ctx).
//
// The library also requires two host stubs:
//   void projectm_eval_memory_host_lock_mutex();
//   void projectm_eval_memory_host_unlock_mutex();
// Defined at the bottom of this file as empty bodies — we never call
// projectm-eval across threads concurrently. The render thread is the
// only consumer.

#include "MilkdropRuntime.h"

#include <projectm-eval.h>

#include <QDebug>
#include <QFile>
#include <QRegularExpression>
#include <QStringList>
#include <QTextStream>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

// Master name table. Lowercase to match projectm-eval's case-insensitive
// variable lookup. The order in this array must match the Var enum in
// MilkdropRuntime.h — we copy the same offsets when registering.
struct NamedVar {
    const char* name;
    int         enumIndex;
};

#define V(SYM, NAME) NamedVar{NAME, MilkdropRuntime::SYM}

constexpr NamedVar kVars[] = {
    V(VAR_TIME, "time"),
    V(VAR_FRAME, "frame"),
    V(VAR_FPS, "fps"),
    V(VAR_PROGRESS, "progress"),
    V(VAR_MESHX, "meshx"),
    V(VAR_MESHY, "meshy"),
    V(VAR_PIXELSX, "pixelsx"),
    V(VAR_PIXELSY, "pixelsy"),
    V(VAR_ASPECTX, "aspectx"),
    V(VAR_ASPECTY, "aspecty"),
    V(VAR_BASS, "bass"),
    V(VAR_MID, "mid"),
    V(VAR_TREB, "treb"),
    V(VAR_BASS_ATT, "bass_att"),
    V(VAR_MID_ATT, "mid_att"),
    V(VAR_TREB_ATT, "treb_att"),
    V(VAR_DECAY, "decay"),
    V(VAR_GAMMA, "gamma"),
    V(VAR_GAMMAADJ, "gammaadj"),
    V(VAR_ECHO_ZOOM, "echo_zoom"),
    V(VAR_ECHO_ALPHA, "echo_alpha"),
    V(VAR_ECHO_ORIENT, "echo_orient"),
    V(VAR_WAVE_R, "wave_r"),
    V(VAR_WAVE_G, "wave_g"),
    V(VAR_WAVE_B, "wave_b"),
    V(VAR_WAVE_A, "wave_a"),
    V(VAR_WAVE_X, "wave_x"),
    V(VAR_WAVE_Y, "wave_y"),
    V(VAR_WAVE_MYSTERY, "wave_mystery"),
    V(VAR_WAVE_MODE, "wave_mode"),
    V(VAR_WAVE_THICK, "wave_thick"),
    V(VAR_WAVE_SMOOTHING, "wave_smoothing"),
    V(VAR_WAVE_BRIGHTEN, "wave_brighten"),
    V(VAR_WAVE_DOTS, "wave_dots"),
    V(VAR_DARKEN_CENTER, "darken_center"),
    V(VAR_BRIGHTEN, "brighten"),
    V(VAR_DARKEN, "darken"),
    V(VAR_SOLARIZE, "solarize"),
    V(VAR_INVERT, "invert"),
    V(VAR_WRAP, "wrap"),
    V(VAR_TEXWRAP, "texwrap"),
    V(VAR_MV_X, "mv_x"),
    V(VAR_MV_Y, "mv_y"),
    V(VAR_MV_DX, "mv_dx"),
    V(VAR_MV_DY, "mv_dy"),
    V(VAR_MV_L, "mv_l"),
    V(VAR_MV_R, "mv_r"),
    V(VAR_MV_G, "mv_g"),
    V(VAR_MV_B, "mv_b"),
    V(VAR_MV_A, "mv_a"),
    V(VAR_ZOOM, "zoom"),
    V(VAR_ZOOMEXP, "zoomexp"),
    V(VAR_ROT, "rot"),
    V(VAR_CX, "cx"),
    V(VAR_CY, "cy"),
    V(VAR_SX, "sx"),
    V(VAR_SY, "sy"),
    V(VAR_WARP, "warp"),
    V(VAR_WARPANIMSPEED, "warpanimspeed"),
    V(VAR_WARPSCALE, "warpscale"),
    V(VAR_X, "x"),
    V(VAR_Y, "y"),
    V(VAR_RAD, "rad"),
    V(VAR_ANG, "ang"),
    V(VAR_DX, "dx"),
    V(VAR_DY, "dy"),
    V(VAR_Q1, "q1"),  V(VAR_Q2, "q2"),  V(VAR_Q3, "q3"),  V(VAR_Q4, "q4"),
    V(VAR_Q5, "q5"),  V(VAR_Q6, "q6"),  V(VAR_Q7, "q7"),  V(VAR_Q8, "q8"),
    V(VAR_Q9, "q9"),  V(VAR_Q10, "q10"), V(VAR_Q11, "q11"), V(VAR_Q12, "q12"),
    V(VAR_Q13, "q13"), V(VAR_Q14, "q14"), V(VAR_Q15, "q15"), V(VAR_Q16, "q16"),
    V(VAR_Q17, "q17"), V(VAR_Q18, "q18"), V(VAR_Q19, "q19"), V(VAR_Q20, "q20"),
    V(VAR_Q21, "q21"), V(VAR_Q22, "q22"), V(VAR_Q23, "q23"), V(VAR_Q24, "q24"),
    V(VAR_Q25, "q25"), V(VAR_Q26, "q26"), V(VAR_Q27, "q27"), V(VAR_Q28, "q28"),
    V(VAR_Q29, "q29"), V(VAR_Q30, "q30"), V(VAR_Q31, "q31"), V(VAR_Q32, "q32"),
    V(VAR_T1, "t1"),  V(VAR_T2, "t2"),  V(VAR_T3, "t3"),  V(VAR_T4, "t4"),
    V(VAR_T5, "t5"),  V(VAR_T6, "t6"),  V(VAR_T7, "t7"),  V(VAR_T8, "t8"),
    V(VAR_RAND, "rand"),
    V(VAR_MONITOR, "monitor"),
};

#undef V

// Reads/normalises a single line into a (key, value) pair. Returns
// false on comment-only / blank / [section-header] / no-equals lines.
// Strips trailing `// comment` from the value.
bool parseKVLine(const QString& rawLine, QString* key, QString* value)
{
    QString line = rawLine.trimmed();
    if (line.isEmpty()) return false;
    if (line.startsWith(QChar('['))) return false;     // [presetXX]
    if (line.startsWith(QStringLiteral("//"))) return false;

    const int eq = line.indexOf(QChar('='));
    if (eq <= 0) return false;
    *key   = line.left(eq).trimmed();
    QString v = line.mid(eq + 1);

    // Strip trailing comment if it's clearly outside a string/keyword;
    // MilkDrop preset values never contain "//" as part of code, so this
    // is safe.
    const int cmtIdx = v.indexOf(QStringLiteral("//"));
    if (cmtIdx >= 0) v.truncate(cmtIdx);

    *value = v.trimmed();
    return true;
}

// Build a single expression string by concatenating all entries with
// matching KEY-prefix-N=... values, ordered by N ascending. So
// per_frame_1, per_frame_2, ... become "expr1;expr2;...;".
QString concatNumbered(const QMap<int, QString>& bag)
{
    QString out;
    out.reserve(256 * bag.size());
    // QMap iteration is sorted by key, so this naturally ascends.
    for (auto it = bag.constBegin(); it != bag.constEnd(); ++it) {
        QString line = it.value();
        // Strip a trailing semicolon if the author added one — we add
        // one ourselves to keep concatenation unambiguous.
        while (line.endsWith(QChar(';')) || line.endsWith(QChar(' ')))
            line.chop(1);
        if (line.isEmpty()) continue;
        out += line;
        out += QChar(';');
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
//  Construction / destruction
// ---------------------------------------------------------------------------

MilkdropRuntime::MilkdropRuntime() = default;

MilkdropRuntime::~MilkdropRuntime()
{
    disposeCompiledPrograms();
    if (m_ctx) {
        projectm_eval_context_destroy(m_ctx);
        m_ctx = nullptr;
    }
}

void MilkdropRuntime::disposeCompiledPrograms()
{
    if (m_codeFrameInit) {
        projectm_eval_code_destroy(m_codeFrameInit);
        m_codeFrameInit = nullptr;
    }
    if (m_codePerFrame) {
        projectm_eval_code_destroy(m_codePerFrame);
        m_codePerFrame = nullptr;
    }
    if (m_codePerVertex) {
        projectm_eval_code_destroy(m_codePerVertex);
        m_codePerVertex = nullptr;
    }
}

void MilkdropRuntime::ensureContext()
{
    if (m_ctx) return;

    m_ctx = projectm_eval_context_create(nullptr, nullptr);
    if (!m_ctx) {
        m_lastError = QStringLiteral("projectm_eval_context_create returned NULL");
        return;
    }

    // Register every wired variable. projectm-eval owns the storage
    // and returns a stable pointer for the context's lifetime.
    for (const auto& nv : kVars) {
        double* p = projectm_eval_context_register_variable(m_ctx, nv.name);
        if (!p) {
            // Should never happen with valid identifiers, but keep the
            // index entry NULL so any later attempt to read explodes
            // visibly rather than silently no-op'ing.
            m_var[nv.enumIndex] = nullptr;
            continue;
        }
        *p = 0.0;
        m_var[nv.enumIndex] = p;
    }
}

// ---------------------------------------------------------------------------
//  Preset loading
// ---------------------------------------------------------------------------

bool MilkdropRuntime::loadPresetFromFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        m_lastError = QStringLiteral("cannot open preset file: %1").arg(path);
        m_hasPreset = false;
        return false;
    }
    return loadPresetFromString(f.readAll());
}

bool MilkdropRuntime::loadPresetFromString(const QByteArray& contents)
{
    ensureContext();
    if (!m_ctx) {
        m_hasPreset = false;
        return false;
    }

    disposeCompiledPrograms();
    resetVariables();

    // ---- Parse the .milk format -----------------------------------
    //
    // Per Geiss's spec, the format is INI-like: a `[presetXX]` header
    // optionally followed by `KEY=VALUE` lines, where the value of
    // per_frame_N / per_vertex_N / per_frame_init_N is a single ns-eel2
    // statement. We concatenate by index so per_frame_1, per_frame_2,
    // ... become one source string for projectm_eval_code_compile.
    //
    // Anything we don't recognise — wavecode_*, shapecode_*, comp_, warp_,
    // pixel-shader sections — is collected into a "skipped" list and
    // logged via qInfo() once per load. Layer 3b explicitly doesn't
    // implement those; the spec lists them under "out of scope".

    QMap<int, QString> bagInit;       // per_frame_init_N
    QMap<int, QString> bagFrame;      // per_frame_N
    QMap<int, QString> bagVertex;     // per_vertex_N
    std::unordered_map<QString, double> initialScalars;
    QStringList skippedSections;

    static const QRegularExpression kNumberedRx(
        QStringLiteral("^(per_frame_init|per_frame|per_vertex)_(\\d+)$"),
        QRegularExpression::CaseInsensitiveOption);

    const QString text = QString::fromUtf8(contents);

    // Sections we silently skip — they exist in real presets but aren't
    // supported in v1.
    //
    // Note that we deliberately don't include "wave_" here — top-level
    // scalar keys like `wave_r`, `wave_g`, `wave_b`, `wave_x`, `wave_y`
    // are legitimate (we just don't render the waveform, but those
    // values are still used as a global colour tint by the shader).
    // Custom-wave subkeys are always under `wavecode_N_…`.
    static const QStringList kSkipPrefixes = {
        QStringLiteral("wavecode_"),
        QStringLiteral("shapecode_"),
        QStringLiteral("comp_"),       // composite pixel-shader section
        QStringLiteral("warp_"),       // per-pixel warp shader section
    };

    const auto lines = text.split(QChar('\n'));
    for (const QString& rawLine : lines) {
        QString k, v;
        if (!parseKVLine(rawLine, &k, &v)) continue;

        const QString kLower = k.toLower();
        const auto match = kNumberedRx.match(kLower);
        if (match.hasMatch()) {
            const QString prefix = match.captured(1);
            const int n = match.captured(2).toInt();
            if (prefix == QStringLiteral("per_frame_init"))
                bagInit.insert(n, v);
            else if (prefix == QStringLiteral("per_frame"))
                bagFrame.insert(n, v);
            else /* per_vertex */
                bagVertex.insert(n, v);
            continue;
        }

        // Bucket known scalar headers. These map to ns-eel2 variables
        // that the per-frame expression may or may not overwrite.
        // Includes the canonical wave_*/sx/sy etc. that real MilkDrop
        // presets use as top-level scalar setters.
        static const QStringList kScalarKeys = {
            QStringLiteral("fdecay"),
            QStringLiteral("zoom"),
            QStringLiteral("rot"),
            QStringLiteral("cx"),
            QStringLiteral("cy"),
            QStringLiteral("sx"),
            QStringLiteral("sy"),
            QStringLiteral("dx"),
            QStringLiteral("dy"),
            QStringLiteral("zoomexp"),
            QStringLiteral("warp"),
            QStringLiteral("gamma"),
            QStringLiteral("echo_zoom"),
            QStringLiteral("fechozoom"),
            QStringLiteral("fechoalpha"),
            QStringLiteral("fechoorient"),
            QStringLiteral("fgammaadj"),
            QStringLiteral("fwarpanimspeed"),
            QStringLiteral("fwarpscale"),
            // Wave colour + position (used by our shader as a tint
            // even though we don't draw waves).
            QStringLiteral("wave_r"),
            QStringLiteral("wave_g"),
            QStringLiteral("wave_b"),
            QStringLiteral("wave_a"),
            QStringLiteral("wave_x"),
            QStringLiteral("wave_y"),
            QStringLiteral("wave_mystery"),
            QStringLiteral("wave_mode"),
            QStringLiteral("wave_thick"),
            QStringLiteral("wave_smoothing"),
            QStringLiteral("wave_brighten"),
            QStringLiteral("wave_dots"),
            // Motion vectors (not rendered but readable from preset code).
            QStringLiteral("mv_x"),  QStringLiteral("mv_y"),
            QStringLiteral("mv_dx"), QStringLiteral("mv_dy"),
            QStringLiteral("mv_l"),  QStringLiteral("mv_r"),
            QStringLiteral("mv_g"),  QStringLiteral("mv_b"),
            QStringLiteral("mv_a"),
            // Booleans (presets store these as 0/1 but our renderer
            // also reads them as floats).
            QStringLiteral("darken_center"),
            QStringLiteral("brighten"),
            QStringLiteral("darken"),
            QStringLiteral("solarize"),
            QStringLiteral("invert"),
            QStringLiteral("wrap"),
            QStringLiteral("texwrap"),
        };
        bool taken = false;
        if (kScalarKeys.contains(kLower)) {
            bool ok = false;
            const double d = v.toDouble(&ok);
            if (ok) initialScalars[kLower] = d;
            taken = true;
        }

        if (taken) continue;

        // Anything else — check if it's a known-but-skipped section.
        bool skipped = false;
        for (const QString& sp : kSkipPrefixes) {
            if (kLower.startsWith(sp)) { skipped = true; break; }
        }
        if (skipped) {
            // Only record the section prefix, not every line; keeps the
            // log tidy when a preset has 4 custom waves with 30 lines
            // each.
            const int u = kLower.indexOf(QChar('_'),
                kLower.indexOf(QChar('_')) + 1);
            const QString tag = (u > 0) ? kLower.left(u) : kLower;
            if (!skippedSections.contains(tag))
                skippedSections.append(tag);
            continue;
        }
        // Unrecognised top-level key — also just record it, don't error.
    }

    if (!skippedSections.isEmpty()) {
        qInfo() << "[MilkdropRuntime] preset references unsupported sections,"
                << "ignored:" << skippedSections;
    }

    // ---- Apply initial scalars ------------------------------------
    applyInitialScalars(initialScalars);

    // ---- Compile each expression block ----------------------------
    const QString src_init   = concatNumbered(bagInit);
    const QString src_frame  = concatNumbered(bagFrame);
    const QString src_vertex = concatNumbered(bagVertex);

    auto compileBlock = [&](const QString& source,
                            const char* blockName) -> projectm_eval_code* {
        if (source.isEmpty()) return nullptr;
        const QByteArray utf8 = source.toUtf8();
        projectm_eval_code* code =
            projectm_eval_code_compile(m_ctx, utf8.constData());
        if (!code) {
            int line = 0, col = 0;
            const char* msg = projectm_eval_get_error(m_ctx, &line, &col);
            m_lastError = QStringLiteral("%1 block compile error at %2:%3 — %4")
                .arg(QString::fromLatin1(blockName))
                .arg(line).arg(col)
                .arg(msg ? QString::fromUtf8(msg)
                         : QStringLiteral("unknown error"));
            qWarning() << "[MilkdropRuntime]" << m_lastError
                       << "; source was:\n" << source;
        }
        return code;
    };

    m_codeFrameInit = compileBlock(src_init,   "per_frame_init");
    m_codePerFrame  = compileBlock(src_frame,  "per_frame");
    m_codePerVertex = compileBlock(src_vertex, "per_vertex");

    // A preset is "loaded" if at least the per_frame or per_vertex block
    // compiled. Init-only or scalars-only is also acceptable — empty
    // presets just render the previous frame back through the decay
    // shader and tend to look black.
    m_hasPreset = (m_codeFrameInit || m_codePerFrame || m_codePerVertex);

    if (!m_hasPreset && m_lastError.isEmpty()) {
        m_lastError = QStringLiteral(
            "preset parsed but contained no executable code");
    } else if (m_hasPreset) {
        // Clear the error if everything compiled — failures from
        // individual blocks may have left lastError populated from a
        // partial parse.
        m_lastError.clear();
    }

    // Reset host-managed time/frame counters so the new preset starts
    // at t=0. Presets often gate behaviour on `time`/`frame`.
    m_time = 0.0;
    m_frameNo = 0;

    // Run per_frame_init exactly once. Reads/writes happen against the
    // variable bindings allocated above.
    if (m_codeFrameInit) {
        projectm_eval_code_execute(m_codeFrameInit);
    }

    // After init, snapshot the per-vertex defaults from whatever was
    // left in the variable slots. Most presets use per_frame_init to
    // set cx/cy or similar; we re-snapshot inside runPerFrame to pick
    // up dynamic changes.
    m_pvDefault_zoom    = m_var[VAR_ZOOM]    ? *m_var[VAR_ZOOM]    : 1.0;
    m_pvDefault_zoomexp = m_var[VAR_ZOOMEXP] ? *m_var[VAR_ZOOMEXP] : 1.0;
    m_pvDefault_rot     = m_var[VAR_ROT]     ? *m_var[VAR_ROT]     : 0.0;
    m_pvDefault_cx      = m_var[VAR_CX]      ? *m_var[VAR_CX]      : 0.5;
    m_pvDefault_cy      = m_var[VAR_CY]      ? *m_var[VAR_CY]      : 0.5;
    m_pvDefault_sx      = m_var[VAR_SX]      ? *m_var[VAR_SX]      : 1.0;
    m_pvDefault_sy      = m_var[VAR_SY]      ? *m_var[VAR_SY]      : 1.0;
    m_pvDefault_warp    = m_var[VAR_WARP]    ? *m_var[VAR_WARP]    : 0.0;

    return m_hasPreset;
}

void MilkdropRuntime::resetVariables()
{
    for (double* p : m_var) {
        if (p) *p = 0.0;
    }
    // Pick sensible defaults for "geometry" vars that presets expect
    // pre-populated. Per Geiss, the per-frame stage starts each frame
    // with these defaults:
    if (m_var[VAR_DECAY])     *m_var[VAR_DECAY]     = 0.96;
    if (m_var[VAR_GAMMA])     *m_var[VAR_GAMMA]     = 1.0;
    if (m_var[VAR_GAMMAADJ])  *m_var[VAR_GAMMAADJ]  = 1.0;
    if (m_var[VAR_ZOOM])      *m_var[VAR_ZOOM]      = 1.0;
    if (m_var[VAR_ZOOMEXP])   *m_var[VAR_ZOOMEXP]   = 1.0;
    if (m_var[VAR_ROT])       *m_var[VAR_ROT]       = 0.0;
    if (m_var[VAR_CX])        *m_var[VAR_CX]        = 0.5;
    if (m_var[VAR_CY])        *m_var[VAR_CY]        = 0.5;
    if (m_var[VAR_SX])        *m_var[VAR_SX]        = 1.0;
    if (m_var[VAR_SY])        *m_var[VAR_SY]        = 1.0;
    if (m_var[VAR_WARP])      *m_var[VAR_WARP]      = 0.0;
    if (m_var[VAR_WARPSCALE]) *m_var[VAR_WARPSCALE] = 1.0;
    if (m_var[VAR_WAVE_A])    *m_var[VAR_WAVE_A]    = 0.8;
    if (m_var[VAR_WAVE_R])    *m_var[VAR_WAVE_R]    = 1.0;
    if (m_var[VAR_WAVE_G])    *m_var[VAR_WAVE_G]    = 1.0;
    if (m_var[VAR_WAVE_B])    *m_var[VAR_WAVE_B]    = 1.0;
    if (m_var[VAR_ECHO_ZOOM]) *m_var[VAR_ECHO_ZOOM] = 1.0;
    if (m_var[VAR_FPS])       *m_var[VAR_FPS]       = 60.0;
    if (m_var[VAR_ASPECTX])   *m_var[VAR_ASPECTX]   = 1.0;
    if (m_var[VAR_ASPECTY])   *m_var[VAR_ASPECTY]   = 1.0;
}

void MilkdropRuntime::applyInitialScalars(
    const std::unordered_map<QString, double>& kv)
{
    auto set = [&](const QString& k, MilkdropRuntime::Var v) {
        auto it = kv.find(k);
        if (it == kv.end() || !m_var[v]) return;
        *m_var[v] = it->second;
    };

    // Map header keys to ns-eel2 variables. Note: the .milk format uses
    // mixed casing in real presets (fDecay, zoom, fEchoZoom). We
    // lowercased everything at parse time.
    set(QStringLiteral("fdecay"),         VAR_DECAY);
    set(QStringLiteral("zoom"),           VAR_ZOOM);
    set(QStringLiteral("rot"),            VAR_ROT);
    set(QStringLiteral("cx"),             VAR_CX);
    set(QStringLiteral("cy"),             VAR_CY);
    set(QStringLiteral("sx"),             VAR_SX);
    set(QStringLiteral("sy"),             VAR_SY);
    set(QStringLiteral("dx"),             VAR_DX);
    set(QStringLiteral("dy"),             VAR_DY);
    set(QStringLiteral("zoomexp"),        VAR_ZOOMEXP);
    set(QStringLiteral("warp"),           VAR_WARP);
    set(QStringLiteral("gamma"),          VAR_GAMMA);
    set(QStringLiteral("echo_zoom"),      VAR_ECHO_ZOOM);
    set(QStringLiteral("fechozoom"),      VAR_ECHO_ZOOM);
    set(QStringLiteral("fechoalpha"),     VAR_ECHO_ALPHA);
    set(QStringLiteral("fechoorient"),    VAR_ECHO_ORIENT);
    set(QStringLiteral("fgammaadj"),      VAR_GAMMAADJ);
    set(QStringLiteral("fwarpanimspeed"), VAR_WARPANIMSPEED);
    set(QStringLiteral("fwarpscale"),     VAR_WARPSCALE);
    set(QStringLiteral("wave_r"),         VAR_WAVE_R);
    set(QStringLiteral("wave_g"),         VAR_WAVE_G);
    set(QStringLiteral("wave_b"),         VAR_WAVE_B);
    set(QStringLiteral("wave_a"),         VAR_WAVE_A);
    set(QStringLiteral("wave_x"),         VAR_WAVE_X);
    set(QStringLiteral("wave_y"),         VAR_WAVE_Y);
    set(QStringLiteral("wave_mystery"),   VAR_WAVE_MYSTERY);
    set(QStringLiteral("wave_mode"),      VAR_WAVE_MODE);
    set(QStringLiteral("wave_thick"),     VAR_WAVE_THICK);
    set(QStringLiteral("wave_smoothing"), VAR_WAVE_SMOOTHING);
    set(QStringLiteral("wave_brighten"),  VAR_WAVE_BRIGHTEN);
    set(QStringLiteral("wave_dots"),      VAR_WAVE_DOTS);
    set(QStringLiteral("mv_x"),           VAR_MV_X);
    set(QStringLiteral("mv_y"),           VAR_MV_Y);
    set(QStringLiteral("mv_dx"),          VAR_MV_DX);
    set(QStringLiteral("mv_dy"),          VAR_MV_DY);
    set(QStringLiteral("mv_l"),           VAR_MV_L);
    set(QStringLiteral("mv_r"),           VAR_MV_R);
    set(QStringLiteral("mv_g"),           VAR_MV_G);
    set(QStringLiteral("mv_b"),           VAR_MV_B);
    set(QStringLiteral("mv_a"),           VAR_MV_A);
    set(QStringLiteral("darken_center"),  VAR_DARKEN_CENTER);
    set(QStringLiteral("brighten"),       VAR_BRIGHTEN);
    set(QStringLiteral("darken"),         VAR_DARKEN);
    set(QStringLiteral("solarize"),       VAR_SOLARIZE);
    set(QStringLiteral("invert"),         VAR_INVERT);
    set(QStringLiteral("wrap"),           VAR_WRAP);
    set(QStringLiteral("texwrap"),        VAR_TEXWRAP);
}

// ---------------------------------------------------------------------------
//  Per-tick / per-frame / per-vertex
// ---------------------------------------------------------------------------

void MilkdropRuntime::setAudio(float bass, float mid, float treb,
                               float bass_att, float mid_att, float treb_att)
{
    if (m_var[VAR_BASS])     *m_var[VAR_BASS]     = double(bass);
    if (m_var[VAR_MID])      *m_var[VAR_MID]      = double(mid);
    if (m_var[VAR_TREB])     *m_var[VAR_TREB]     = double(treb);
    if (m_var[VAR_BASS_ATT]) *m_var[VAR_BASS_ATT] = double(bass_att);
    if (m_var[VAR_MID_ATT])  *m_var[VAR_MID_ATT]  = double(mid_att);
    if (m_var[VAR_TREB_ATT]) *m_var[VAR_TREB_ATT] = double(treb_att);
}

void MilkdropRuntime::advance(float dt)
{
    if (dt < 0) dt = 0;
    m_time += double(dt);
    ++m_frameNo;
    if (dt > 1e-6f) m_fps = 1.0 / double(dt);

    if (m_var[VAR_TIME])  *m_var[VAR_TIME]  = m_time;
    if (m_var[VAR_FRAME]) *m_var[VAR_FRAME] = double(m_frameNo);
    if (m_var[VAR_FPS])   *m_var[VAR_FPS]   = m_fps;
}

void MilkdropRuntime::runPerFrame()
{
    if (!m_codePerFrame || !m_ctx) return;

    // Update per-frame `rand` to a fresh roll. Geiss's runtime exposes
    // `rand` as a continuously-evolving value, not a one-shot random
    // function; ns-eel2 has no built-in random opcode, so this is the
    // honest way to give the preset usable noise.
    if (m_var[VAR_RAND]) {
        // Cheap LCG. Seed kept implicit by aliasing the frame counter —
        // we want determinism per-frame so a re-rendered frame would
        // match.
        const uint32_t s = uint32_t(m_frameNo) * 1664525u + 1013904223u;
        *m_var[VAR_RAND] = double(s & 0x00FFFFFFu) / double(0x01000000);
    }

    // Refresh per-vertex defaults to per_frame-set values *before*
    // running per-frame — that way per-frame can both read AND write
    // them. (E.g. per_frame can say "zoom = zoom * 1.01", relying on
    // the previous default.)
    if (m_var[VAR_ZOOM])    *m_var[VAR_ZOOM]    = m_pvDefault_zoom;
    if (m_var[VAR_ZOOMEXP]) *m_var[VAR_ZOOMEXP] = m_pvDefault_zoomexp;
    if (m_var[VAR_ROT])     *m_var[VAR_ROT]     = m_pvDefault_rot;
    if (m_var[VAR_CX])      *m_var[VAR_CX]      = m_pvDefault_cx;
    if (m_var[VAR_CY])      *m_var[VAR_CY]      = m_pvDefault_cy;
    if (m_var[VAR_SX])      *m_var[VAR_SX]      = m_pvDefault_sx;
    if (m_var[VAR_SY])      *m_var[VAR_SY]      = m_pvDefault_sy;
    if (m_var[VAR_WARP])    *m_var[VAR_WARP]    = m_pvDefault_warp;

    projectm_eval_code_execute(m_codePerFrame);

    // Snapshot the per-frame-decided defaults for per-vertex starting
    // values. These are the "this frame's" defaults for the warp mesh.
    m_pvDefault_zoom    = m_var[VAR_ZOOM]    ? *m_var[VAR_ZOOM]    : 1.0;
    m_pvDefault_zoomexp = m_var[VAR_ZOOMEXP] ? *m_var[VAR_ZOOMEXP] : 1.0;
    m_pvDefault_rot     = m_var[VAR_ROT]     ? *m_var[VAR_ROT]     : 0.0;
    m_pvDefault_cx      = m_var[VAR_CX]      ? *m_var[VAR_CX]      : 0.5;
    m_pvDefault_cy      = m_var[VAR_CY]      ? *m_var[VAR_CY]      : 0.5;
    m_pvDefault_sx      = m_var[VAR_SX]      ? *m_var[VAR_SX]      : 1.0;
    m_pvDefault_sy      = m_var[VAR_SY]      ? *m_var[VAR_SY]      : 1.0;
    m_pvDefault_warp    = m_var[VAR_WARP]    ? *m_var[VAR_WARP]    : 0.0;
}

void MilkdropRuntime::runPerVertex(int meshW, int meshH, float* outUv)
{
    if (!outUv) return;

    // Update mesh-resolution variables — they're read-only from the
    // preset's POV, but we have to set them every call in case the
    // host changes the resolution.
    if (m_var[VAR_MESHX]) *m_var[VAR_MESHX] = double(meshW);
    if (m_var[VAR_MESHY]) *m_var[VAR_MESHY] = double(meshH);

    const double cx_f = m_pvDefault_cx;
    const double cy_f = m_pvDefault_cy;

    if (!m_codePerVertex || !m_ctx) {
        // Identity mesh — every vertex samples its own grid cell. This
        // keeps the renderer working even when a preset has no per_vertex
        // expression (e.g. our simple_zoom test preset).
        for (int j = 0; j < meshH; ++j) {
            for (int i = 0; i < meshW; ++i) {
                const float u = (meshW > 1) ? float(i) / float(meshW - 1) : 0.5f;
                const float v = (meshH > 1) ? float(j) / float(meshH - 1) : 0.5f;
                const int idx = 2 * (j * meshW + i);

                // Even with no per_vertex, per_frame's `zoom`/`rot`
                // should still apply globally. Bake them into the UV.
                // (centre cx_f, cy_f; rotate by `rot`; scale by `zoom`.)
                const double dx = double(u) - cx_f;
                const double dy = double(v) - cy_f;
                const double rot = m_pvDefault_rot;
                const double inv_zoom = (m_pvDefault_zoom != 0.0)
                    ? 1.0 / m_pvDefault_zoom : 1.0;
                const double cs = std::cos(rot), sn = std::sin(rot);
                const double xr = dx * cs - dy * sn;
                const double yr = dx * sn + dy * cs;
                outUv[idx + 0] = float(cx_f + xr * inv_zoom);
                outUv[idx + 1] = float(cy_f + yr * inv_zoom);
            }
        }
        return;
    }

    // The full per-vertex path. For every grid point we:
    //   1. Reset (x, y, rad, ang, dx, dy, zoom, rot, cx, cy, sx, sy, warp)
    //      to per-frame defaults.
    //   2. Set (x, y, rad, ang) to this vertex's coordinates.
    //   3. Execute the compiled per_vertex bytecode.
    //   4. Read back (dx, dy, zoom, rot, cx, cy) and synthesise the
    //      sample UV.
    //
    // Per-vertex result semantics (Geiss spec, paraphrased):
    //   "dx, dy = per-vertex translation in [0,1] UV space.
    //    zoom = scalar zoom toward (cx, cy).
    //    rot  = rotation around (cx, cy)."
    //
    // The renderer reads outUv as a 2D bilinear lookup table; the warp
    // shader samples the previous frame at the computed UV.

    for (int j = 0; j < meshH; ++j) {
        const double v_grid = (meshH > 1) ? double(j) / double(meshH - 1) : 0.5;
        for (int i = 0; i < meshW; ++i) {
            const double u_grid = (meshW > 1) ? double(i) / double(meshW - 1) : 0.5;

            const double x = u_grid;
            const double y = v_grid;
            const double rad = std::sqrt((x - cx_f) * (x - cx_f) +
                                         (y - cy_f) * (y - cy_f));
            const double ang = std::atan2(y - cy_f, x - cx_f);

            // (1) Reset per-vertex slots to per-frame defaults.
            if (m_var[VAR_ZOOM])    *m_var[VAR_ZOOM]    = m_pvDefault_zoom;
            if (m_var[VAR_ZOOMEXP]) *m_var[VAR_ZOOMEXP] = m_pvDefault_zoomexp;
            if (m_var[VAR_ROT])     *m_var[VAR_ROT]     = m_pvDefault_rot;
            if (m_var[VAR_CX])      *m_var[VAR_CX]      = m_pvDefault_cx;
            if (m_var[VAR_CY])      *m_var[VAR_CY]      = m_pvDefault_cy;
            if (m_var[VAR_SX])      *m_var[VAR_SX]      = m_pvDefault_sx;
            if (m_var[VAR_SY])      *m_var[VAR_SY]      = m_pvDefault_sy;
            if (m_var[VAR_WARP])    *m_var[VAR_WARP]    = m_pvDefault_warp;
            if (m_var[VAR_DX])      *m_var[VAR_DX]      = 0.0;
            if (m_var[VAR_DY])      *m_var[VAR_DY]      = 0.0;

            // (2) Set this vertex's inputs.
            if (m_var[VAR_X])   *m_var[VAR_X]   = x;
            if (m_var[VAR_Y])   *m_var[VAR_Y]   = y;
            if (m_var[VAR_RAD]) *m_var[VAR_RAD] = rad;
            if (m_var[VAR_ANG]) *m_var[VAR_ANG] = ang;

            // (3) Execute compiled per_vertex.
            projectm_eval_code_execute(m_codePerVertex);

            // (4) Read outputs and synthesise the warp UV.
            const double dx_pv   = m_var[VAR_DX]   ? *m_var[VAR_DX]   : 0.0;
            const double dy_pv   = m_var[VAR_DY]   ? *m_var[VAR_DY]   : 0.0;
            const double zoom_pv = m_var[VAR_ZOOM] ? *m_var[VAR_ZOOM] : 1.0;
            const double rot_pv  = m_var[VAR_ROT]  ? *m_var[VAR_ROT]  : 0.0;
            const double cx_pv   = m_var[VAR_CX]   ? *m_var[VAR_CX]   : cx_f;
            const double cy_pv   = m_var[VAR_CY]   ? *m_var[VAR_CY]   : cy_f;

            // Geiss's mesh distortion math:
            //   1. shift to centre cx_pv, cy_pv.
            //   2. rotate by rot_pv.
            //   3. divide by zoom_pv (zoom > 1 = sample CLOSER to centre
            //      = visual zoom-in).
            //   4. shift back, add (dx_pv, dy_pv).
            const double sx0 = x - cx_pv;
            const double sy0 = y - cy_pv;
            const double cs = std::cos(rot_pv), sn = std::sin(rot_pv);
            const double xr = sx0 * cs - sy0 * sn;
            const double yr = sx0 * sn + sy0 * cs;
            const double inv_zoom = (zoom_pv != 0.0) ? 1.0 / zoom_pv : 1.0;
            const double u = cx_pv + xr * inv_zoom + dx_pv;
            const double v = cy_pv + yr * inv_zoom + dy_pv;

            const int idx = 2 * (j * meshW + i);
            outUv[idx + 0] = float(u);
            outUv[idx + 1] = float(v);
        }
    }
}

// ---------------------------------------------------------------------------
//  Read-back accessors
// ---------------------------------------------------------------------------

float MilkdropRuntime::decay() const {
    return m_var[VAR_DECAY] ? float(*m_var[VAR_DECAY]) : 0.96f;
}
float MilkdropRuntime::gamma() const {
    return m_var[VAR_GAMMA] ? float(*m_var[VAR_GAMMA]) : 1.0f;
}
float MilkdropRuntime::echo_zoom() const {
    return m_var[VAR_ECHO_ZOOM] ? float(*m_var[VAR_ECHO_ZOOM]) : 1.0f;
}
float MilkdropRuntime::echo_alpha() const {
    return m_var[VAR_ECHO_ALPHA] ? float(*m_var[VAR_ECHO_ALPHA]) : 0.0f;
}
float MilkdropRuntime::wave_r() const {
    return m_var[VAR_WAVE_R] ? float(*m_var[VAR_WAVE_R]) : 1.0f;
}
float MilkdropRuntime::wave_g() const {
    return m_var[VAR_WAVE_G] ? float(*m_var[VAR_WAVE_G]) : 1.0f;
}
float MilkdropRuntime::wave_b() const {
    return m_var[VAR_WAVE_B] ? float(*m_var[VAR_WAVE_B]) : 1.0f;
}
float MilkdropRuntime::wave_a() const {
    return m_var[VAR_WAVE_A] ? float(*m_var[VAR_WAVE_A]) : 0.8f;
}
float MilkdropRuntime::darken_center() const {
    return m_var[VAR_DARKEN_CENTER] ? float(*m_var[VAR_DARKEN_CENTER]) : 0.0f;
}
float MilkdropRuntime::brighten() const {
    return m_var[VAR_BRIGHTEN] ? float(*m_var[VAR_BRIGHTEN]) : 0.0f;
}
float MilkdropRuntime::darken() const {
    return m_var[VAR_DARKEN] ? float(*m_var[VAR_DARKEN]) : 0.0f;
}
float MilkdropRuntime::invert() const {
    return m_var[VAR_INVERT] ? float(*m_var[VAR_INVERT]) : 0.0f;
}

float MilkdropRuntime::q(int i) const {
    if (i < 1) return 0.0f;
    if (i > 32) return 0.0f;
    const int idx = VAR_Q1 + (i - 1);
    return m_var[idx] ? float(*m_var[idx]) : 0.0f;
}

double* MilkdropRuntime::variable(const char* name)
{
    return lookup(name);
}

double* MilkdropRuntime::lookup(const char* name)
{
    if (!name) return nullptr;
    // Linear scan over kVars[] — only used for test/debug, so the cost
    // doesn't matter. Case-insensitive comparison via std::tolower.
    auto eq = [](const char* a, const char* b) {
        while (*a && *b) {
            int ca = std::tolower(static_cast<unsigned char>(*a));
            int cb = std::tolower(static_cast<unsigned char>(*b));
            if (ca != cb) return false;
            ++a; ++b;
        }
        return *a == 0 && *b == 0;
    };
    for (const auto& nv : kVars) {
        if (eq(nv.name, name)) return m_var[nv.enumIndex];
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
//  projectm-eval host stubs.
//
//  Required by the library even if we never use the locks (it's part of
//  the public API surface — see Memory-Handling.md). Empty bodies are
//  fine because we don't share gmegabuf between threads.
// ---------------------------------------------------------------------------

extern "C" void projectm_eval_memory_host_lock_mutex() {}
extern "C" void projectm_eval_memory_host_unlock_mutex() {}
