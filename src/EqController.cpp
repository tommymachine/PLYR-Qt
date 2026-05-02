#include "EqController.h"

#include "AudioEngine.h"
#include "eq_presets.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>

namespace {

constexpr double kDbMin   = -12.0;
constexpr double kDbMax   =  12.0;
constexpr double kQMin    =  0.5;    // global/width
constexpr double kQMax    =  2.5;
constexpr double kQBandMin = 0.3;    // per-band override allows a wider range
constexpr double kQBandMax = 4.0;

inline double clampd(double x, double lo, double hi) {
    return std::clamp(x, lo, hi);
}

} // namespace


EqController::EqController(AudioEngine* audio, QObject* parent)
    : QObject(parent)
    , m_audio(audio)
    , m_eq(nullptr)   // attached via AudioEngine::engineReady (audio-thread init)
{
    // Default working state = Flat, loaded clean. Matches what the engine
    // already holds (all zeros) but explicitly sets baseline so Save/Revert
    // logic starts from a known point.
    m_state.layers = { Layer{ "flat", 1.0 } };
    m_state.globalQ = 1.0;
    m_baseline = m_state;
    m_baseId   = "builtin:flat";
    m_baseName = "Flat";

    loadUserPresets();

    // Ship with EQ off; the user enables it from the panel when they want
    // to hear it. pushToDsp is a no-op until the EQ handle arrives.
    m_enabled = false;

    // Restore working + baseline state (including enabled flag) from
    // the previous session. If there was none, the defaults above stand.
    loadSession();
    m_lastDirty = !statesEqual(m_state, m_baseline);

    // AudioEngine creates its eq_engine_t on the audio thread inside the
    // worker's init(). Wait for engineReady (main-thread signal) before
    // attaching, then flush current state to the DSP.
    if (m_audio) {
        connect(m_audio, &AudioEngine::engineReady, this, [this]() {
            m_eq = m_audio->eqEngine();
            pushToDsp();
        });
        // Handle the case where engineReady fired before this connect
        // ran (possible if audio init finished very fast).
        if (auto* eq = m_audio->eqEngine()) {
            m_eq = eq;
        }
    }

    pushToDsp();   // no-op today if m_eq is still null.
}


EqController::~EqController() = default;


// ---------------- property getters ----------------

double EqController::preamp() const {
    if (m_state.preampOverride) return *m_state.preampOverride;
    // Summed from layers.
    double pre = 0.0;
    for (const auto& L : m_state.layers) {
        double g[EQ_NUM_BANDS]; double p = 0.0;
        if (getPresetGains(L.id, g, &p)) pre += p * L.intensity;
    }
    return clampd(pre, kDbMin, kDbMax);
}

QVariantList EqController::bandFrequencies() const {
    QVariantList v;
    for (int b = 0; b < EQ_NUM_BANDS; b++) v.push_back(EQ_BAND_FREQUENCIES[b]);
    return v;
}

QVariantList EqController::bandGains() const {
    QVariantList v;
    double g[EQ_NUM_BANDS] = {0};
    for (const auto& L : m_state.layers) {
        double lg[EQ_NUM_BANDS]; double lp = 0.0;
        if (!getPresetGains(L.id, lg, &lp)) continue;
        for (int b = 0; b < EQ_NUM_BANDS; b++) g[b] += lg[b] * L.intensity;
    }
    for (int b = 0; b < EQ_NUM_BANDS; b++) {
        double final = m_state.gainOverride[b] ? *m_state.gainOverride[b] : g[b];
        v.push_back(clampd(final, kDbMin, kDbMax));
    }
    return v;
}

QVariantList EqController::bandQs() const {
    QVariantList v;
    for (int b = 0; b < EQ_NUM_BANDS; b++) {
        if (m_state.qOverride[b]) v.push_back(*m_state.qOverride[b]);
        else                      v.push_back(QVariant{});  // null = use global
    }
    return v;
}

QVariantList EqController::presets() const {
    QVariantList out;
    // Built-ins.
    for (int i = 0; i < eq_preset_count(); i++) {
        const EqPreset* p = eq_preset_at(i);
        QVariantMap m;
        m["id"]          = QString::fromUtf8(p->id);
        m["displayName"] = QString::fromUtf8(p->display_name);
        m["builtin"]     = true;
        QVariantList gains;
        for (int b = 0; b < EQ_NUM_BANDS; b++) gains.push_back(p->band_gains_db[b]);
        m["gains"]       = gains;
        m["preamp"]      = p->preamp_db;
        out.push_back(m);
    }
    // User presets.
    for (const auto& u : m_userPresets) {
        QVariantMap m;
        m["id"]          = u.id;
        m["displayName"] = u.displayName;
        m["builtin"]     = false;
        QVariantList gains;
        for (int b = 0; b < EQ_NUM_BANDS; b++) gains.push_back(u.bandGains[b]);
        m["gains"]       = gains;
        m["preamp"]      = u.preamp;
        out.push_back(m);
    }
    return out;
}

QVariantList EqController::layers() const {
    QVariantList out;
    for (const auto& L : m_state.layers) {
        QVariantMap m;
        m["id"]        = L.id;
        m["intensity"] = L.intensity;
        out.push_back(m);
    }
    return out;
}

bool EqController::dirty() const {
    return !statesEqual(m_state, m_baseline);
}

QString EqController::loadedName() const {
    return m_baseName;
}

bool EqController::canOverwrite() const {
    return m_baseId.startsWith("user:");
}


// ---------------- top-level setters ----------------

void EqController::setEnabled(bool v) {
    if (m_enabled == v) return;
    m_enabled = v;
    if (m_eq) eq_set_bypass(m_eq, v ? 0 : 1);
    emit enabledChanged();
    saveSession();
}

void EqController::setPreamp(double db) {
    db = clampd(db, kDbMin, kDbMax);
    m_state.preampOverride = db;
    pushToDsp();
    emit preampChanged();
    emit bandGainsChanged();   // effective preamp view may change
    markDirty();
}

void EqController::setWidth(double q) {
    q = clampd(q, kQMin, kQMax);
    if (m_state.globalQ == q) return;
    m_state.globalQ = q;
    pushToDsp();
    emit widthChanged();
    markDirty();
}


// ---------------- band editing ----------------

void EqController::setBandGain(int band, double db) {
    if (band < 0 || band >= EQ_NUM_BANDS) return;
    db = clampd(db, kDbMin, kDbMax);
    m_state.gainOverride[band] = db;
    pushToDsp();
    emit bandGainsChanged();
    markDirty();
}

void EqController::setBandQ(int band, double q) {
    if (band < 0 || band >= EQ_NUM_BANDS) return;
    q = clampd(q, kQBandMin, kQBandMax);
    m_state.qOverride[band] = q;
    pushToDsp();
    emit bandQsChanged();
    markDirty();
}

void EqController::clearBandQ(int band) {
    if (band < 0 || band >= EQ_NUM_BANDS) return;
    m_state.qOverride[band].reset();
    pushToDsp();
    emit bandQsChanged();
    markDirty();
}


// ---------------- preset layering ----------------

void EqController::loadPreset(const QString& id) {
    double dummyG[EQ_NUM_BANDS]; double dummyP = 0.0;
    if (!getPresetGains(id, dummyG, &dummyP)) return;

    EqState next;
    next.layers = { Layer{ id, 1.0 } };

    const UserPreset* u = findUserPreset(id);
    QString baseName;
    if (u) {
        // User preset: adopt its stored Qs and globalQ.
        next.globalQ = u->globalQ;
        for (int b = 0; b < EQ_NUM_BANDS; b++)
            if (u->bandQs[b]) next.qOverride[b] = u->bandQs[b];
        baseName = u->displayName;
    } else {
        // Built-in: implicit width 1.0, no per-band Qs.
        next.globalQ = 1.0;
        if (const EqPreset* p = eq_preset_by_id(id.toUtf8().constData()))
            baseName = QString::fromUtf8(p->display_name);
    }

    m_state = next;
    const QString baseId = (u ? QStringLiteral("user:") : QStringLiteral("builtin:")) + id;
    setBaseline(m_state, baseId, baseName);

    pushToDsp();
    emit layersChanged();
    emit bandGainsChanged();
    emit bandQsChanged();
    emit preampChanged();
    emit widthChanged();
}

void EqController::activatePreset(const QString& id, double intensity) {
    double dummyG[EQ_NUM_BANDS]; double dummyP = 0.0;
    if (!getPresetGains(id, dummyG, &dummyP)) return;
    intensity = clampd(intensity, 0.0, 1.0);

    for (auto& L : m_state.layers) {
        if (L.id == id) {           // already active; just update intensity
            L.intensity = intensity;
            pushToDsp();
            emit layersChanged();
            emit bandGainsChanged();
            emit preampChanged();
            markDirty();
            return;
        }
    }
    m_state.layers.push_back(Layer{ id, intensity });
    pushToDsp();
    emit layersChanged();
    emit bandGainsChanged();
    emit preampChanged();
    markDirty();
}

void EqController::deactivatePreset(const QString& id) {
    const int before = m_state.layers.size();
    m_state.layers.erase(std::remove_if(m_state.layers.begin(), m_state.layers.end(),
                                        [&](const Layer& L){ return L.id == id; }),
                         m_state.layers.end());
    if (m_state.layers.size() == before) return;
    pushToDsp();
    emit layersChanged();
    emit bandGainsChanged();
    emit preampChanged();
    markDirty();
}

void EqController::setPresetIntensity(const QString& id, double intensity) {
    intensity = clampd(intensity, 0.0, 1.0);
    bool changed = false;
    for (auto& L : m_state.layers)
        if (L.id == id && L.intensity != intensity) {
            L.intensity = intensity;
            changed = true;
        }
    if (!changed) return;
    pushToDsp();
    emit layersChanged();
    emit bandGainsChanged();
    emit preampChanged();
    markDirty();
}


// ---------------- save / revert ----------------

QString EqController::suggestPresetName() const {
    // Start from the loaded name if we have one; otherwise "Custom".
    QString base = m_baseName.isEmpty() ? QStringLiteral("Custom") : m_baseName;
    // Strip a trailing "_N" so repeated saves don't stack (Classical_1 -> Classical_2).
    static const QRegularExpression suffix(QStringLiteral("_(\\d+)$"));
    QRegularExpressionMatch m = suffix.match(base);
    if (m.hasMatch()) base = base.left(m.capturedStart());
    return nextUserId(base);   // returns next *display name*, not id
}

bool EqController::saveAs(const QString& displayName) {
    if (displayName.trimmed().isEmpty()) return false;

    // Block names that collide with built-ins or existing user presets
    // (case-insensitive). Callers should use suggestPresetName() for a
    // guaranteed-available name.
    for (int i = 0; i < eq_preset_count(); i++)
        if (QString::fromUtf8(eq_preset_at(i)->display_name)
              .compare(displayName, Qt::CaseInsensitive) == 0)
            return false;
    for (const auto& existing : m_userPresets)
        if (existing.displayName.compare(displayName, Qt::CaseInsensitive) == 0)
            return false;

    // Bake working state into a preset definition.
    UserPreset p;
    p.displayName = displayName;
    p.id = QStringLiteral("user_") + displayName.toLower()
             .replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")),
                      QStringLiteral("_"));
    // Ensure unique id (even if display name differs only in case).
    while (findUserPreset(p.id) != nullptr) p.id += QStringLiteral("_");

    QVariantList gains = bandGains();
    for (int b = 0; b < EQ_NUM_BANDS; b++) p.bandGains[b] = gains[b].toDouble();
    for (int b = 0; b < EQ_NUM_BANDS; b++) p.bandQs[b] = m_state.qOverride[b];
    p.preamp  = preamp();
    p.globalQ = m_state.globalQ;

    m_userPresets.push_back(p);
    saveUserPresets();

    // After save, working state = clean load of this new user preset.
    EqState next;
    next.layers = { Layer{ p.id, 1.0 } };
    for (int b = 0; b < EQ_NUM_BANDS; b++)
        if (p.bandQs[b]) next.qOverride[b] = p.bandQs[b];
    next.globalQ = p.globalQ;
    m_state = next;
    setBaseline(m_state, QStringLiteral("user:") + p.id, p.displayName);

    pushToDsp();
    emit presetsChanged();
    emit layersChanged();
    emit bandGainsChanged();
    emit bandQsChanged();
    emit preampChanged();
    emit widthChanged();
    return true;
}

bool EqController::save() {
    if (!canOverwrite()) return false;
    const QString id = m_baseId.mid(QStringLiteral("user:").size());
    UserPreset* u = findUserPreset(id);
    if (!u) return false;

    QVariantList gains = bandGains();
    for (int b = 0; b < EQ_NUM_BANDS; b++) u->bandGains[b] = gains[b].toDouble();
    for (int b = 0; b < EQ_NUM_BANDS; b++) u->bandQs[b] = m_state.qOverride[b];
    u->preamp  = preamp();
    u->globalQ = m_state.globalQ;
    saveUserPresets();

    // Rebase working state to match what's now stored.
    EqState next;
    next.layers = { Layer{ u->id, 1.0 } };
    for (int b = 0; b < EQ_NUM_BANDS; b++)
        if (u->bandQs[b]) next.qOverride[b] = u->bandQs[b];
    next.globalQ = u->globalQ;
    m_state = next;
    setBaseline(m_state, m_baseId, u->displayName);

    pushToDsp();
    emit presetsChanged();
    emit layersChanged();
    emit bandGainsChanged();
    emit bandQsChanged();
    emit preampChanged();
    return true;
}

void EqController::revert() {
    m_state = m_baseline;
    pushToDsp();
    emit layersChanged();
    emit bandGainsChanged();
    emit bandQsChanged();
    emit preampChanged();
    emit widthChanged();
    markDirty();   // will clear because state == baseline
}

bool EqController::deleteUserPreset(const QString& id) {
    auto it = std::find_if(m_userPresets.begin(), m_userPresets.end(),
                           [&](const UserPreset& u){ return u.id == id; });
    if (it == m_userPresets.end()) return false;

    m_userPresets.erase(it);
    saveUserPresets();

    // If the deleted preset was the baseline or an active layer, fall back
    // to Flat cleanly to avoid dangling references.
    if (m_baseId == QStringLiteral("user:") + id) {
        loadPreset(QStringLiteral("flat"));
    } else {
        // Remove from active layers if present.
        const int before = m_state.layers.size();
        m_state.layers.erase(std::remove_if(m_state.layers.begin(), m_state.layers.end(),
                                            [&](const Layer& L){ return L.id == id; }),
                             m_state.layers.end());
        if (m_state.layers.size() != before) {
            pushToDsp();
            emit layersChanged();
            emit bandGainsChanged();
            emit preampChanged();
        }
    }
    emit presetsChanged();
    return true;
}


// ---------------- internals ----------------

bool EqController::getPresetGains(const QString& id,
                                  double out_gains[EQ_NUM_BANDS],
                                  double* out_preamp,
                                  std::optional<double> out_qs[EQ_NUM_BANDS]) const {
    if (const EqPreset* p = eq_preset_by_id(id.toUtf8().constData())) {
        for (int b = 0; b < EQ_NUM_BANDS; b++) out_gains[b] = p->band_gains_db[b];
        if (out_preamp) *out_preamp = p->preamp_db;
        if (out_qs) for (int b = 0; b < EQ_NUM_BANDS; b++) out_qs[b].reset();
        return true;
    }
    if (const UserPreset* u = findUserPreset(id)) {
        for (int b = 0; b < EQ_NUM_BANDS; b++) out_gains[b] = u->bandGains[b];
        if (out_preamp) *out_preamp = u->preamp;
        if (out_qs) for (int b = 0; b < EQ_NUM_BANDS; b++) out_qs[b] = u->bandQs[b];
        return true;
    }
    return false;
}

void EqController::pushToDsp() {
    // Persist working + baseline state on every change, regardless of
    // whether the DSP handle is attached yet. Makes the session survive
    // a quit even before the user has played anything.
    saveSession();

    if (!m_eq) return;

    double gains[EQ_NUM_BANDS] = {0};
    double preamp = 0.0;
    for (const auto& L : m_state.layers) {
        double lg[EQ_NUM_BANDS]; double lp = 0.0;
        if (!getPresetGains(L.id, lg, &lp)) continue;
        for (int b = 0; b < EQ_NUM_BANDS; b++) gains[b] += lg[b] * L.intensity;
        preamp += lp * L.intensity;
    }
    for (int b = 0; b < EQ_NUM_BANDS; b++) {
        if (m_state.gainOverride[b]) gains[b] = *m_state.gainOverride[b];
        gains[b] = clampd(gains[b], kDbMin, kDbMax);
    }
    if (m_state.preampOverride) preamp = *m_state.preampOverride;
    preamp = clampd(preamp, kDbMin, kDbMax);

    eq_apply_target(m_eq, gains, preamp);
    eq_set_global_q(m_eq, clampd(m_state.globalQ, kQMin, kQMax));
    for (int b = 0; b < EQ_NUM_BANDS; b++) {
        if (m_state.qOverride[b]) eq_set_band_q(m_eq, b, *m_state.qOverride[b]);
        else                      eq_clear_band_q(m_eq, b);
    }
    eq_set_bypass(m_eq, m_enabled ? 0 : 1);
}

void EqController::setBaseline(const EqState& s,
                               const QString& baseId,
                               const QString& baseName) {
    const bool baseIdChanged   = (m_baseId != baseId);
    const bool baseNameChanged = (m_baseName != baseName);

    m_baseline = s;
    m_baseId   = baseId;
    m_baseName = baseName;

    // dirty flips to false since state == baseline now.
    const bool nowDirty = dirty();
    if (nowDirty != m_lastDirty) {
        m_lastDirty = nowDirty;
        emit dirtyChanged();
    }
    if (baseNameChanged) emit loadedNameChanged();
    if (baseIdChanged)   emit canOverwriteChanged();
}

void EqController::markDirty() {
    const bool nowDirty = dirty();
    if (nowDirty != m_lastDirty) {
        m_lastDirty = nowDirty;
        emit dirtyChanged();
    }
}

bool EqController::statesEqual(const EqState& a, const EqState& b) {
    if (a.layers.size() != b.layers.size()) return false;
    for (int i = 0; i < a.layers.size(); i++) {
        if (a.layers[i].id        != b.layers[i].id)        return false;
        if (a.layers[i].intensity != b.layers[i].intensity) return false;
    }
    for (int i = 0; i < EQ_NUM_BANDS; i++) {
        if (a.gainOverride[i] != b.gainOverride[i]) return false;
        if (a.qOverride[i]    != b.qOverride[i])    return false;
    }
    if (a.preampOverride != b.preampOverride) return false;
    if (a.globalQ        != b.globalQ)        return false;
    return true;
}


// ---------------- user preset persistence ----------------

QString EqController::userPresetsPath() const {
    const QString dir = QStandardPaths::writableLocation(
        QStandardPaths::AppConfigLocation);
    QDir().mkpath(dir);
    return dir + "/eq_user_presets.json";
}

void EqController::loadUserPresets() {
    QFile f(userPresetsPath());
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) return;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) return;
    const QJsonArray arr = doc.object().value("presets").toArray();

    m_userPresets.clear();
    m_userPresets.reserve(arr.size());
    for (const QJsonValue& v : arr) {
        const QJsonObject o = v.toObject();
        UserPreset p;
        p.id          = o.value("id").toString();
        p.displayName = o.value("displayName").toString();
        p.preamp      = o.value("preamp").toDouble(0.0);
        p.globalQ     = o.value("globalQ").toDouble(1.0);
        const QJsonArray g = o.value("gains").toArray();
        for (int b = 0; b < EQ_NUM_BANDS && b < g.size(); b++)
            p.bandGains[b] = g[b].toDouble(0.0);
        const QJsonArray q = o.value("qs").toArray();
        for (int b = 0; b < EQ_NUM_BANDS && b < q.size(); b++) {
            const QJsonValue qv = q[b];
            if (qv.isNull() || !qv.isDouble()) p.bandQs[b].reset();
            else                               p.bandQs[b] = qv.toDouble();
        }
        if (!p.id.isEmpty() && !p.displayName.isEmpty())
            m_userPresets.push_back(p);
    }
    emit presetsChanged();
}

void EqController::saveUserPresets() const {
    QJsonArray arr;
    for (const auto& u : m_userPresets) {
        QJsonObject o;
        o["id"]          = u.id;
        o["displayName"] = u.displayName;
        o["preamp"]      = u.preamp;
        o["globalQ"]     = u.globalQ;
        QJsonArray g;
        for (int b = 0; b < EQ_NUM_BANDS; b++) g.append(u.bandGains[b]);
        o["gains"] = g;
        QJsonArray q;
        for (int b = 0; b < EQ_NUM_BANDS; b++)
            q.append(u.bandQs[b] ? QJsonValue(*u.bandQs[b]) : QJsonValue::Null);
        o["qs"] = q;
        arr.append(o);
    }
    QJsonObject root;
    root["presets"] = arr;

    QFile f(userPresetsPath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

EqController::UserPreset* EqController::findUserPreset(const QString& id) {
    for (auto& u : m_userPresets) if (u.id == id) return &u;
    return nullptr;
}

const EqController::UserPreset* EqController::findUserPreset(const QString& id) const {
    for (const auto& u : m_userPresets) if (u.id == id) return &u;
    return nullptr;
}

QString EqController::nextUserId(const QString& displayBase) const {
    // Returns next available *display name* "Base_N". The machine id is
    // derived elsewhere from the display name.
    for (int i = 1; i < 10000; i++) {
        QString candidate = QStringLiteral("%1_%2").arg(displayBase).arg(i);
        bool taken = false;
        for (const auto& u : m_userPresets)
            if (u.displayName.compare(candidate, Qt::CaseInsensitive) == 0) {
                taken = true; break;
            }
        if (!taken) return candidate;
    }
    return displayBase;
}


// ---------------- session persistence (QSettings "eq/session") ----------

QJsonObject EqController::serializeEqState(const EqState& s)
{
    QJsonObject o;
    QJsonArray layers;
    for (const auto& L : s.layers) {
        QJsonObject li;
        li["id"]        = L.id;
        li["intensity"] = L.intensity;
        layers.append(li);
    }
    o["layers"] = layers;

    QJsonArray gains;
    for (int b = 0; b < EQ_NUM_BANDS; b++)
        gains.append(s.gainOverride[b] ? QJsonValue(*s.gainOverride[b])
                                       : QJsonValue::Null);
    o["gainOverrides"] = gains;

    QJsonArray qs;
    for (int b = 0; b < EQ_NUM_BANDS; b++)
        qs.append(s.qOverride[b] ? QJsonValue(*s.qOverride[b])
                                 : QJsonValue::Null);
    o["qOverrides"] = qs;

    o["preampOverride"] = s.preampOverride
        ? QJsonValue(*s.preampOverride) : QJsonValue::Null;
    o["globalQ"] = s.globalQ;
    return o;
}


void EqController::saveSession() const
{
    QJsonObject root;
    root["version"]  = 1;
    root["enabled"]  = m_enabled;
    root["baseId"]   = m_baseId;
    root["baseName"] = m_baseName;
    root["working"]  = serializeEqState(m_state);
    root["baseline"] = serializeEqState(m_baseline);

    QSettings s;
    s.setValue(QStringLiteral("eq/session"),
               QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
}


void EqController::loadSession()
{
    QSettings s;
    const QString raw = s.value(QStringLiteral("eq/session")).toString();
    if (raw.isEmpty()) return;

    const QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8());
    if (!doc.isObject()) return;
    const QJsonObject root = doc.object();
    if (root.value("version").toInt(0) != 1) return;   // unknown schema

    // Helper that fills an EqState from a JSON object, validating that
    // layer ids still resolve (built-ins never go away; user presets the
    // user may have deleted get silently dropped).
    auto fill = [this](const QJsonObject& o, EqState& dst) {
        dst.layers.clear();
        for (const QJsonValue& v : o.value("layers").toArray()) {
            const QJsonObject li = v.toObject();
            Layer L;
            L.id        = li.value("id").toString();
            L.intensity = li.value("intensity").toDouble(1.0);
            double dummy_g[EQ_NUM_BANDS]; double dummy_p = 0.0;
            if (getPresetGains(L.id, dummy_g, &dummy_p))
                dst.layers.append(L);
        }

        const QJsonArray gains = o.value("gainOverrides").toArray();
        for (int b = 0; b < EQ_NUM_BANDS; b++) {
            if (b < gains.size() && gains[b].isDouble())
                dst.gainOverride[b] = gains[b].toDouble();
            else
                dst.gainOverride[b].reset();
        }

        const QJsonArray qs = o.value("qOverrides").toArray();
        for (int b = 0; b < EQ_NUM_BANDS; b++) {
            if (b < qs.size() && qs[b].isDouble())
                dst.qOverride[b] = qs[b].toDouble();
            else
                dst.qOverride[b].reset();
        }

        const QJsonValue pv = o.value("preampOverride");
        if (pv.isDouble()) dst.preampOverride = pv.toDouble();
        else               dst.preampOverride.reset();

        dst.globalQ = o.value("globalQ").toDouble(1.0);
    };

    m_enabled  = root.value("enabled").toBool(false);
    m_baseId   = root.value("baseId").toString(QStringLiteral("builtin:flat"));
    m_baseName = root.value("baseName").toString(QStringLiteral("Flat"));
    fill(root.value("working").toObject(),  m_state);
    fill(root.value("baseline").toObject(), m_baseline);

    // Sanity: if the stored state somehow has no layers (every preset
    // got deleted?), fall back to Flat clean.
    if (m_state.layers.isEmpty()) {
        m_state.layers = { Layer{ QStringLiteral("flat"), 1.0 } };
        m_baseline     = m_state;
        m_baseId       = QStringLiteral("builtin:flat");
        m_baseName     = QStringLiteral("Flat");
    }
}
