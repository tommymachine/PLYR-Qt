// AudioWorker — the audio-thread half of the player.
//
// Owns the decode/pipe/sink/EQ chain. Lives on a dedicated QThread so
// Qt's internal audio-ring-buffer refill runs there, not on the main
// thread. That insulates playback from GUI stalls (modal dialogs, heavy
// QML relayouts, etc.) that would otherwise drain the ring and skip.
//
//   AudioEngine (main thread)       AudioWorker (audio thread)
//   ------------------------        --------------------------
//   Q_PROPERTY getters → atomics    QAudioDecoder
//   Q_INVOKABLE → emit request…  →  QAudioSink, PcmPipe, eq_engine_t
//   QML-facing signals              Position timer, segment bookkeeping
//                         ← signal  workerSourceChanged, workerPosition…
//
// Direction of flow:
//   - AudioEngine emits request… signals; AudioWorker slots receive them
//     on the audio thread.
//   - AudioWorker emits state signals; AudioEngine slots receive them
//     on the main thread, update atomic caches, re-emit QML-facing
//     signals for QML bindings to pick up.

#pragma once

#include "PcmPipe.h"
#include "eq_engine.h"

#include <QAudioDecoder>
#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QByteArray>
#include <QMediaDevices>
#include <QObject>
#include <QTimer>
#include <QUrl>
#include <atomic>
#include <memory>

class FftProcessor;

class AudioWorker : public QObject {
    Q_OBJECT

public:
    explicit AudioWorker(QObject* parent = nullptr);
    ~AudioWorker() override;

    // Called before thread.start(). Stores a non-owning pointer — the
    // FftProcessor lives on the main thread but its pushPcm() is thread-
    // safe (internal QMutex), so our DirectConnection emit from PcmPipe
    // delivering on the audio thread is fine.
    void setFftProcessor(FftProcessor* fft) { m_fft = fft; }

    // Thread-safe read of the EQ handle. Returns the pointer stored by
    // init() after the engine is created, or nullptr before that.
    EqEngine* eqEngine() const { return m_eqAtom.load(); }

    // Format is fixed at construction, safe to read from any thread.
    const QAudioFormat& format() const { return m_fmt; }

public slots:
    // init() runs on the audio thread when QThread::started fires.
    // Creates sink, decoder, pipe, timer, EQ; wires their signals.
    void init();
    // shutdown() tears everything down on the audio thread, invoked
    // BlockingQueued from AudioEngine's dtor before the thread quits.
    void shutdown();

    // Command slots — invoked via queued signals from AudioEngine.
    void play();
    void pause();
    void stop();
    void seek(qint64 ms);
    void setSource(const QUrl& u);
    void setVolume(float v);
    void playAt(int playlistIndex, const QUrl& url);
    void enqueueAt(int playlistIndex, const QUrl& url);

signals:
    // State-change notifications. Auto-queued to AudioEngine on main.
    void engineReady();
    void workerSourceChanged(QUrl url);
    void workerPositionChanged(qint64 ms);
    void workerDurationChanged(qint64 ms);
    void workerPlayingChanged(bool playing);
    void workerVolumeChanged(float volume);
    void workerTrackEnded();
    void workerActiveTrackChanged(int playlistIndex);
    void workerReadyForNextTrack();

private slots:
    void onBufferReady();
    void onDecoderFinished();
    void onSinkStateChanged(QtAudio::State state);
    void onSamplesServed(const char* data, qint64 bytes);
    void onAudioOutputsChanged();

private:
    void   startSinkIfNeeded();
    void   startNextInQueue();
    qint64 bytesPerMs() const;
    qint64 bytesPerSecond() const;
    qint64 currentPipeByte() const;
    qint64 computePositionMs() const;
    void   tickPosition();
    void   checkForSegmentTransition();

    // Tear down the current QAudioSink and rebuild a fresh one from the
    // canonical format. Reattaches to the pipe and resumes from the
    // pipe's current readPos. Used when the sink stops spontaneously
    // (device disconnect, audio-system reset) or the system default
    // output changes (user switches output device).
    void   recreateSink();

    struct Segment {
        qint64 startByte    = 0;
        qint64 endByte      = -1;
        qint64 durationMs   = 0;
        QUrl   source;
        int    playlistIndex = -1;
    };

    QAudioFormat                    m_fmt;
    std::unique_ptr<PcmPipe>        m_pipe;
    std::unique_ptr<QAudioDecoder>  m_decoder;
    std::unique_ptr<QAudioSink>     m_sink;
    std::unique_ptr<QTimer>         m_positionTimer;
    std::unique_ptr<QMediaDevices>  m_mediaDevices;
    QByteArray                      m_lastDefaultOutputId;
    EqEngine*                       m_eq = nullptr;
    std::atomic<EqEngine*>          m_eqAtom{nullptr};    // for cross-thread read
    FftProcessor*                   m_fft = nullptr;

    QUrl                            m_currentSource;
    qint64                          m_durationMs = 0;
    bool                            m_playing = false;
    float                           m_volume = 1.0f;
    bool                            m_decoderDone = false;
    bool                            m_sinkStarted = false;
    qint64                          m_sinkRestartPipeByte = 0;

    QVector<Segment>                m_segments;
    int                             m_currentSegmentIndex = 0;
    int                             m_decodingSegmentIndex = -1;
    QVector<QPair<QUrl, int>>       m_queue;
};
