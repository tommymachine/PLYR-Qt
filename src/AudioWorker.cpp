#include "AudioWorker.h"
#include "FftProcessor.h"

#include <QAudioBuffer>
#include <QDebug>
#include <QThread>
#include <QtAudio>
#include <cstring>

namespace {
QAudioFormat makeFormat()
{
    QAudioFormat f;
    f.setSampleRate(44100);
    f.setChannelCount(2);
    f.setSampleFormat(QAudioFormat::Float);
    return f;
}
}  // namespace


AudioWorker::AudioWorker(QObject* parent)
    : QObject(parent)
    , m_fmt(makeFormat())
{
    // Ctor must not allocate any QObject children. They are all created
    // in init(), which runs after moveToThread() on the audio thread, so
    // they get the correct thread affinity.
}

AudioWorker::~AudioWorker() = default;


void AudioWorker::init()
{
    Q_ASSERT(QThread::currentThread() == thread());

    m_pipe          = std::make_unique<PcmPipe>();
    m_decoder       = std::make_unique<QAudioDecoder>();
    m_sink          = std::make_unique<QAudioSink>(m_fmt);
    m_positionTimer = std::make_unique<QTimer>();

    m_decoder->setAudioFormat(m_fmt);
    m_sink->setVolume(m_volume);

    // Half-second internal ring buffer (default is 250 ms). With audio on
    // its own thread this is mostly belt-and-suspenders, but cheap.
    const qint64 bufferBytes =
        qint64(m_fmt.bytesPerSample()) *
        m_fmt.channelCount() *
        m_fmt.sampleRate() / 2;
    m_sink->setBufferSize(bufferBytes);

    m_eq = eq_create(m_fmt.sampleRate(), m_fmt.channelCount());
    eq_set_bypass(m_eq, 1);
    m_pipe->setEqEngine(m_eq);
    m_pipe->setFloatFrameLayout(m_fmt.channelCount());
    m_eqAtom.store(m_eq);

    connect(m_decoder.get(), &QAudioDecoder::bufferReady,
            this, &AudioWorker::onBufferReady);
    connect(m_decoder.get(), &QAudioDecoder::finished,
            this, &AudioWorker::onDecoderFinished);
    connect(m_decoder.get(), &QAudioDecoder::durationChanged,
            this, [this](qint64 ms) {
                if (ms <= 0) return;
                if (m_decodingSegmentIndex >= 0 &&
                    m_decodingSegmentIndex < m_segments.size())
                {
                    m_segments[m_decodingSegmentIndex].durationMs = ms;
                }
                if (m_decodingSegmentIndex == m_currentSegmentIndex) {
                    m_durationMs = ms;
                    emit workerDurationChanged(ms);
                }
            });
    connect(m_decoder.get(),
            QOverload<QAudioDecoder::Error>::of(&QAudioDecoder::error),
            this, [](QAudioDecoder::Error e) {
                qWarning() << "QAudioDecoder error:" << e;
            });

    connect(m_sink.get(), &QAudioSink::stateChanged,
            this, &AudioWorker::onSinkStateChanged);

    // DirectConnection: pipe->samplesServed fires inside PcmPipe::readData
    // on this same (audio) thread. The slot calls FftProcessor::pushPcm
    // which is thread-safe (internal QMutex).
    connect(m_pipe.get(), &PcmPipe::samplesServed,
            this, &AudioWorker::onSamplesServed,
            Qt::DirectConnection);

    m_positionTimer->setInterval(33);      // ~30 Hz
    connect(m_positionTimer.get(), &QTimer::timeout,
            this, &AudioWorker::tickPosition);

    // Watch for system audio-output device changes (user switches default
    // output, device unplug/replug). On change we tear down the sink
    // and recreate it against the new default to avoid Qt's modernized
    // sink getting stuck on a stale stream.
    m_mediaDevices = std::make_unique<QMediaDevices>();
    m_lastDefaultOutputId = QMediaDevices::defaultAudioOutput().id();
    connect(m_mediaDevices.get(), &QMediaDevices::audioOutputsChanged,
            this, &AudioWorker::onAudioOutputsChanged);

    emit engineReady();
}


void AudioWorker::shutdown()
{
    Q_ASSERT(QThread::currentThread() == thread());

    if (m_decoder) m_decoder->stop();
    // Mark sinkStarted=false BEFORE stopping so onSinkStateChanged's
    // unexpected-stop branch doesn't fire and trigger a recreate during
    // shutdown.
    m_sinkStarted = false;
    if (m_sink)    m_sink->stop();
    if (m_pipe)    m_pipe->setEqEngine(nullptr);
    if (m_eq) { eq_destroy(m_eq); m_eq = nullptr; m_eqAtom.store(nullptr); }

    m_mediaDevices.reset();
    m_positionTimer.reset();
    m_sink.reset();
    m_decoder.reset();
    m_pipe.reset();
}


// ---- Command slots (run on audio thread via queued signals) ---------

void AudioWorker::setSource(const QUrl& u)
{
    playAt(-1, u);
}


void AudioWorker::setVolume(float v)
{
    if (std::abs(v - m_volume) < 1e-6f) return;
    m_volume = v;
    if (m_sink) m_sink->setVolume(v);
    emit workerVolumeChanged(v);
}


void AudioWorker::play()
{
    if (!m_sink) return;
    if (m_sink->state() == QtAudio::SuspendedState) {
        m_sink->resume();
    } else if (!m_sinkStarted) {
        startSinkIfNeeded();
    }
    if (!m_playing) {
        m_playing = true;
        emit workerPlayingChanged(true);
        if (m_positionTimer) m_positionTimer->start();
    }
}


void AudioWorker::pause()
{
    if (m_sink && m_sink->state() == QtAudio::ActiveState) m_sink->suspend();
    if (m_playing) {
        m_playing = false;
        emit workerPlayingChanged(false);
        if (m_positionTimer) m_positionTimer->stop();
    }
}


void AudioWorker::stop()
{
    // Order matters: clear m_sinkStarted FIRST so onSinkStateChanged's
    // "unexpected stop" branch doesn't fire when stop() goes synchronous.
    m_sinkStarted = false;
    if (m_decoder) m_decoder->stop();
    if (m_sink)    m_sink->stop();
    if (m_pipe)    m_pipe->clearAll();
    m_segments.clear();
    m_queue.clear();
    m_currentSegmentIndex  = 0;
    m_decodingSegmentIndex = -1;
    m_decoderDone = false;
    m_sinkRestartPipeByte = 0;
    if (m_playing) {
        m_playing = false;
        emit workerPlayingChanged(false);
        if (m_positionTimer) m_positionTimer->stop();
    }
    emit workerPositionChanged(0);
}


void AudioWorker::seek(qint64 ms)
{
    if (m_segments.isEmpty() || m_currentSegmentIndex >= m_segments.size())
        return;

    const auto& seg = m_segments[m_currentSegmentIndex];
    if (ms < 0) ms = 0;
    if (seg.durationMs > 0 && ms > seg.durationMs) ms = seg.durationMs;

    const qint64 bps            = bytesPerSecond();
    qint64       byteInSeg      = (ms * bps) / 1000;
    const qint64 bytesPerFrame  = qint64(m_fmt.bytesPerSample()) * m_fmt.channelCount();
    byteInSeg = (byteInSeg / bytesPerFrame) * bytesPerFrame;

    qint64 targetPipeByte = seg.startByte + byteInSeg;
    const qint64 avail = m_pipe ? m_pipe->totalSize() : 0;
    if (targetPipeByte > avail) targetPipeByte = avail;

    const bool wasPlaying = m_playing;
    // Clear sinkStarted before stop() so the synchronous state change
    // doesn't get treated as an unexpected stop and trigger recreate.
    m_sinkStarted = false;
    if (m_sink) m_sink->stop();
    if (m_pipe) m_pipe->seekToPos(targetPipeByte);

    startSinkIfNeeded();
    if (!wasPlaying && m_sink) m_sink->suspend();

    emit workerPositionChanged(computePositionMs());
}


void AudioWorker::playAt(int playlistIndex, const QUrl& url)
{
    qInfo() << "AudioWorker::playAt:" << playlistIndex << url;

    // Clear sinkStarted FIRST so onSinkStateChanged sees this as an
    // intentional stop, not an unexpected one.
    m_sinkStarted = false;
    if (m_decoder) m_decoder->stop();
    if (m_sink)    m_sink->stop();
    if (m_pipe)    m_pipe->clearAll();
    m_segments.clear();
    m_queue.clear();
    m_currentSegmentIndex  = 0;
    m_decodingSegmentIndex = -1;
    m_decoderDone = false;
    m_sinkRestartPipeByte = 0;
    m_durationMs = 0;

    m_currentSource = url;
    emit workerSourceChanged(url);
    emit workerDurationChanged(0);
    emit workerPositionChanged(0);

    if (url.isEmpty()) return;

    Segment s;
    s.startByte     = 0;
    s.endByte       = -1;
    s.source        = url;
    s.playlistIndex = playlistIndex;
    m_segments.append(s);
    m_decodingSegmentIndex = 0;

    if (m_decoder) {
        m_decoder->setSource(url);
        m_decoder->start();
    }

    emit workerActiveTrackChanged(playlistIndex);
}


void AudioWorker::enqueueAt(int playlistIndex, const QUrl& url)
{
    if (url.isEmpty()) return;
    for (const auto& s : m_segments)
        if (s.playlistIndex == playlistIndex) return;
    for (const auto& q : m_queue)
        if (q.second == playlistIndex) return;

    qInfo() << "AudioWorker::enqueueAt:" << playlistIndex << url;
    m_queue.append({url, playlistIndex});

    if (m_decoderDone) startNextInQueue();
}


// ---- Audio-thread helpers ----------------------------------------------

void AudioWorker::startNextInQueue()
{
    if (m_queue.isEmpty()) return;

    auto next = m_queue.takeFirst();
    const QUrl& url = next.first;
    const int   idx = next.second;

    if (m_decodingSegmentIndex >= 0 && m_decodingSegmentIndex < m_segments.size())
        m_segments[m_decodingSegmentIndex].endByte =
            m_pipe ? m_pipe->totalSize() : 0;

    Segment s;
    s.startByte     = m_pipe ? m_pipe->totalSize() : 0;
    s.endByte       = -1;
    s.source        = url;
    s.playlistIndex = idx;
    m_segments.append(s);
    m_decodingSegmentIndex = m_segments.size() - 1;
    m_decoderDone = false;

    if (m_decoder) {
        m_decoder->stop();
        m_decoder->setSource(url);
        m_decoder->start();
    }
}


qint64 AudioWorker::bytesPerSecond() const
{
    return qint64(m_fmt.bytesPerSample()) *
           m_fmt.channelCount() *
           m_fmt.sampleRate();
}

qint64 AudioWorker::bytesPerMs() const
{
    return bytesPerSecond() / 1000;
}

qint64 AudioWorker::currentPipeByte() const
{
    if (!m_sinkStarted || !m_sink) return m_sinkRestartPipeByte;
    const qint64 playedBytes =
        (m_sink->processedUSecs() * bytesPerSecond()) / 1000000;
    return m_sinkRestartPipeByte + playedBytes;
}

qint64 AudioWorker::computePositionMs() const
{
    if (m_segments.isEmpty()) return 0;
    const qint64 segStart =
        (m_currentSegmentIndex < m_segments.size())
        ? m_segments[m_currentSegmentIndex].startByte : 0;
    const qint64 inSegment = std::max<qint64>(0, currentPipeByte() - segStart);
    const qint64 bps = bytesPerSecond();
    return bps > 0 ? (inSegment * 1000 / bps) : 0;
}

void AudioWorker::tickPosition()
{
    emit workerPositionChanged(computePositionMs());
    checkForSegmentTransition();
}

void AudioWorker::startSinkIfNeeded()
{
    if (m_sinkStarted) return;
    if (!m_sink || !m_pipe) return;
    m_sinkRestartPipeByte = m_pipe->readPos();
    m_sink->start(m_pipe.get());
    m_sinkStarted = true;
}


void AudioWorker::onBufferReady()
{
    if (!m_decoder || !m_pipe) return;
    while (m_decoder->bufferAvailable()) {
        const QAudioBuffer buf = m_decoder->read();
        const auto bytes = buf.byteCount();
        if (bytes <= 0) continue;
        m_pipe->append(QByteArray(
            reinterpret_cast<const char*>(buf.constData<char>()), bytes));
    }

    if (!m_sinkStarted) {
        startSinkIfNeeded();
        if (!m_playing) {
            if (m_sink) m_sink->suspend();
            m_playing = true;
            emit workerPlayingChanged(true);
            if (m_sink && m_sink->state() == QtAudio::SuspendedState)
                m_sink->resume();
            if (m_positionTimer) m_positionTimer->start();
        }
    }
}


void AudioWorker::onDecoderFinished()
{
    if (m_decodingSegmentIndex >= 0 &&
        m_decodingSegmentIndex < m_segments.size())
    {
        auto& seg = m_segments[m_decodingSegmentIndex];
        seg.endByte = m_pipe ? m_pipe->totalSize() : 0;

        const qint64 headroom = seg.endByte - currentPipeByte();
        const qint64 bps      = bytesPerSecond();
        const double headroomSec = bps > 0 ? double(headroom) / double(bps) : 0.0;
        qInfo().noquote()
            << "[gapless] decoder finished idx" << seg.playlistIndex
            << "| endByte =" << seg.endByte
            << "| pipe-read =" << currentPipeByte()
            << "| head-room =" << headroom
            << QString::asprintf("(%.2f s)", headroomSec)
            << "| queued =" << m_queue.size();
    }
    m_decoderDone = true;

    if (!m_queue.isEmpty()) {
        startNextInQueue();
    } else {
        emit workerReadyForNextTrack();
    }
}


void AudioWorker::checkForSegmentTransition()
{
    if (m_segments.isEmpty() || !m_sinkStarted || !m_pipe) return;
    const qint64 b = currentPipeByte();

    for (int i = m_currentSegmentIndex; i < m_segments.size(); ++i) {
        const auto& seg = m_segments[i];
        if (seg.endByte > 0 && b >= seg.endByte) continue;
        if (b >= seg.startByte) {
            if (i != m_currentSegmentIndex) {
                const auto& prev = m_segments[m_currentSegmentIndex];
                const qint64 underruns = m_pipe->underrunCount();
                const qint64 undBytes  = m_pipe->underrunBytes();
                const qint64 served    = m_pipe->totalBytesServed();
                const qint64 expectedAtBoundary =
                    (prev.endByte > 0) ? prev.endByte : seg.startByte;
                qInfo().noquote()
                    << "[gapless] crossing"
                    << QString("%1 → %2").arg(prev.playlistIndex).arg(seg.playlistIndex)
                    << "at pipe-byte" << b
                    << "| prev.end =" << prev.endByte
                    << "| new.start =" << seg.startByte
                    << "| bytes-served =" << served
                    << "| expected-at-boundary =" << expectedAtBoundary
                    << "| underruns =" << underruns
                    << QString("(%1 B short)").arg(undBytes);

                m_currentSegmentIndex = i;
                m_durationMs          = seg.durationMs;
                m_currentSource       = seg.source;
                emit workerSourceChanged(seg.source);
                emit workerDurationChanged(seg.durationMs);
                emit workerActiveTrackChanged(seg.playlistIndex);
            }
            break;
        }
    }
}


void AudioWorker::onSinkStateChanged(QtAudio::State state)
{
    const char* s = "?";
    switch (state) {
        case QtAudio::ActiveState:    s = "Active";    break;
        case QtAudio::IdleState:      s = "Idle";      break;
        case QtAudio::SuspendedState: s = "Suspended"; break;
        case QtAudio::StoppedState:   s = "Stopped";   break;
    }
    qInfo().noquote()
        << "[gapless] sink state =" << s
        << "| pipe-read =" << currentPipeByte()
        << "| pipe-total =" << (m_pipe ? m_pipe->totalSize() : 0)
        << "| underruns =" << (m_pipe ? m_pipe->underrunCount() : 0);

    // Unexpected stop while we still wanted the sink active = the audio
    // device went away (USB unplug, sample-rate reconfigure on sleep/wake,
    // OS audio-system reset). Recreate against the current default output
    // so playback can resume without a crash on next pipe append.
    if (state == QtAudio::StoppedState && m_sinkStarted) {
        qWarning() << "[gapless] sink stopped unexpectedly (likely device "
                      "change or error) — recreating sink";
        recreateSink();
        return;
    }

    if (state == QtAudio::IdleState) {
        const bool moreToCome =
            !m_queue.isEmpty() ||
            (m_currentSegmentIndex + 1 < m_segments.size()) ||
            !m_decoderDone;
        if (moreToCome) {
            qWarning() << "[gapless] !!! sink went Idle with more to play"
                       << "— this is the gap. Re-kicking sink.";
            m_sinkStarted = false;
            startSinkIfNeeded();
            return;
        }
        if (m_decoderDone) {
            if (m_playing) {
                m_playing = false;
                emit workerPlayingChanged(false);
                if (m_positionTimer) m_positionTimer->stop();
            }
            emit workerTrackEnded();
        }
    }
}


void AudioWorker::onAudioOutputsChanged()
{
    // Many spurious fires (system audio panel briefly opening, app focus,
    // etc.) — only act if the system default output actually changed.
    const QByteArray newDefault = QMediaDevices::defaultAudioOutput().id();
    if (newDefault == m_lastDefaultOutputId) return;
    qInfo().noquote()
        << "[gapless] default audio output changed —"
        << "old:" << m_lastDefaultOutputId
        << "new:" << newDefault;
    m_lastDefaultOutputId = newDefault;
    recreateSink();
}


void AudioWorker::recreateSink()
{
    qInfo() << "[gapless] recreating QAudioSink";

    const bool wasStarted = m_sinkStarted;

    // Detach and tear down the old sink. Disconnect our slots first so its
    // trailing state notifications don't recurse back into recreate. The
    // old sink may still be on the call stack (if recreate was invoked
    // from its own stateChanged slot), so use deleteLater to defer the
    // actual destruction to the next event-loop tick.
    QAudioSink* old = m_sink.release();
    if (old) {
        QObject::disconnect(old, nullptr, this, nullptr);
        old->stop();
        old->deleteLater();
    }
    m_sinkStarted = false;

    // Build a fresh sink with the same canonical format, volume, and
    // buffer config. macOS will pick up the current default output here.
    m_sink = std::make_unique<QAudioSink>(m_fmt);
    m_sink->setVolume(m_volume);
    const qint64 bufferBytes = qint64(m_fmt.bytesPerSample()) *
                                m_fmt.channelCount() *
                                m_fmt.sampleRate() / 2;     // 0.5 s
    m_sink->setBufferSize(bufferBytes);
    connect(m_sink.get(), &QAudioSink::stateChanged,
            this, &AudioWorker::onSinkStateChanged);

    // Resume from the pipe's current readPos, preserving play/pause state.
    if (wasStarted && m_pipe) {
        m_sinkRestartPipeByte = m_pipe->readPos();
        m_sink->start(m_pipe.get());
        m_sinkStarted = true;
        if (!m_playing) m_sink->suspend();
    }
}


void AudioWorker::onSamplesServed(const char* data, qint64 bytes)
{
    if (m_fft) m_fft->pushPcm(data, bytes, m_fmt);
}
