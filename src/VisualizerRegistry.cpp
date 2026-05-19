#include "VisualizerRegistry.h"

#include <QColor>
#include <QDebug>
#include <QSettings>

namespace concerto::viz {

namespace {

// Settings key helpers — kept together so the on-disk layout is obvious
// from one place. See class header for the wire format.
constexpr const char* kCurrentIdKey = "viz/currentId";
constexpr const char* kDefaultId    = "viz16band";

QString paramKey(const QString& id, const QString& key) {
    return QStringLiteral("viz/%1/%2").arg(id, key);
}

QString idGroupPrefix(const QString& id) {
    return QStringLiteral("viz/%1/").arg(id);
}

} // namespace

VisualizerRegistry::VisualizerRegistry(QObject* parent)
    : QObject(parent)
{
    registerBuiltins();
    buildAvailableCache();

    // Restore the persisted selection. QSettings is configured globally
    // in main.cpp (setOrganizationName + setApplicationName), so the
    // no-arg ctor lands in the right per-app store.
    QSettings settings;
    const QString saved = settings.value(kCurrentIdKey).toString();
    if (!saved.isEmpty() && findDef(saved) != nullptr) {
        m_currentId = saved;
    } else {
        // Fresh install or stale id from a removed viz — fall back to
        // the 16-band shader, the legacy default.
        m_currentId = QString::fromLatin1(kDefaultId);
    }
}

VisualizerRegistry::~VisualizerRegistry() = default;

// ----------------------------------------------------------------------
// Property writes
// ----------------------------------------------------------------------

void VisualizerRegistry::setCurrentVisualizerId(const QString& id)
{
    if (id == m_currentId) return;
    if (findDef(id) == nullptr) {
        qWarning() << "[VisualizerRegistry] setCurrentVisualizerId: unknown id"
                   << id;
        return;
    }
    m_currentId = id;
    QSettings().setValue(kCurrentIdKey, m_currentId);
    emit currentVisualizerIdChanged();
}

// ----------------------------------------------------------------------
// QML-invokable accessors
// ----------------------------------------------------------------------

QVariantMap VisualizerRegistry::visualizerInfo(const QString& id) const
{
    const VizDef* v = findDef(id);
    if (!v) return {};
    QVariantMap m = defHeader(*v);
    m["params"] = paramSchema(id);
    return m;
}

QVariantList VisualizerRegistry::paramSchema(const QString& id) const
{
    const VizDef* v = findDef(id);
    if (!v) return {};
    QVariantList out;
    out.reserve(v->params.size());
    for (const auto& p : v->params) out.append(paramToMap(p));
    return out;
}

QVariant VisualizerRegistry::paramValue(const QString& id,
                                        const QString& key) const
{
    const VizDef* v = findDef(id);
    if (!v) return {};
    const ParamDef* p = findParam(*v, key);
    if (!p) return {};
    return QSettings().value(paramKey(id, key), p->defaultValue);
}

QVariant VisualizerRegistry::paramValue(const QString& id,
                                        const QString& key,
                                        const QVariant& fallback) const
{
    const VizDef* v = findDef(id);
    if (!v) return fallback;
    const ParamDef* p = findParam(*v, key);
    const QVariant def = p ? p->defaultValue : fallback;
    return QSettings().value(paramKey(id, key), def);
}

void VisualizerRegistry::setParamValue(const QString& id,
                                       const QString& key,
                                       const QVariant& value)
{
    const VizDef* v = findDef(id);
    if (!v) {
        qWarning() << "[VisualizerRegistry] setParamValue: unknown viz id"
                   << id;
        return;
    }
    if (findParam(*v, key) == nullptr) {
        qWarning() << "[VisualizerRegistry] setParamValue: viz" << id
                   << "has no param" << key;
        return;
    }
    QSettings().setValue(paramKey(id, key), value);
    emit paramChanged(id, key);
}

void VisualizerRegistry::resetParams(const QString& id)
{
    const VizDef* v = findDef(id);
    if (!v) return;

    QSettings settings;
    // Snapshot the keys that were actually persisted under this prefix
    // so we can emit paramChanged() for each one we clear.
    settings.beginGroup(idGroupPrefix(id).chopped(1));   // drop trailing '/'
    const QStringList persisted = settings.childKeys();
    settings.remove("");                                  // clear the group
    settings.endGroup();

    for (const QString& k : persisted) emit paramChanged(id, k);
}

QStringList VisualizerRegistry::paramKeys(const QString& id) const
{
    const VizDef* v = findDef(id);
    if (!v) return {};
    QStringList out;
    out.reserve(v->params.size());
    for (const auto& p : v->params) out.append(p.key);
    return out;
}

// ----------------------------------------------------------------------
// Built-in registry
// ----------------------------------------------------------------------

void VisualizerRegistry::registerBuiltins()
{
    // 1. In-Main.qml ShaderEffect — no qmlSource (handled inline by the
    //    main window's shader path). Default selection.
    //    Schema maps to FftProcessor::displaySlope, persisted under the
    //    legacy "viz16BandSlope" key (see main.cpp:123 + 157).
    m_defs.append(VizDef{
        "viz16band", "16-Band Shader", QString(), "Built-in",
        /*audioFeatures*/ false,
        /*fft*/           true,
        /*cqt*/           false,
        /*chroma*/        false,
        /*mfcc*/          false,
        /*sidecar*/       false,
        QList<ParamDef>{
            ParamDef{"displaySlope", "Display Slope (dB/oct)", "float",
                     3.0, 0.0, 6.0, {}},
        },
    });

    // 2. DSP: oscilloscope / vectorscope (Layer 1a). Maps to ScopeRenderer.
    m_defs.append(VizDef{
        "scope", "Oscilloscope / Vectorscope",
        "qrc:/qt/qml/PLYR/qml/ScopeView.qml",
        "DSP",
        /*audioFeatures*/ true,
        /*fft*/           false,
        /*cqt*/           false,
        /*chroma*/        false,
        /*mfcc*/          false,
        /*sidecar*/       false,
        QList<ParamDef>{
            ParamDef{"mode",            "Mode",             "enum",
                     0, {}, {}, QStringList{"Oscilloscope", "Vectorscope"}},
            ParamDef{"sigma",           "Beam Sharpness",   "float",
                     1.5, 0.5, 5.0, {}},
            ParamDef{"decay",           "Phosphor Decay",   "float",
                     0.08, 0.0, 0.5, {}},
            ParamDef{"beamIntensity",   "Beam Intensity",   "float",
                     1.0, 0.1, 4.0, {}},
            ParamDef{"beamColor",       "Beam Color",       "color",
                     QColor(0x34, 0xff, 0x34), {}, {}, {}},
            ParamDef{"stereoSeparated", "Split L/R",        "bool",
                     true, {}, {}, {}},
            ParamDef{"audioGain",       "Audio Gain",       "float",
                     1.0, 0.1, 8.0, {}},
        },
    });

    // 3. DSP: SPAN-style spectrum analyzer (Layer 1b). Maps to SpectrumAnalyzer.
    m_defs.append(VizDef{
        "spectrum", "Spectrum Analyzer (SPAN)",
        "qrc:/qt/qml/PLYR/qml/SpectrumView.qml",
        "DSP",
        /*audioFeatures*/ true,
        /*fft*/           false,
        /*cqt*/           false,
        /*chroma*/        false,
        /*mfcc*/          false,
        /*sidecar*/       false,
        QList<ParamDef>{
            ParamDef{"displaySlope",     "Slope (dB/oct)",   "float",
                     4.5, 0.0, 9.0, {}},
            ParamDef{"smoothingOctaves", "Smoothing (oct)",  "float",
                     0.0, 0.0, 1.0, {}},
            ParamDef{"dBMin",            "dB Min",           "float",
                     -90.0, -120.0, -40.0, {}},
            ParamDef{"dBMax",            "dB Max",           "float",
                     0.0, -30.0, 20.0, {}},
            ParamDef{"showPeakHold",     "Peak Hold",        "bool",
                     true, {}, {}, {}},
            ParamDef{"showInfinitePeak", "Infinite Peak",    "bool",
                     false, {}, {}, {}},
        },
    });

    // 4. DSP: constant-Q spectrogram waterfall (Layer 1c). Maps to
    //    CqtSpectrogram. The CqtAnalyzer's geometric params (binsPerOctave,
    //    nOctaves, fMin) are construction-time only and not user-tunable
    //    at runtime; we expose only the display + scroll knobs.
    m_defs.append(VizDef{
        "cqt", "Constant-Q Spectrogram",
        "qrc:/qt/qml/PLYR/qml/CqtSpectrogramView.qml",
        "DSP",
        /*audioFeatures*/ true,
        /*fft*/           false,
        /*cqt*/           true,
        /*chroma*/        false,
        /*mfcc*/          false,
        /*sidecar*/       false,
        QList<ParamDef>{
            ParamDef{"dbMin",      "dB Min",      "float",
                     -80.0, -120.0, -40.0, {}},
            ParamDef{"dbMax",      "dB Max",      "float",
                     0.0, -30.0, 20.0, {}},
            ParamDef{"autoScroll", "Auto Scroll", "bool",
                     true, {}, {}, {}},
        },
    });

    // 5. Music-Theoretic: Tonnetz lattice overlay (Layer 2a). Maps to TonnetzView.
    m_defs.append(VizDef{
        "tonnetz", "Tonnetz Overlay",
        "qrc:/qt/qml/PLYR/qml/TonnetzOverlay.qml",
        "Music-Theoretic",
        /*audioFeatures*/ false,
        /*fft*/           false,
        /*cqt*/           false,
        /*chroma*/        true,
        /*mfcc*/          false,
        /*sidecar*/       false,
        QList<ParamDef>{
            ParamDef{"litThreshold", "Lit Threshold", "float",
                     0.4, 0.0, 1.0, {}},
            ParamDef{"topK",         "Top-K Triads",  "int",
                     4, 1, 12, {}},
            ParamDef{"majorColor",   "Major Color",   "color",
                     QColor("#FFC433"), {}, {}, {}},
            ParamDef{"minorColor",   "Minor Color",   "color",
                     QColor("#33C8FF"), {}, {}, {}},
            ParamDef{"restColor",    "Rest Color",    "color",
                     QColor("#1A1A1F"), {}, {}, {}},
            ParamDef{"showLabels",   "Show Labels",   "bool",
                     true, {}, {}, {}},
            ParamDef{"showAxes",     "Show Axes",     "bool",
                     false, {}, {}, {}},
        },
    });

    // 6. Music-Theoretic: Janata-style tonal torus (Layer 2b). The viz is a
    //    QML file (TonalTorusView.qml); its tunable surface is the three
    //    color properties at the root. Backed by KeyEstimator (analyzer
    //    knobs there — softmaxTemperature / smoothingMs — are
    //    Q_INVOKABLE setters, not Q_PROPERTYs, so they're not part of the
    //    persisted param schema today).
    m_defs.append(VizDef{
        "tonaltorus", "Tonal Torus",
        "qrc:/qt/qml/PLYR/qml/TonalTorusView.qml",
        "Music-Theoretic",
        /*audioFeatures*/ false,
        /*fft*/           false,
        /*cqt*/           false,
        /*chroma*/        true,
        /*mfcc*/          false,
        /*sidecar*/       false,
        QList<ParamDef>{
            ParamDef{"keyColor",        "Key Glow Color",   "color",
                     QColor("#FFD966"), {}, {}, {}},
            ParamDef{"backColor",       "Torus Base Color", "color",
                     QColor("#0C2640"), {}, {}, {}},
            ParamDef{"backgroundColor", "Background",       "color",
                     QColor("#070A12"), {}, {}, {}},
        },
    });

    // 7. Music-Theoretic: 3D MFCC trajectory (Layer 2c). Maps to MfccTrajectory.
    m_defs.append(VizDef{
        "mfcc", "MFCC Trajectory",
        "qrc:/qt/qml/PLYR/qml/MfccTrajectoryView.qml",
        "Music-Theoretic",
        /*audioFeatures*/ false,
        /*fft*/           false,
        /*cqt*/           false,
        /*chroma*/        false,
        /*mfcc*/          true,
        /*sidecar*/       false,
        QList<ParamDef>{
            ParamDef{"headColor",     "Head Color",     "color",
                     QColor(0x00, 0xE0, 0xFF), {}, {}, {}},
            ParamDef{"tailColor",     "Tail Color",     "color",
                     QColor(0x5B, 0x1E, 0x96), {}, {}, {}},
            ParamDef{"cameraOrbitHz", "Orbit Speed",    "float",
                     1.0 / 60.0, 0.0, 0.5, {}},
        },
    });

    // 8. Spectacle: ShaderToy-style audio shader library (Layer 3a).
    //    Shader selection is preset-based (handled inside ShaderToyPane.qml
    //    via currentIndex + a built-in shaderLibrary list, advanced by
    //    arrow keys / cycle button); not modeled as a persisted param here.
    m_defs.append(VizDef{
        "shadertoy", "Shader Library",
        "qrc:/qt/qml/PLYR/qml/ShaderToyPane.qml",
        "Spectacle",
        /*audioFeatures*/ true,
        /*fft*/           false,
        /*cqt*/           false,
        /*chroma*/        false,
        /*mfcc*/          false,
        /*sidecar*/       false,
        QList<ParamDef>{},
    });

    // 9. Spectacle: MilkDrop warp-mesh preset runtime (Layer 3b).
    //    Preset selection (presetPath) lives outside the param schema —
    //    it's better modeled as a file picker / preset browser. Runtime
    //    internals (decay/zoom/cx/cy/q1..q8/etc.) are driven by the loaded
    //    .milk script, not user-tunable.
    m_defs.append(VizDef{
        "milkdrop", "MilkDrop Presets",
        "qrc:/qt/qml/PLYR/qml/MilkdropView.qml",
        "Spectacle",
        /*audioFeatures*/ true,
        /*fft*/           false,
        /*cqt*/           false,
        /*chroma*/        false,
        /*mfcc*/          false,
        /*sidecar*/       false,
        QList<ParamDef>{},
    });

    // 10. Frontier: persistent-homology barcode (Layer 4a). Maps to
    //     PersistentHomologyAnalyzer + PersistenceBarcode (renderer).
    m_defs.append(VizDef{
        "persistence", "Persistence Barcode",
        "qrc:/qt/qml/PLYR/qml/PersistenceBarcodeView.qml",
        "Frontier",
        /*audioFeatures*/ false,
        /*fft*/           false,
        /*cqt*/           false,
        /*chroma*/        false,
        /*mfcc*/          true,
        /*sidecar*/       false,
        QList<ParamDef>{
            ParamDef{"windowSize",      "Window Size",      "int",
                     64, 16, 256, {}},
            ParamDef{"hopsPerCompute",  "Recompute Every",  "int",
                     30, 1, 240, {}},
            ParamDef{"dimMax",          "Max Dimension",    "int",
                     1, 0, 2, {}},
            ParamDef{"h1Color",         "H1 Loop Color",    "color",
                     QColor(0x33, 0xC8, 0xFF), {}, {}, {}},
            ParamDef{"h0Color",         "H0 Cluster Color", "color",
                     QColor(0x88, 0x88, 0x88), {}, {}, {}},
            ParamDef{"minPersistence",  "Min Persistence",  "float",
                     0.0, 0.0, 1.0, {}},
            ParamDef{"autoScaleRadius", "Auto Scale",       "bool",
                     true, {}, {}, {}},
        },
    });

    // 11. Frontier: self-similarity-matrix scrubber (Layer 4b). Reads
    //     a precomputed .ssm sidecar — offline pipeline, no real-time DSP
    //     or user-tunable params (display mode / tint are caller-supplied
    //     bindings in SsmScrubber.qml, not persisted state).
    m_defs.append(VizDef{
        "ssm", "Self-Similarity Matrix",
        "qrc:/qt/qml/PLYR/qml/SsmScrubber.qml",
        "Frontier",
        /*audioFeatures*/ false,
        /*fft*/           false,
        /*cqt*/           false,
        /*chroma*/        false,
        /*mfcc*/          false,
        /*sidecar*/       true,
        QList<ParamDef>{},
    });

    // 12. Frontier: Hilbert-pair rosette (Layer 4c). Maps to HilbertRosette.
    m_defs.append(VizDef{
        "hilbert", "Hilbert Rosette",
        "qrc:/qt/qml/PLYR/qml/HilbertRosetteView.qml",
        "Frontier",
        /*audioFeatures*/ true,
        /*fft*/           false,
        /*cqt*/           false,
        /*chroma*/        false,
        /*mfcc*/          false,
        /*sidecar*/       false,
        QList<ParamDef>{
            ParamDef{"trailTau",       "Trail Decay (s)",  "float",
                     0.15, 0.0, 2.0, {}},
            ParamDef{"dotRadius",      "Dot Size (px)",    "float",
                     8.0, 1.0, 32.0, {}},
            ParamDef{"ringRadius",     "Ring Radius",      "float",
                     0.40, 0.1, 0.8, {}},
            ParamDef{"showBaseRing",   "Show Base Ring",   "bool",
                     true, {}, {}, {}},
            ParamDef{"showBandLabels", "Show Band Labels", "bool",
                     false, {}, {}, {}},
        },
    });

    // 13. Frontier: PDE / Chladni (Layer 4d). Maps to PdeView. The view
    //     has two modes (Chladni / GrayScott); params from both are
    //     exposed since the user picks the mode dynamically.
    m_defs.append(VizDef{
        "pde", "PDE / Chladni",
        "qrc:/qt/qml/PLYR/qml/PdeView.qml",
        "Frontier",
        /*audioFeatures*/ true,
        /*fft*/           false,
        /*cqt*/           false,
        /*chroma*/        false,
        /*mfcc*/          false,
        /*sidecar*/       false,
        QList<ParamDef>{
            ParamDef{"mode",         "Mode",            "enum",
                     1, {}, {}, QStringList{"Chladni", "Gray-Scott"}},
            ParamDef{"chladniM",     "Chladni m",       "float",
                     5.0, 1.0, 16.0, {}},
            ParamDef{"chladniN",     "Chladni n",       "float",
                     7.0, 1.0, 16.0, {}},
            ParamDef{"gsFeedBase",   "GS Feed (F)",     "float",
                     0.035, 0.0, 0.1, {}},
            ParamDef{"gsKillBase",   "GS Kill (k)",     "float",
                     0.062, 0.0, 0.1, {}},
            ParamDef{"gsColorA",     "GS Color A",      "color",
                     QColor(0x14, 0x07, 0x2A), {}, {}, {}},
            ParamDef{"gsColorB",     "GS Color B",      "color",
                     QColor(0xFF, 0xE6, 0xC2), {}, {}, {}},
        },
    });
}

// ----------------------------------------------------------------------
// Private helpers
// ----------------------------------------------------------------------

void VisualizerRegistry::buildAvailableCache()
{
    m_availableCache.clear();
    m_availableCache.reserve(m_defs.size());
    for (const auto& v : m_defs) m_availableCache.append(defHeader(v));
}

const VisualizerRegistry::VizDef*
VisualizerRegistry::findDef(const QString& id) const
{
    for (const auto& v : m_defs)
        if (v.id == id) return &v;
    return nullptr;
}

const VisualizerRegistry::ParamDef*
VisualizerRegistry::findParam(const VizDef& v, const QString& key)
{
    for (const auto& p : v.params)
        if (p.key == key) return &p;
    return nullptr;
}

QVariantMap VisualizerRegistry::paramToMap(const ParamDef& p)
{
    QVariantMap m;
    m["key"]     = p.key;
    m["label"]   = p.label;
    m["type"]    = p.type;
    m["default"] = p.defaultValue;
    if (p.minValue.isValid()) m["min"] = p.minValue;
    if (p.maxValue.isValid()) m["max"] = p.maxValue;
    if (!p.choices.isEmpty()) m["choices"] = p.choices;
    return m;
}

QVariantMap VisualizerRegistry::defHeader(const VizDef& v)
{
    QVariantMap m;
    m["id"]                    = v.id;
    m["displayName"]           = v.displayName;
    m["qmlSource"]             = v.qmlSource;
    m["category"]              = v.category;
    m["requiresAudioFeatures"] = v.requiresAudioFeatures;
    m["requiresFft"]           = v.requiresFft;
    m["requiresCqt"]           = v.requiresCqt;
    m["requiresChroma"]        = v.requiresChroma;
    m["requiresMfcc"]          = v.requiresMfcc;
    m["offlineSidecarOnly"]    = v.offlineSidecarOnly;
    return m;
}

} // namespace concerto::viz
