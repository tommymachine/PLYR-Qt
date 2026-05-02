// EqController — Qt/QML-facing wrapper around the C EQ engine.
//
// Owns layering, override tracking, dirty state, preset save/revert, and
// user-preset persistence. The underlying eq_engine is owned by AudioEngine
// and only parameter-pushed through here. All user-facing state lives in
// this class; the C DSP only ever sees final 10 dB values + preamp + Qs.
//
// Design contract (from discussion):
//   - dB presets layer by additive dB sum; preamps sum too; result clamped ±12.
//   - dirty = working state differs from last loaded/saved baseline.
//   - Saving a built-in preset's edited derivative auto-names "<Name>_N".
//   - Built-in presets cannot be overwritten; user presets can.
//   - Width (global Q) and per-band Q overrides round out the saved state
//     for user presets; built-ins carry neither (width = 1.0 implicit).

#pragma once

#include "eq_engine.h"

#include <QObject>
#include <QVariantList>
#include <QVector>
#include <optional>

class AudioEngine;

class EqController : public QObject {
    Q_OBJECT

    Q_PROPERTY(bool    enabled      READ enabled      WRITE setEnabled  NOTIFY enabledChanged)
    Q_PROPERTY(double  preamp       READ preamp       WRITE setPreamp   NOTIFY preampChanged)
    Q_PROPERTY(double  width        READ width        WRITE setWidth    NOTIFY widthChanged)

    Q_PROPERTY(QVariantList bandFrequencies READ bandFrequencies CONSTANT)
    Q_PROPERTY(QVariantList bandGains       READ bandGains       NOTIFY bandGainsChanged)
    Q_PROPERTY(QVariantList bandQs          READ bandQs          NOTIFY bandQsChanged)

    Q_PROPERTY(QVariantList presets READ presets NOTIFY presetsChanged)
    Q_PROPERTY(QVariantList layers  READ layers  NOTIFY layersChanged)

    Q_PROPERTY(bool    dirty        READ dirty        NOTIFY dirtyChanged)
    Q_PROPERTY(QString loadedName   READ loadedName   NOTIFY loadedNameChanged)
    Q_PROPERTY(bool    canOverwrite READ canOverwrite NOTIFY canOverwriteChanged)

public:
    explicit EqController(AudioEngine* audio, QObject* parent = nullptr);
    ~EqController() override;

    // Read-side of properties.
    bool          enabled() const { return m_enabled; }
    double        preamp()  const;
    double        width()   const { return m_state.globalQ; }
    QVariantList  bandFrequencies() const;
    QVariantList  bandGains() const;
    QVariantList  bandQs()    const;
    QVariantList  presets()   const;
    QVariantList  layers()    const;
    bool          dirty()     const;
    QString       loadedName() const;
    bool          canOverwrite() const;

    // Write-side of top-level controls.
    void setEnabled(bool);
    void setPreamp(double db);
    void setWidth(double q);

public slots:
    // Band editing — each sets an override, marking dirty.
    void setBandGain (int band, double db);
    void setBandQ    (int band, double q);
    void clearBandQ  (int band);

    // Preset layering.
    void loadPreset         (const QString& id);                     // clean-load
    void activatePreset     (const QString& id, double intensity = 1.0);
    void deactivatePreset   (const QString& id);
    void setPresetIntensity (const QString& id, double intensity);

    // Save / revert.
    QString suggestPresetName() const;
    bool    saveAs (const QString& displayName);
    bool    save   ();
    void    revert ();
    bool    deleteUserPreset(const QString& id);

signals:
    void enabledChanged();
    void preampChanged();
    void widthChanged();
    void bandGainsChanged();
    void bandQsChanged();
    void presetsChanged();
    void layersChanged();
    void dirtyChanged();
    void loadedNameChanged();
    void canOverwriteChanged();

private:
    struct Layer {
        QString id;
        double  intensity = 1.0;
    };

    // Bundle of everything a user action can edit. Copying this is how we
    // take a baseline for revert / dirty-check.
    struct EqState {
        QVector<Layer>        layers;
        std::optional<double> gainOverride[EQ_NUM_BANDS];
        std::optional<double> qOverride   [EQ_NUM_BANDS];
        std::optional<double> preampOverride;
        double                globalQ = 1.0;
    };

    struct UserPreset {
        QString id;                             // "user_classical_1"
        QString displayName;                    // "Classical_1"
        double  bandGains[EQ_NUM_BANDS] = {0};
        std::optional<double> bandQs[EQ_NUM_BANDS];
        double  preamp  = 0.0;
        double  globalQ = 1.0;
    };

    // Compute final band gains/preamp by summing layers + applying overrides.
    // Also pushes everything to the C DSP via eq_apply_target + eq_set_*.
    void pushToDsp();

    // Baseline management: set after every clean load/save, read by revert.
    void setBaseline(const EqState& s,
                     const QString& baseId,        // "builtin:classical" / "user:u_id"
                     const QString& baseName);
    void markDirty();                              // emit dirtyChanged if state flipped

    // User preset storage — JSON file under AppConfigLocation.
    QString userPresetsPath() const;
    void    loadUserPresets();
    void    saveUserPresets() const;
    UserPreset* findUserPreset(const QString& id);
    const UserPreset* findUserPreset(const QString& id) const;
    QString nextUserId(const QString& displayName) const;

    // Session persistence — QSettings key "eq/session". Writes the full
    // working + baseline EqState so the app comes back exactly where it
    // was (including dirty state), with Save/Revert still meaningful.
    void saveSession() const;
    void loadSession();
    static class QJsonObject serializeEqState(const EqState& s);

    // Helpers for layering math.
    bool getPresetGains(const QString& id,
                        double out_gains[EQ_NUM_BANDS],
                        double* out_preamp,
                        std::optional<double> out_qs[EQ_NUM_BANDS] = nullptr) const;

    static bool statesEqual(const EqState& a, const EqState& b);

    AudioEngine*        m_audio = nullptr;
    EqEngine*           m_eq    = nullptr;   // borrowed from AudioEngine
    bool                m_enabled = false;

    EqState             m_state;              // working state
    EqState             m_baseline;           // snapshot for revert / dirty compare
    QString             m_baseId;             // "builtin:<id>" / "user:<id>" / ""
    QString             m_baseName;           // display name for UI ("", "Classical", "Classical_1")
    bool                m_lastDirty = false;  // cached for signal throttling

    QVector<UserPreset> m_userPresets;
};
