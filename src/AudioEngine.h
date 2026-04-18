// Full cross-platform audio engine:
//
//   QAudioDecoder ──bufferReady──►  PcmPipe ──readData──► QAudioSink
//                                       │
//                                       └──samplesServed──► FftProcessor
//
// Owns the PCM path end-to-end, so the visualizer always gets the
// samples that are currently playing, regardless of Qt Multimedia
// backend. Seek is implemented by repositioning the pipe's playhead
// within the decoded buffer (the decoder decodes the whole track into
// memory — typical Rachmaninoff track is 40–110 MB at 44.1kHz/2ch/f32).

#pragma once

#include "PcmPipe.h"

#include <QAudioDecoder>
#include <QAudioFormat>
#include <QAudioSink>
#include <QObject>
#include <QTimer>
#include <QUrl>
#include <memory>

class FftProcessor;

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

    void setFftProcessor(FftProcessor* fft) { m_fft = fft; }
    const QAudioFormat& format() const { return m_fmt; }

    QUrl   source()   const { return m_source; }
    void   setSource(const QUrl& u);
    qint64 position() const;                    // ms
    qint64 duration() const { return m_durationMs; }
    bool   isPlaying() const { return m_playing; }
    float  volume()    const { return m_volume; }
    void   setVolume(float v);

    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void seek(qint64 ms);

signals:
    void sourceChanged();
    void positionChanged();
    void durationChanged();
    void playingChanged();
    void volumeChanged();
    void trackEnded();

private slots:
    void onBufferReady();
    void onDecoderFinished();
    void onSinkStateChanged(QtAudio::State state);
    void onSamplesServed(const char* data, qint64 bytes);

private:
    void startSinkIfNeeded();
    qint64 bytesPerMs() const;

    QUrl                         m_source;
    qint64                       m_durationMs = 0;
    bool                         m_playing    = false;
    float                        m_volume     = 1.0f;
    bool                         m_decoderDone = false;
    bool                         m_sinkStarted = false;

    QAudioFormat                 m_fmt;
    QAudioDecoder                m_decoder;
    std::unique_ptr<QAudioSink>  m_sink;
    PcmPipe                      m_pipe;
    FftProcessor*                m_fft = nullptr;
    QTimer                       m_positionTimer;

    // Position tracking. `processedUSecs()` is cumulative time rendered
    // since the most recent sink.start(); it resets on stop(). `m_baseUSec`
    // is the file-time at which the current start() began, so the real
    // position is m_baseUSec + processedUSecs (μs).
    qint64                       m_baseUSec = 0;
};
