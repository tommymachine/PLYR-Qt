#include "AudioEngine.h"
#include "FftProcessor.h"

#include <QAudioBuffer>

AudioEngine::AudioEngine(QObject* parent)
    : QObject(parent)
{
    m_player.setAudioOutput(&m_out);
    m_player.setAudioBufferOutput(&m_buf);

    connect(&m_player, &QMediaPlayer::sourceChanged,
            this,      &AudioEngine::sourceChanged);
    connect(&m_player, &QMediaPlayer::positionChanged,
            this,      &AudioEngine::positionChanged);
    connect(&m_player, &QMediaPlayer::durationChanged,
            this,      &AudioEngine::durationChanged);
    connect(&m_player, &QMediaPlayer::playbackStateChanged,
            this,      &AudioEngine::playingChanged);
    connect(&m_out,    &QAudioOutput::volumeChanged,
            this,      &AudioEngine::volumeChanged);
    connect(&m_player, &QMediaPlayer::mediaStatusChanged,
            this, [this](QMediaPlayer::MediaStatus s) {
                if (s == QMediaPlayer::EndOfMedia) emit trackEnded();
            });
    connect(&m_buf, &QAudioBufferOutput::audioBufferReceived,
            this, [this](const QAudioBuffer& buf) {
                if (m_fft) m_fft->pushBuffer(buf);
            });
}

void AudioEngine::setFftProcessor(FftProcessor* fft)
{
    m_fft = fft;
}

void AudioEngine::setSource(const QUrl& u)
{
    if (u == m_player.source()) return;
    m_player.setSource(u);
    if (!u.isEmpty()) m_player.play();
}

void AudioEngine::setVolume(float v)
{
    m_out.setVolume(v);
}

void AudioEngine::play()      { m_player.play(); }
void AudioEngine::pause()     { m_player.pause(); }
void AudioEngine::stop()      { m_player.stop(); }
void AudioEngine::seek(qint64 ms) { m_player.setPosition(ms); }
