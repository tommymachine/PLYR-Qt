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

#include <QMutex>
#include <QObject>
#include <QUrl>
#include <atomic>

class AudioWorker;
class FftProcessor;
class QThread;

class AudioEngine : public QObject {
    Q_OBJECT

    Q_PROPERTY(QUrl   source   READ source   WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(qint64 position READ position                 NOTIFY positionChanged)
    Q_PROPERTY(qint64 duration READ duration                 NOTIFY durationChanged)
    Q_PROPERTY(bool   playing  READ isPlaying                NOTIFY playingChanged)
    Q_PROPERTY(float  volume   READ volume   WRITE setVolume NOTIFY volumeChanged)

public:
    explicit AudioEngine(QObject* parent = nullptr);
    ~AudioEngine() override;

    // Called before anything else ticks. Worker hasn't started its thread
    // yet, so a plain pointer store is safe.
    void setFftProcessor(FftProcessor* fft);

    // EQ handle. Returns nullptr until AudioWorker::init() has created it
    // (signalled via engineReady below). EqController waits on that
    // signal before touching the DSP.
    EqEngine* eqEngine() const { return m_eqCache.load(); }

    // --- Property reads: atomic caches updated by queued signals from
    //     the worker. Main-thread-safe.
    QUrl   source()      const;
    qint64 position()    const { return m_positionCache.load();  }
    qint64 duration()    const { return m_durationCache.load();  }
    bool   isPlaying()   const { return m_playingCache.load();   }
    float  volume()      const { return m_volumeCache.load();    }
    int    activeIndex() const { return m_activeIndexCache.load();}

    // --- Setters: emit queued signals to the worker; update caches so
    //     QML reflects the change immediately (avoids one frame of lag).
    void   setSource(const QUrl& u);
    void   setVolume(float v);

    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void seek(qint64 ms);
    Q_INVOKABLE void playAt   (int playlistIndex, const QUrl& url);
    Q_INVOKABLE void enqueueAt(int playlistIndex, const QUrl& url);

signals:
    // QML-facing change signals.
    void sourceChanged();
    void positionChanged();
    void durationChanged();
    void playingChanged();
    void volumeChanged();
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
    void requestPlayAt(int playlistIndex, QUrl url);
    void requestEnqueueAt(int playlistIndex, QUrl url);

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

private:
    QThread*      m_thread = nullptr;  // parented to this (main thread)
    AudioWorker*  m_worker = nullptr;  // lives on m_thread; deleteLater on finish

    std::atomic<qint64>     m_positionCache    {0};
    std::atomic<qint64>     m_durationCache    {0};
    std::atomic<bool>       m_playingCache     {false};
    std::atomic<float>      m_volumeCache      {1.0f};
    std::atomic<int>        m_activeIndexCache {-1};
    std::atomic<EqEngine*>  m_eqCache          {nullptr};

    mutable QMutex m_sourceMutex;
    QUrl           m_sourceCache;
};
