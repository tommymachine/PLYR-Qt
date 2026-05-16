// AudioEngine — main-thread QML facade over the AudioWorker (audio thread).
//
// This class lives on the main thread so QML's binding engine is happy to
// connect to its NOTIFY signals. All heavy lifting (decode, sink pull,
// EQ, FFT tap) happens on a dedicated audio thread inside AudioWorker.
//
// Public API is unchanged from the pre-threading version: Q_INVOKABLE
// play/pause/stop/seek/etc. and Q_PROPERTY source/position/duration/etc.
// Internally the engine:
//   - owns a QThread + AudioWorker (parent-less, moved to thread)
//   - forwards commands to the worker via requestXXX signals (queued)
//   - receives state changes via workerXxxChanged signals, updates local
//     atomic caches, and re-emits QML-facing signals
//
// QML always sees a main-thread QObject.

#pragma once

#include "eq_engine.h"

#include <QByteArray>
#include <QMutex>
#include <QObject>
#include <QUrl>
#include <atomic>
#include <memory>

#ifdef Q_OS_MACOS
namespace plyr::sync {
class AudioClock;
class DisplayClock;
}
#endif

class AudioWorker;
class FftProcessor;
class AudioFeatures;
class QThread;

class AudioEngine : public QObject {
    Q_OBJECT

    Q_PROPERTY(QUrl   source   READ source   WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(qint64 position READ position                 NOTIFY positionChanged)
    Q_PROPERTY(qint64 duration READ duration                 NOTIFY durationChanged)
    Q_PROPERTY(bool   playing  READ isPlaying                NOTIFY playingChanged)
    Q_PROPERTY(float  volume   READ volume   WRITE setVolume NOTIFY volumeChanged)

    // A/V-sync calibration bias in milliseconds.
    //
    // On macOS, the audio playhead-at-display-time is computed analytically
    // from CoreAudio's per-buffer host-time anchor + the CAMetalDisplayLink's
    // targetPresentationTimestamp + the output device's full latency chain
    // (see AudioClock / DisplayClock). Calibration is added on top of that
    // formula in case the user's headphones / DAC / Bluetooth path adds
    // additional ms the OS doesn't report (most BT codecs do).
    //
    // On non-macOS, the analytic formula isn't available; calibration is
    // added on top of the legacy 35 ms static lookahead instead, so the
    // knob still does the same thing perceptually.
    //
    // Default 0. Range -300..+300 ms (wide enough to catch BT AAC's 150 ms
    // codec delay either direction).
    Q_PROPERTY(int  syncCalibrationMs READ syncCalibrationMs WRITE setSyncCalibrationMs NOTIFY syncCalibrationMsChanged)

    // True if the current output device reports a Bluetooth transport.
    // QML can use this to widen the calibration UI's slider range and
    // surface a "Bluetooth detected" hint. Mac-only — non-Mac builds
    // always read false.
    Q_PROPERTY(bool outputIsBluetooth   READ outputIsBluetooth   NOTIFY outputIsBluetoothChanged)

public:
    explicit AudioEngine(QObject* parent = nullptr);
    ~AudioEngine() override;

    // Called before anything else ticks. Worker hasn't started its thread
    // yet, so a plain pointer store is safe.
    void setFftProcessor(FftProcessor* fft);
    void setAudioFeatures(AudioFeatures* af);

    // EQ handle. Returns nullptr until AudioWorker::init() has created it
    // (signalled via engineReady below). EqController waits on that
    // signal before touching the DSP.
    EqEngine* eqEngine() const { return m_eqCache.load(); }

    // --- Property reads: atomic caches updated by queued signals from
    //     the worker. Main-thread-safe.
    QUrl   source()             const;
    qint64 position()           const { return m_positionCache.load();           }
    qint64 duration()           const { return m_durationCache.load();           }
    bool   isPlaying()          const { return m_playingCache.load();            }
    float  volume()             const { return m_volumeCache.load();             }
    int    activeIndex()        const { return m_activeIndexCache.load();        }
    int    syncCalibrationMs()  const { return m_syncCalibrationMsCache.load();  }
    bool   outputIsBluetooth()  const { return m_outputIsBluetoothCache.load();  }

    // --- Setters: emit queued signals to the worker; update caches so
    //     QML reflects the change immediately (avoids one frame of lag).
    void   setSource(const QUrl& u);
    void   setVolume(float v);
    void   setSyncCalibrationMs(int ms);

    // Hand the worker a non-owning pointer to the clocks so its lookahead
    // tap can read live anchors. Called from main.cpp after construction.
    // On non-Mac this is a no-op. Wires the QML window in too — the
    // display clock needs the CAMetalLayer behind QQuickWindow::winId().
    Q_INVOKABLE void attachToWindow(QObject* window);

    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void seek(qint64 ms);
    Q_INVOKABLE void playAt   (int playlistIndex, const QUrl& url);
    Q_INVOKABLE void enqueueAt(int playlistIndex, const QUrl& url);

    // Live-PCM preview path for the CD ripper. See AudioWorker.h for
    // the full contract. pushPreviewPcm() crosses threads via a queued
    // signal — the QByteArray is implicitly shared so the bytes don't
    // get copied on the hop.
    Q_INVOKABLE void startPreviewStream(qint64 totalDurationMs = 0,
                                        qint64 startOffsetMs   = 0);
    Q_INVOKABLE void pushPreviewPcm(const QByteArray& int16Bytes);
    Q_INVOKABLE void stopPreviewStream();

    // A/V-sync calibration aid: generate `durationSec` seconds of
    // 1 Hz Hann-windowed 1 kHz sine pulses (a perceptual "click track")
    // and feed it through the existing streaming-preview path. Stays
    // entirely in memory — no file on disk, no playlist entry, no
    // recent-folder write. Pair with the SyncTuner overlay to dial in
    // syncCalibrationMs against an unambiguous transient.
    Q_INVOKABLE void playTestSinePulses(int durationSec = 30);

signals:
    // QML-facing change signals.
    void sourceChanged();
    void positionChanged();
    void durationChanged();
    void playingChanged();
    void volumeChanged();
    void syncCalibrationMsChanged();
    void outputIsBluetoothChanged();
    void trackEnded();
    void activeTrackChanged(int playlistIndex);
    void readyForNextTrack();

    // Fires once, after worker has created the EQ engine. EqController
    // uses this as the moment to start pushing state into the DSP.
    void engineReady();

    // --- Command signals to the worker. Qt auto-queues across threads.
    void requestPlay();
    void requestPause();
    void requestStop();
    void requestSeek(qint64 ms);
    void requestSetSource(QUrl u);
    void requestSetVolume(float v);
    void requestSetSyncCalibrationMs(int ms);
    void requestPlayAt(int playlistIndex, QUrl url);
    void requestEnqueueAt(int playlistIndex, QUrl url);
    void requestStartPreviewStream(qint64 totalDurationMs, qint64 startOffsetMs);
    void requestPushPreviewPcm(QByteArray int16Bytes);
    void requestStopPreviewStream();

private slots:
    // --- Receive state changes from the worker (auto-queued to main).
    void onWorkerEngineReady();
    void onWorkerSource(QUrl url);
    void onWorkerPosition(qint64 ms);
    void onWorkerDuration(qint64 ms);
    void onWorkerPlaying(bool playing);
    void onWorkerVolume(float v);
    void onWorkerTrackEnded();
    void onWorkerActiveTrack(int playlistIndex);
    void onWorkerReadyForNextTrack();
    // Default output device changed; reattach AudioClock probe so its
    // latency value tracks the new device. Mac-only behavior — on other
    // platforms this is a no-op slot.
    void onWorkerOutputDeviceChanged();

private:
    QThread*      m_thread = nullptr;  // parented to this (main thread)
    AudioWorker*  m_worker = nullptr;  // lives on m_thread; deleteLater on finish

    std::atomic<qint64>     m_positionCache    {0};
    std::atomic<qint64>     m_durationCache    {0};
    std::atomic<bool>       m_playingCache     {false};
    std::atomic<float>      m_volumeCache      {1.0f};
    std::atomic<int>        m_activeIndexCache {-1};
    // Calibration bias added on top of the analytic A/V-sync formula
    // (mac) or on top of the legacy 35 ms static lookahead (non-mac).
    // Default 0 so the formula is taken at face value on first launch.
    std::atomic<int>        m_syncCalibrationMsCache {0};
    std::atomic<bool>       m_outputIsBluetoothCache {false};
    std::atomic<EqEngine*>  m_eqCache                {nullptr};

    mutable QMutex m_sourceMutex;
    QUrl           m_sourceCache;

#ifdef Q_OS_MACOS
    // Two output-clock primitives: one for the speaker-emit anchor,
    // one for the wall-clock instant the next frame will appear on
    // screen. AudioWorker reads both via non-owning pointers (set up
    // after construction below) and feeds the FFT/Features tap from
    // the exact playhead sample for that scanout.
    std::unique_ptr<plyr::sync::AudioClock>   m_audioClock;
    std::unique_ptr<plyr::sync::DisplayClock> m_displayClock;
#endif
};
