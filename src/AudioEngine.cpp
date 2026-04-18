#include "AudioEngine.h"
#include "FftProcessor.h"

#include <QAudioBuffer>
#include <QDebug>
#include <QtAudio>
#include <cstring>

namespace {
// Canonical internal format. Everything is resampled to this on decode
// so the sink, pipe, FFT, and position math all agree on a single shape.
QAudioFormat makeFormat()
{
    QAudioFormat f;
    f.setSampleRate(44100);
    f.setChannelCount(2);
    f.setSampleFormat(QAudioFormat::Float);
    return f;
}
}


AudioEngine::AudioEngine(QObject* parent)
    : QObject(parent)
    , m_fmt(makeFormat())
{
    m_decoder.setAudioFormat(m_fmt);

    m_sink = std::make_unique<QAudioSink>(m_fmt, this);
    m_sink->setVolume(m_volume);

    connect(&m_decoder, &QAudioDecoder::bufferReady,
            this, &AudioEngine::onBufferReady);
    connect(&m_decoder, &QAudioDecoder::finished,
            this, &AudioEngine::onDecoderFinished);
    connect(&m_decoder, &QAudioDecoder::durationChanged,
            this, [this](qint64 ms) {
                // QAudioDecoder fires one durationChanged with the real
                // length early on, then a second one with -1 when decode
                // finishes (its "no longer decoding" sentinel). Ignore
                // non-positive values so the real duration sticks — QML
                // binds slider `to:` against this.
                if (ms <= 0) return;
                m_durationMs = ms;
                emit durationChanged();
            });
    // `error` signal shares its name with the `error()` getter; use
    // QOverload to disambiguate.
    connect(&m_decoder, QOverload<QAudioDecoder::Error>::of(&QAudioDecoder::error),
            this, [](QAudioDecoder::Error e) {
                qWarning() << "QAudioDecoder error:" << e;
            });

    connect(m_sink.get(), &QAudioSink::stateChanged,
            this, &AudioEngine::onSinkStateChanged);

    connect(&m_pipe, &PcmPipe::samplesServed,
            this, &AudioEngine::onSamplesServed,
            Qt::DirectConnection);

    // Drive positionChanged at ~30 Hz so QML sliders feel smooth.
    m_positionTimer.setInterval(33);
    connect(&m_positionTimer, &QTimer::timeout,
            this, [this]() { emit positionChanged(); });
}

AudioEngine::~AudioEngine()
{
    m_decoder.stop();
    if (m_sink) m_sink->stop();
}


void AudioEngine::setSource(const QUrl& u)
{
    if (u == m_source) return;
    m_source = u;
    emit sourceChanged();

    qInfo() << "AudioEngine::setSource:" << u;

    // Tear down anything currently running.
    m_decoder.stop();
    if (m_sink) m_sink->stop();
    m_pipe.clearAll();
    m_decoderDone = false;
    m_sinkStarted = false;
    m_durationMs  = 0;
    m_baseUSec    = 0;
    emit durationChanged();
    emit positionChanged();

    if (u.isEmpty()) return;

    m_decoder.setSource(u);
    m_decoder.start();
}


void AudioEngine::setVolume(float v)
{
    if (std::abs(v - m_volume) < 1e-6f) return;
    m_volume = v;
    if (m_sink) m_sink->setVolume(v);
    emit volumeChanged();
}


void AudioEngine::play()
{
    if (!m_sink) return;
    if (m_sink->state() == QtAudio::SuspendedState) {
        m_sink->resume();
    } else if (!m_sinkStarted) {
        startSinkIfNeeded();
    }
    if (!m_playing) { m_playing = true; emit playingChanged(); m_positionTimer.start(); }
}

void AudioEngine::pause()
{
    if (m_sink && m_sink->state() == QtAudio::ActiveState) m_sink->suspend();
    if (m_playing) { m_playing = false; emit playingChanged(); m_positionTimer.stop(); }
}

void AudioEngine::stop()
{
    m_decoder.stop();
    if (m_sink) m_sink->stop();
    m_pipe.clearAll();
    m_sinkStarted = false;
    m_decoderDone = false;
    m_baseUSec    = 0;
    if (m_playing) { m_playing = false; emit playingChanged(); m_positionTimer.stop(); }
    emit positionChanged();
}


qint64 AudioEngine::bytesPerMs() const
{
    // m_fmt.bytesPerSample() * channels * sampleRate / 1000
    return qint64(m_fmt.bytesPerSample()) *
           m_fmt.channelCount() *
           m_fmt.sampleRate() / 1000;
}


qint64 AudioEngine::position() const
{
    // processedUSecs is time-of-audio-actually-rendered since start();
    // add the file-time this start() began at for the absolute position.
    const qint64 rendered = m_sinkStarted && m_sink
                          ? m_sink->processedUSecs()
                          : 0;
    return (m_baseUSec + rendered) / 1000;
}


void AudioEngine::seek(qint64 ms)
{
    if (ms < 0) ms = 0;
    if (m_durationMs > 0 && ms > m_durationMs) ms = m_durationMs;

    const qint64 bpm = bytesPerMs();
    qint64 byteOffset = ms * bpm;

    // Frame-align (don't slice through a sample).
    const qint64 bytesPerFrame = qint64(m_fmt.bytesPerSample()) * m_fmt.channelCount();
    byteOffset = (byteOffset / bytesPerFrame) * bytesPerFrame;

    // Clamp to what the decoder has produced so far.
    const qint64 avail = m_pipe.totalSize();
    if (byteOffset > avail) byteOffset = avail;

    // Reposition atomically: stop the sink so it's not mid-read, seek
    // the pipe, start the sink again. Preserves playing/paused state.
    // Update m_baseUSec to the new file-time; processedUSecs will reset
    // to 0 on the next start(), and position() adds base + rendered.
    const bool wasPlaying = m_playing;
    if (m_sink) m_sink->stop();
    m_sinkStarted = false;
    m_baseUSec    = ms * 1000;

    m_pipe.seekToPos(byteOffset);

    startSinkIfNeeded();
    if (!wasPlaying && m_sink) m_sink->suspend();

    emit positionChanged();
}


void AudioEngine::startSinkIfNeeded()
{
    if (m_sinkStarted) return;
    if (!m_sink) return;
    // Give the pipe to the sink in pull mode.
    m_sink->start(&m_pipe);
    m_sinkStarted = true;
}


void AudioEngine::onBufferReady()
{
    while (m_decoder.bufferAvailable()) {
        const QAudioBuffer buf = m_decoder.read();
        const auto bytes = buf.byteCount();
        if (bytes <= 0) continue;
        m_pipe.append(QByteArray(
            reinterpret_cast<const char*>(buf.constData<char>()), bytes));
    }

    // Start the sink once we have something to play. Honor paused state.
    if (!m_sinkStarted) {
        startSinkIfNeeded();
        if (m_playing) {
            // nothing extra; start() already active
        } else {
            // No explicit play() yet — leave suspended.
            if (m_sink) m_sink->suspend();
        }
        if (!m_playing) {
            // Auto-play on source change, matching earlier UX.
            m_playing = true;
            emit playingChanged();
            if (m_sink && m_sink->state() == QtAudio::SuspendedState)
                m_sink->resume();
            m_positionTimer.start();
        }
    }
}


void AudioEngine::onDecoderFinished()
{
    m_decoderDone = true;
}


void AudioEngine::onSinkStateChanged(QtAudio::State state)
{
    // When the sink runs out of data AND the decoder is done, we've
    // finished playing the whole track.
    if (state == QtAudio::IdleState && m_decoderDone) {
        if (m_playing) { m_playing = false; emit playingChanged(); m_positionTimer.stop(); }
        emit trackEnded();
    }
}


void AudioEngine::onSamplesServed(const char* data, qint64 bytes)
{
    if (m_fft) m_fft->pushPcm(data, bytes, m_fmt);
}
