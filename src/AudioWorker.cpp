#include "AudioWorker.h"
#include "AudioFeatures.h"
#include "FftProcessor.h"

#ifdef Q_OS_MACOS
#include "AudioClock_macOS.h"
#include "DisplayClock_macOS.h"
#endif

#include <QAudioBuffer>
#include <QDebug>
#include <QThread>
#include <QtAudio>
#include <algorithm>
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

    m_pipe           = std::make_unique<PcmPipe>();
    m_decoder        = std::make_unique<QAudioDecoder>();
    m_sink           = std::make_unique<QAudioSink>(m_fmt);
    m_positionTimer  = std::make_unique<QTimer>();
    m_lookaheadTimer = std::make_unique<QTimer>();

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

    // The FFT/AudioFeatures used to subscribe to PcmPipe::samplesServed
    // here (DirectConnection), seeing audio at the moment it was copied
    // into QAudioSink's period buffer. That made the visualizer trail
    // the actual audio output by ~15–25 ms (sink → CoreAudio path)
    // minus the visual pipeline (~20–35 ms compose+vsync), which
    // landed slightly behind on punchy material.
    //
    // The lookahead tap below feeds them from a peek that's computed
    // analytically on macOS (AudioClock anchor + DisplayClock target
    // − output latency, all in nanosecond-accurate seconds) or from a
    // static 35 ms fallback elsewhere. By the time a frame is composited
    // and presented, the audio those samples represent is the audio
    // actually leaving the speakers. `samplesServed` is still emitted
    // from the pipe for any future diagnostic consumer, but the FFT /
    // features pipeline no longer listens to it.

    m_positionTimer->setInterval(33);      // ~30 Hz
    connect(m_positionTimer.get(), &QTimer::timeout,
            this, &AudioWorker::tickPosition);

    // Lookahead tap. 60 Hz so the FFT/features see roughly one fresh
    // slice per visualized frame at 60 fps. Runs on the audio thread
    // (same thread as PcmPipe::readData), so peek + push are entirely
    // on this thread and contend with the sink only on the pipe mutex.
    //
    // Each tick re-anchors to (playhead + lookaheadMs) and copies one
    // scratch buffer worth of bytes. 16 ms × bytesPerMs ≈ 5.6 kB at
    // 44.1 kHz stereo float32 — small enough to memcpy under the pipe
    // lock in microseconds, large enough to overlap consecutive ticks
    // (16.7 ms apart) so no audio frames get skipped over. Pre-sized
    // once here; the tick path never allocates.
    m_lookaheadTimer->setInterval(16);     // ~60 Hz
    const qint64 scratchCap = 16 * bytesPerMs();  // ~5.6 kB at 44.1 kHz
    m_lookaheadScratch.resize(int(scratchCap));
    connect(m_lookaheadTimer.get(), &QTimer::timeout,
            this, &AudioWorker::tickLookahead);

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
    m_lookaheadTimer.reset();
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
        if (m_positionTimer)  m_positionTimer->start();
        if (m_lookaheadTimer) m_lookaheadTimer->start();
    }
}


void AudioWorker::pause()
{
    if (m_sink && m_sink->state() == QtAudio::ActiveState) m_sink->suspend();
    if (m_playing) {
        m_playing = false;
        emit workerPlayingChanged(false);
        if (m_positionTimer)  m_positionTimer->stop();
        if (m_lookaheadTimer) m_lookaheadTimer->stop();
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
        if (m_positionTimer)  m_positionTimer->stop();
        if (m_lookaheadTimer) m_lookaheadTimer->stop();
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

    // If we're switching from preview-stream mode (CD ripper feeding
    // the pipe), drop the streaming flag so any in-flight pushPreviewPcm
    // becomes a no-op rather than writing into the pipe the decoder is
    // about to take over.
    m_previewStreamActive = false;

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


void AudioWorker::startPreviewStream(qint64 totalDurationMs, qint64 startOffsetMs)
{
    Q_ASSERT(QThread::currentThread() == thread());

    // Tear down any file-playback state cleanly first. Same dance as
    // playAt() but minus the decoder kickoff.
    m_sinkStarted = false;
    if (m_decoder) m_decoder->stop();
    if (m_sink)    m_sink->stop();
    if (m_pipe)    m_pipe->clearAll();
    m_segments.clear();
    m_queue.clear();
    m_currentSegmentIndex  = 0;
    m_decodingSegmentIndex = -1;
    m_decoderDone = true;          // no decoder running
    m_sinkRestartPipeByte = 0;
    m_durationMs = totalDurationMs;

    // Synthetic single segment representing the streaming preview. The
    // sink reads from the pipe; pushPreviewPcm() feeds the pipe.
    // durationMs comes from the disc TOC ((leadOut - firstTrack)/75 × 1000),
    // so the scrubber + duration label work the same as during a
    // normal QAudioDecoder playback.
    //
    // On resume (startOffsetMs > 0), the read picks up partway through
    // the disc — so the first bytes the audio engine sees are NOT from
    // disc-sector 0. We offset the position counter by setting the
    // segment's startByte to a negative byte count corresponding to
    // startOffsetMs. computePositionMs sees (pipeByte=0 − negativeStart)
    // and reports the right disc-relative position from the first frame.
    const qint64 bpms = bytesPerMs();
    const qint64 offsetBytes = startOffsetMs * bpms;
    Segment s;
    s.startByte     = -offsetBytes;
    s.endByte       = -1;
    s.source        = QUrl(QStringLiteral("preview://cd-rip"));
    s.playlistIndex = -1;
    s.durationMs    = totalDurationMs;
    m_segments.append(s);
    m_currentSegmentIndex = 0;
    m_decodingSegmentIndex = 0;

    m_currentSource = s.source;
    emit workerSourceChanged(s.source);
    emit workerDurationChanged(totalDurationMs);
    emit workerPositionChanged(startOffsetMs);

    m_previewStreamActive = true;
    startSinkIfNeeded();
    // Treat as playing — even before bytes arrive the sink is "Active"
    // waiting on the first read, and we want the SCANCERTO transport
    // (and the main player's transport) to show ⏸ from the moment the
    // rip kicks off rather than after the first PCM chunk lands.
    // Always emit/restart even if m_playing was already true so the
    // QML rebinds and the position timer ticks against the new
    // segment.
    m_playing = true;
    emit workerPlayingChanged(true);
    if (m_positionTimer)  m_positionTimer->start();
    if (m_lookaheadTimer) m_lookaheadTimer->start();
}


void AudioWorker::pushPreviewPcm(QByteArray int16Bytes)
{
    if (!m_previewStreamActive || !m_pipe) return;
    if (int16Bytes.isEmpty()) return;

    // CDDA is interleaved int16 LE stereo @ 44.1 kHz. The sink's pipe
    // format is interleaved float32 @ 44.1 kHz (same SR + channel
    // count). Convert in place into a contiguous float32 buffer, then
    // append to the pipe.
    const int16_t* src =
        reinterpret_cast<const int16_t*>(int16Bytes.constData());
    const qsizetype sampleCount =
        int16Bytes.size() / qsizetype(sizeof(int16_t));

    QByteArray floatBytes;
    floatBytes.resize(sampleCount * qsizetype(sizeof(float)));
    float* dst = reinterpret_cast<float*>(floatBytes.data());

    constexpr float kInv = 1.0f / 32768.0f;
    for (qsizetype i = 0; i < sampleCount; ++i) {
        dst[i] = float(src[i]) * kInv;
    }

    m_pipe->append(floatBytes);
}


void AudioWorker::stopPreviewStream()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_previewStreamActive) return;
    m_previewStreamActive = false;
    // Mirror stop() for the rest of the teardown — the next file
    // playback path comes via playAt() in main.cpp.
    m_sinkStarted = false;
    if (m_sink) m_sink->stop();
    if (m_pipe) m_pipe->clearAll();
    m_segments.clear();
    m_currentSegmentIndex  = 0;
    m_decodingSegmentIndex = -1;
    m_decoderDone = false;
    m_sinkRestartPipeByte = 0;
    if (m_playing) {
        m_playing = false;
        emit workerPlayingChanged(false);
        if (m_positionTimer)  m_positionTimer->stop();
        if (m_lookaheadTimer) m_lookaheadTimer->stop();
    }
    emit workerPositionChanged(0);
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
            if (m_positionTimer)  m_positionTimer->start();
            if (m_lookaheadTimer) m_lookaheadTimer->start();
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
        // Streaming preview is an open-ended source — the RipWorker
        // keeps pushing CDDA chunks as the disc reads. Pipe gaps
        // between chunks are EXPECTED, not "track ended". Re-kick the
        // sink so it picks up the next chunk and keep m_playing /
        // position timer running. Otherwise the SCANCERTO transport
        // would freeze the moment the worker pauses for a millisecond
        // (which it does between reads).
        if (m_previewStreamActive) {
            m_sinkStarted = false;
            startSinkIfNeeded();
            return;
        }

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
                if (m_positionTimer)  m_positionTimer->stop();
                if (m_lookaheadTimer) m_lookaheadTimer->stop();
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
    // Engine listens for this and re-attaches the AudioClock probe AU
    // to the new default device so its latency value follows the change.
    emit workerOutputDeviceChanged();
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


void AudioWorker::tickLookahead()
{
    // Audio-thread tick. Compute how far ahead of the sink's current
    // readPos to peek in the pipe, then push that slice to the FFT and
    // AudioFeatures.
    //
    // macOS path (analytic): solve for the playhead sample at the next
    // scanout instant T_pixel:
    //   s_f = S_a + (T_pixel − host_to_seconds(H_a) − L_out) · SR
    // where (H_a, S_a, SR, L_out) come from AudioClock's seqlock anchor
    // and T_pixel comes from DisplayClock's CADisplayLink targetTimestamp.
    // Because the AU has been consuming at SR samples/sec since H_a, the
    // S_a/H_a anchor algebraically cancels out:
    //   leadFrames = (T_pixel − t_now − L_out) · SR
    // — but we still validate the anchor exists (proves the AU is alive)
    // and read SR + L_out from it.
    //
    // Fallback (non-Mac, attach failed, or no callbacks yet): use the
    // static 35 ms lookahead the engine shipped with before the analytic
    // path landed. Calibration is added on top either way.
    //
    // Allocation-free: m_lookaheadScratch is pre-sized in init().
    if (!m_pipe || m_lookaheadScratch.isEmpty()) return;
    if (!m_fft && !m_features) return;

    const qint64 bpms = bytesPerMs();
    if (bpms <= 0) return;
    const qint64 bytesPerFrame =
        qint64(m_fmt.bytesPerSample()) * m_fmt.channelCount();
    if (bytesPerFrame <= 0) return;

    qint64 offsetBytes = -1;  // -1 = analytic not available, use fallback

#ifdef Q_OS_MACOS
    concerto::sync::AudioClock*   ac =
        m_audioClockAtom.load(std::memory_order_acquire);
    concerto::sync::DisplayClock* dc =
        m_displayClockAtom.load(std::memory_order_acquire);
    if (ac && dc) {
        auto aOpt = ac->load();
        auto dOpt = dc->load();
        // One-time diagnostic: confirm both anchors are ticking the
        // first time we successfully compute the analytic lookahead.
        static bool firstSyncLogged = false;
        if (!firstSyncLogged && aOpt && dOpt && aOpt->sampleRate > 0.0) {
            firstSyncLogged = true;
            qInfo().noquote() << QString::asprintf(
                "[avsync] first analytic sync: aBuf=%llu dCb=%llu "
                "lat=%.2fms refreshIv=%.3fms",
                (unsigned long long)aOpt->version,
                (unsigned long long)dOpt->version,
                aOpt->outputLatencySec * 1000.0,
                dOpt->refreshIntervalSec * 1000.0);
        }
        if (aOpt && dOpt && aOpt->sampleRate > 0.0) {
            // Two anchors at host-time domain (seconds since boot):
            //   t_now    = wall-clock now
            //   T_pixel  = scanout instant the next frame will appear
            //   L_out    = device + safety + stream latency in seconds
            //
            // The AU's S_a anchor maps host time H_a to sample-index S_a;
            // by linearity, the sample heard at T_pixel is
            //   S_at_pixel = S_a + (T_pixel − t(H_a) − L_out) · SR.
            // The "lead" relative to the AU's current consumption point
            // is therefore
            //   leadSamples = S_at_pixel − S_now_being_consumed
            //               = (T_pixel − t_now − L_out) · SR
            // — the H_a / S_a anchor cancels out because the AU has been
            // consuming at SR samples/second since H_a. We still validate
            // that the anchor exists (proves the AU is alive) and read
            // SR + L_out from it.
            const double now    = concerto::sync::AudioClock::nowSeconds();
            const double deltaS = dOpt->targetPresentationSec - now
                                   - aOpt->outputLatencySec;
            // leadFrames is the number of audio frames the pipe playhead
            // is behind the sample-that-will-be-heard-at-T_pixel.
            // bytesPerFrame = bytesPerSample × channelCount.
            const double leadFrames = deltaS * aOpt->sampleRate;
            offsetBytes = qint64(
                std::llround(leadFrames * double(bytesPerFrame)));
        }
    }
#endif

    if (offsetBytes < 0) {
        // Fallback path: legacy static lookahead.
        offsetBytes = qint64(m_lookaheadStaticMs) * bpms;
    }
    // Calibration bias (user-tunable, signed) — same units regardless
    // of which path produced the base offset.
    offsetBytes += qint64(m_syncCalibrationMs) * bpms;

    // Account for Qt's QAudioSink internal ringbuffer dwell. PcmPipe's
    // readPos advances when Qt PULLS bytes from us, but those bytes
    // then sit in QAudioSink's internal buffer (set in init(), default
    // 500 ms) before reaching the AURenderCallback. processedUSecs()
    // counts bytes Qt has actually handed off to CoreAudio. The gap
    // (readPos - currentPipeByte) is the sink-buffer dwell — subtract
    // it so the FFT taps the byte that's audible NOW, not the byte
    // that's audible 500 ms from now.
    if (m_sink && m_sinkStarted) {
        const qint64 readPos      = m_pipe->readPos();
        const qint64 audibleByte  = currentPipeByte();
        const qint64 sinkDwellB   = readPos - audibleByte;
        if (sinkDwellB > 0) offsetBytes -= sinkDwellB;
    }

    // Clamp to a sane range. Negative offsets are now legal (peek
    // supports them) — they let us tap samples already pulled from
    // the pipe but still in Qt's sink-buffer. Bound them by ±2 s so
    // an off-the-rails formula can't read into the void.
    const qint64 maxOffset = qint64(2000) * bpms;
    if (offsetBytes >  maxOffset) offsetBytes =  maxOffset;
    if (offsetBytes < -maxOffset) offsetBytes = -maxOffset;

    // Frame-align so the FFT's stereo deinterleave never starts mid-
    // frame. C++ integer division truncates toward zero, so for
    // negative offsets we need floor-division to align downward.
    if (offsetBytes >= 0) {
        offsetBytes = (offsetBytes / bytesPerFrame) * bytesPerFrame;
    } else {
        offsetBytes = -(((-offsetBytes + bytesPerFrame - 1) / bytesPerFrame)
                        * bytesPerFrame);
    }

    const qint64 cap = m_lookaheadScratch.size();
    const qint64 n =
        m_pipe->peek(offsetBytes, m_lookaheadScratch.data(), cap);
    if (n <= 0) {
        // Pipe doesn't have enough data ahead (very end of track, or
        // streaming preview starving briefly). The FFT ring keeps its
        // contents and the bands decay through normal release smoothing.
        return;
    }

    if (m_fft)      m_fft     ->pushPcm(m_lookaheadScratch.constData(), n, m_fmt);
    if (m_features) m_features->pushPcm(m_lookaheadScratch.constData(), n, m_fmt);
}


void AudioWorker::setSyncCalibrationMs(int ms)
{
    // Queued setter — runs on the audio thread. Clamp to ±300 ms.
    if (ms < -300) ms = -300;
    if (ms >  300) ms =  300;
    m_syncCalibrationMs = ms;
}

#ifdef Q_OS_MACOS
void AudioWorker::setClocksForLookahead(concerto::sync::AudioClock*   audio,
                                        concerto::sync::DisplayClock* display)
{
    // Called from main thread. Atomic store with release so the audio-
    // thread tick's acquire load sees consistent pointers.
    m_audioClockAtom.store(audio,    std::memory_order_release);
    m_displayClockAtom.store(display, std::memory_order_release);
}
#endif
