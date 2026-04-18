// Simple playback wrapper around QMediaPlayer + QAudioOutput +
// QAudioBufferOutput. Lives in C++ because QAudioBufferOutput is not
// exposed as a QML type in this Qt version. Exposes the subset of
// QMediaPlayer we actually need, plus a direct signal for decoded audio
// buffers that the FFT processor hooks into.

#pragma once

#include <QAudioBufferOutput>
#include <QAudioOutput>
#include <QMediaPlayer>
#include <QObject>
#include <QUrl>

class FftProcessor;

class AudioEngine : public QObject {
    Q_OBJECT

    Q_PROPERTY(QUrl   source   READ source   WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(qint64 position READ position NOTIFY positionChanged)
    Q_PROPERTY(qint64 duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(bool   playing  READ isPlaying               NOTIFY playingChanged)
    Q_PROPERTY(float  volume   READ volume   WRITE setVolume NOTIFY volumeChanged)

public:
    explicit AudioEngine(QObject* parent = nullptr);

    // Route decoded audio buffers to the FFT processor.
    void setFftProcessor(FftProcessor* fft);

    QUrl   source() const            { return m_player.source(); }
    void   setSource(const QUrl& u);
    qint64 position() const          { return m_player.position(); }
    qint64 duration() const          { return m_player.duration(); }
    bool   isPlaying() const         { return m_player.playbackState() == QMediaPlayer::PlayingState; }
    float  volume() const            { return m_out.volume(); }
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

private:
    QMediaPlayer       m_player;
    QAudioOutput       m_out;
    QAudioBufferOutput m_buf;
    FftProcessor*      m_fft = nullptr;
};
