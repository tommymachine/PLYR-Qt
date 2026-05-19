// PdeView -- Layer 4d audio-modulated PDE visualizer.
//
// Two presets in one widget. Both are real partial-differential
// equations solved on the GPU; the audio bands modulate parameters,
// so the patterns are deeply musical.
//
//   * Chladni  -- stateless. A 2D rectangular plate's vibrational
//                 eigenmodes superposed; the visible "sand piles on
//                 zero-crossings" pattern is the level-set
//                 |u(x,y)| < band. (m, n) mode numbers track the
//                 spectral centroid; bass_att drives a small time
//                 perturbation so the pattern breathes; rms_att
//                 controls overall brightness.
//
//   * GrayScott -- a coupled reaction-diffusion PDE
//                  du/dt = Du*lap(u) - u*v^2 + F*(1-u)
//                  dv/dt = Dv*lap(v) + u*v^2 - (F+k)*v
//                  solved by forward Euler. Ping-pong RGBA16F textures
//                  hold (u, v); the host runs `gsStepsPerFrame`
//                  substeps per visible frame. Audio modulation:
//                  F = F_base + 0.020 * bass_att,
//                  k = k_base + 0.005 * treb_att (small, so we stay
//                  inside the "spots" basin of attraction). Onsets
//                  fire a soft circular v-bump at a quasi-random
//                  location (visible as a fresh patch of growth).
//
// Threading: identical to the rest of the visualizer stack.
//   * GUI thread owns the PdeView QObject, stages audio scalars +
//     reset / mode switches in atomics.
//   * synchronize() (render-thread, GUI-thread blocked) snapshots the
//     staged state.
//   * render() runs the solver passes + display pass.
//
// References (math only -- no GPL source is copied):
//   * Pearson 1993, "Complex Patterns in a Simple System", Science 261.
//   * Gray & Scott 1984 (the original derivation).
//   * Sims, Karl: https://www.karlsims.com/rd.html (parameter map +
//     forward-Euler-on-grid recipe). The shader code in this layer is
//     re-derived; only the PDE math and the (F, k) basin chart are
//     drawn from these references.
//   * Chladni 1787 _Entdeckungen ueber die Theorie des Klanges_; the
//     closed-form superposed-modes trick is folklore.

#pragma once

#include <QColor>
#include <QMutex>
#include <QObject>
#include <QQuickRhiItem>
#include <array>
#include <atomic>
#include <chrono>
#include <qqmlregistration.h>
#include <random>

class AudioFeatures;
class PdeViewImpl;

class PdeView : public QQuickRhiItem {
    Q_OBJECT
    QML_ELEMENT

public:
    enum class Mode : int {
        Chladni    = 0,
        GrayScott  = 1
    };
    Q_ENUM(Mode)

    Q_PROPERTY(AudioFeatures* audioSource READ audioSource WRITE setAudioSource
               NOTIFY audioSourceChanged)
    Q_PROPERTY(Mode  mode READ mode WRITE setMode NOTIFY modeChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)

    // Chladni -------------------------------------------------------------
    Q_PROPERTY(float chladniM READ chladniM WRITE setChladniM
               NOTIFY chladniMChanged)
    Q_PROPERTY(float chladniN READ chladniN WRITE setChladniN
               NOTIFY chladniNChanged)
    Q_PROPERTY(QColor chladniLineColor READ chladniLineColor
               WRITE setChladniLineColor NOTIFY chladniLineColorChanged)
    Q_PROPERTY(QColor chladniBgColor READ chladniBgColor
               WRITE setChladniBgColor NOTIFY chladniBgColorChanged)
    Q_PROPERTY(float chladniLineWidth READ chladniLineWidth
               WRITE setChladniLineWidth NOTIFY chladniLineWidthChanged)

    // Gray-Scott ----------------------------------------------------------
    Q_PROPERTY(float gsFeedBase READ gsFeedBase WRITE setGsFeedBase
               NOTIFY gsFeedBaseChanged)
    Q_PROPERTY(float gsKillBase READ gsKillBase WRITE setGsKillBase
               NOTIFY gsKillBaseChanged)
    Q_PROPERTY(float gsDu READ gsDu WRITE setGsDu NOTIFY gsDuChanged)
    Q_PROPERTY(float gsDv READ gsDv WRITE setGsDv NOTIFY gsDvChanged)
    Q_PROPERTY(float gsDt READ gsDt WRITE setGsDt NOTIFY gsDtChanged)
    Q_PROPERTY(int   gsStepsPerFrame READ gsStepsPerFrame
               WRITE setGsStepsPerFrame NOTIFY gsStepsPerFrameChanged)
    Q_PROPERTY(QColor gsColorA READ gsColorA WRITE setGsColorA NOTIFY gsColorAChanged)
    Q_PROPERTY(QColor gsColorB READ gsColorB WRITE setGsColorB NOTIFY gsColorBChanged)
    // Read-only diagnostics surfaced to QML for a debug HUD.
    Q_PROPERTY(float gsLastFeed READ gsLastFeed NOTIFY gsParamsAdvanced)
    Q_PROPERTY(float gsLastKill READ gsLastKill NOTIFY gsParamsAdvanced)

    explicit PdeView(QQuickItem* parent = nullptr);
    ~PdeView() override;

    AudioFeatures* audioSource() const { return m_audioSource; }
    void setAudioSource(AudioFeatures* s);

    Mode  mode() const { return m_mode; }
    void  setMode(Mode m);

    bool active() const { return m_active.load(std::memory_order_relaxed); }
    void setActive(bool a);

    float chladniM() const { return m_chladniM; }
    void  setChladniM(float v);
    float chladniN() const { return m_chladniN; }
    void  setChladniN(float v);
    QColor chladniLineColor() const { return m_chladniLine; }
    void   setChladniLineColor(const QColor& c);
    QColor chladniBgColor() const { return m_chladniBg; }
    void   setChladniBgColor(const QColor& c);
    float  chladniLineWidth() const { return m_chladniLineW; }
    void   setChladniLineWidth(float v);

    float gsFeedBase() const { return m_gsFeedBase; }
    void  setGsFeedBase(float v);
    float gsKillBase() const { return m_gsKillBase; }
    void  setGsKillBase(float v);
    float gsDu() const { return m_gsDu; }
    void  setGsDu(float v);
    float gsDv() const { return m_gsDv; }
    void  setGsDv(float v);
    float gsDt() const { return m_gsDt; }
    void  setGsDt(float v);
    int   gsStepsPerFrame() const { return m_gsSteps; }
    void  setGsStepsPerFrame(int v);
    QColor gsColorA() const { return m_gsColorA; }
    void   setGsColorA(const QColor& c);
    QColor gsColorB() const { return m_gsColorB; }
    void   setGsColorB(const QColor& c);

    float gsLastFeed() const { return m_gsLastFeed.load(std::memory_order_relaxed); }
    float gsLastKill() const { return m_gsLastKill.load(std::memory_order_relaxed); }

    // Re-seed the Gray-Scott simulation. Synchronous-from-QML; the
    // actual texture rewrite happens at the start of the next render().
    Q_INVOKABLE void resetGrayScott();

protected:
    QQuickRhiItemRenderer* createRenderer() override;

signals:
    void audioSourceChanged();
    void modeChanged();
    void chladniMChanged();
    void chladniNChanged();
    void chladniLineColorChanged();
    void chladniBgColorChanged();
    void chladniLineWidthChanged();
    void gsFeedBaseChanged();
    void gsKillBaseChanged();
    void gsDuChanged();
    void gsDvChanged();
    void gsDtChanged();
    void gsStepsPerFrameChanged();
    void gsColorAChanged();
    void gsColorBChanged();
    // Emitted on the GUI thread once per frame after the renderer
    // commits the live (F, k) it computed from the audio source --
    // lets a debug HUD show the effective values.
    void gsParamsAdvanced();
    void activeChanged();

private slots:
    // Pulls fresh audio scalars from AudioFeatures into the staged
    // snapshot. Connected to AudioFeatures::featuresUpdated.
    void onFeaturesUpdated();
    // Drains AudioFeatures::onset so we can fire a flux impulse on the
    // next solver step.
    void onAudioOnset();

private:
    friend class PdeViewImpl;

    // GUI-thread state staged for the renderer.
    struct StagedAudio {
        float bass = 0.0f, mid = 0.0f, treb = 0.0f;
        float bass_att = 0.0f, mid_att = 0.0f, treb_att = 0.0f;
        float rms_att = 0.0f, flux_norm = 0.0f, centroid_norm = 0.0f;
    };

    AudioFeatures*         m_audioSource = nullptr;

    QMutex                 m_stagedMutex;
    StagedAudio            m_staged;
    std::atomic<bool>      m_audioDirty {false};

    // Onset-driven flux-impulse pending count. The renderer drains it
    // each frame; queueing more than one between renders is fine
    // (multiple impulses get clamped to a single bump at fresh sites).
    std::atomic<int>       m_pendingFluxImpulses {0};

    // Properties.
    Mode    m_mode         = Mode::GrayScott;
    float   m_chladniM     = 5.0f;
    float   m_chladniN     = 7.0f;
    QColor  m_chladniLine  = QColor(0xFF, 0xFF, 0xFF);
    QColor  m_chladniBg    = QColor(0x07, 0x0A, 0x12);
    float   m_chladniLineW = 1.0f;

    float   m_gsFeedBase   = 0.035f;
    float   m_gsKillBase   = 0.062f;
    float   m_gsDu         = 0.16f;
    float   m_gsDv         = 0.08f;
    float   m_gsDt         = 1.0f;
    int     m_gsSteps      = 8;
    QColor  m_gsColorA     = QColor(0x14, 0x07, 0x2A);   // deep purple
    QColor  m_gsColorB     = QColor(0xFF, 0xE6, 0xC2);   // warm cream

    // Flags drained by the renderer.
    std::atomic<bool>      m_modeChanged    {false};
    std::atomic<bool>      m_gsResetRequested {false};

    // Renderer publishes the actual (F, k) it used last frame so QML
    // can display it.
    std::atomic<float>     m_gsLastFeed {0.035f};
    std::atomic<float>     m_gsLastKill {0.062f};

    // Visualizer-selector gate. False = audio staging + render-side
    // audio pickup go to no-ops; the solver still runs on whatever
    // state was last staged (no GPU work elision -- that's QML's job
    // via item visibility).
    std::atomic<bool>      m_active {true};
};
