// VisualizerRegistry — canonical catalogue of every visualizer the app
// can host. Holds id / displayName / qmlSource / category / capability
// flags / param schema for each, plus persisted param values and the
// currently-selected viz id. A future VisualizerHost (QML) consults this
// registry to decide which qmlSource to load, what params to surface in
// the edit UI, and which DSP analyzers to wire up.
//
// Lives on the GUI thread. Stack-allocated in main.cpp and exposed as
// the QML context property "visualizers" (same pattern as fft / audio /
// audioFeatures / eq / ripper / systemPaths). Not a QML_SINGLETON —
// QML_UNCREATABLE because the single instance is the one constructed in
// main.
//
// Persistence: writes through to QSettings on every set so a crash /
// force-quit doesn't lose param edits. Keys:
//   viz/currentId            — current viz id
//   viz/<id>/<paramKey>      — per-param values

#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <qqmlregistration.h>

namespace concerto::viz {

class VisualizerRegistry : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided as the 'visualizers' context property")

    Q_PROPERTY(QString currentVisualizerId
               READ currentVisualizerId
               WRITE setCurrentVisualizerId
               NOTIFY currentVisualizerIdChanged)
    Q_PROPERTY(QVariantList available READ available CONSTANT)

public:
    // ---- Internal types -------------------------------------------------

    struct ParamDef {
        QString     key;          // e.g. "sigma"
        QString     label;        // e.g. "Beam Sharpness"
        QString     type;         // "float" | "int" | "color" | "bool" | "enum"
        QVariant    defaultValue;
        QVariant    minValue;     // float/int only; QVariant() = N/A
        QVariant    maxValue;
        QStringList choices;      // enum only
    };

    struct VizDef {
        QString id;                                 // canonical short id
        QString displayName;                        // human-readable label
        QString qmlSource;                          // qrc:/qt/qml/PLYR/qml/<File>.qml
        QString category;                           // DSP | Music-Theoretic | Spectacle | Frontier | Built-in

        // Capability flags — consumed by a future VisualizerHost to wire
        // the right inputs (analyzers, sidecar loaders, etc).
        bool requiresAudioFeatures = false;
        bool requiresFft           = false;
        bool requiresCqt           = false;
        bool requiresChroma        = false;
        bool requiresMfcc          = false;
        bool offlineSidecarOnly    = false;         // SSM

        QList<ParamDef> params;
    };

    explicit VisualizerRegistry(QObject* parent = nullptr);
    ~VisualizerRegistry() override;

    // ---- Property reads -------------------------------------------------
    QString      currentVisualizerId() const { return m_currentId; }
    QVariantList available()           const { return m_availableCache; }

    // ---- Property writes ------------------------------------------------
    // Setter validates against the registered viz list; unknown ids are
    // rejected with a qWarning() and no-op. Persists immediately to
    // QSettings under "viz/currentId".
    void setCurrentVisualizerId(const QString& id);

    // ---- QML-invokable accessors ---------------------------------------

    // Full descriptor for one viz, or an empty map if id is unknown.
    Q_INVOKABLE QVariantMap  visualizerInfo(const QString& id) const;

    // Param schema for one viz — list of {key, label, type, default,
    // min, max, choices}. Empty list for unknown ids or vizes with no
    // params declared.
    Q_INVOKABLE QVariantList paramSchema(const QString& id) const;

    // Persisted param value, or the schema default if unset. Unknown
    // id/key returns an invalid QVariant.
    Q_INVOKABLE QVariant     paramValue(const QString& id,
                                        const QString& key) const;

    // Three-arg variant: same as the two-arg form but with an explicit
    // fallback when the QSettings entry is missing AND the schema lookup
    // also fails (unknown key).
    Q_INVOKABLE QVariant     paramValue(const QString& id,
                                        const QString& key,
                                        const QVariant& fallback) const;

    // Write a param value through to QSettings under
    // "viz/<id>/<key>". Unknown id/key emits qWarning() and no-ops.
    // Emits paramChanged(id, key) on success.
    Q_INVOKABLE void         setParamValue(const QString& id,
                                           const QString& key,
                                           const QVariant& value);

    // Clear the entire "viz/<id>/" QSettings subtree, restoring defaults
    // on next read. Emits paramChanged() for each cleared key. Unknown
    // id is a silent no-op (no schema, nothing to reset).
    Q_INVOKABLE void         resetParams(const QString& id);

    // All declared param keys for a viz (whether or not values are set).
    Q_INVOKABLE QStringList  paramKeys(const QString& id) const;

signals:
    void currentVisualizerIdChanged();
    void paramChanged(QString id, QString key);

private:
    // Populate m_defs with the 13 built-in viz entries. Called once
    // from the constructor before settings restore.
    void registerBuiltins();

    // Build the QVariantList that backs the `available` Q_PROPERTY.
    // Called once after registerBuiltins().
    void buildAvailableCache();

    // Helper: find a VizDef by id, or nullptr.
    const VizDef* findDef(const QString& id) const;

    // Helper: find a ParamDef within a VizDef by key, or nullptr.
    static const ParamDef* findParam(const VizDef& v, const QString& key);

    // Serialize a single ParamDef to a QVariantMap, for paramSchema().
    static QVariantMap paramToMap(const ParamDef& p);

    // Serialize a VizDef header (everything except params) — used by
    // both visualizerInfo() and buildAvailableCache().
    static QVariantMap defHeader(const VizDef& v);

    QList<VizDef> m_defs;
    QVariantList  m_availableCache;
    QString       m_currentId;
};

} // namespace concerto::viz
