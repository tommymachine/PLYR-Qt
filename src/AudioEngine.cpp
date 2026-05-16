#include "AudioEngine.h"
#include "AudioWorker.h"

#ifdef Q_OS_MACOS
#include "AudioClock_macOS.h"
#include "DisplayClock_macOS.h"
#endif

#include <QDebug>
#include <QMetaObject>
#include <QMutexLocker>
#include <QQuickWindow>
#include <QThread>
#include <QTimer>
#include <QtMath>
#include <algorithm>
#include <cmath>


AudioEngine::AudioEngine(QObject* parent)
    : QObject(parent)
{
    m_thread = new QThread(this);
    m_thread->setObjectName("ConcertoAudio");

    // Worker is unparented because it must moveToThread — Qt forbids
    // moving a QObject across threads if it has a parent on a different
    // thread. We manually delete it after the thread stops.
    m_worker = new AudioWorker();
    m_worker->moveToThread(m_thread);

    // Create the worker's Qt objects (sink, decoder, pipe, timer, EQ)
    // AFTER moveToThread, so they get the audio thread's affinity.
    connect(m_thread, &QThread::started, m_worker, &AudioWorker::init);

    // --- Worker → Engine: state updates (auto-queued, main thread). ---
    connect(m_worker, &AudioWorker::engineReady,           this, &AudioEngine::onWorkerEngineReady);
    connect(m_worker, &AudioWorker::workerSourceChanged,   this, &AudioEngine::onWorkerSource);
    connect(m_worker, &AudioWorker::workerPositionChanged, this, &AudioEngine::onWorkerPosition);
    connect(m_worker, &AudioWorker::workerDurationChanged, this, &AudioEngine::onWorkerDuration);
    connect(m_worker, &AudioWorker::workerPlayingChanged,  this, &AudioEngine::onWorkerPlaying);
    connect(m_worker, &AudioWorker::workerVolumeChanged,   this, &AudioEngine::onWorkerVolume);
    connect(m_worker, &AudioWorker::workerTrackEnded,      this, &AudioEngine::onWorkerTrackEnded);
    connect(m_worker, &AudioWorker::workerActiveTrackChanged, this, &AudioEngine::onWorkerActiveTrack);
    connect(m_worker, &AudioWorker::workerReadyForNextTrack,  this, &AudioEngine::onWorkerReadyForNextTrack);
    connect(m_worker, &AudioWorker::workerOutputDeviceChanged, this, &AudioEngine::onWorkerOutputDeviceChanged);

    // --- Engine → Worker: commands (auto-queued, audio thread). ---
    connect(this, &AudioEngine::requestPlay,      m_worker, &AudioWorker::play);
    connect(this, &AudioEngine::requestPause,     m_worker, &AudioWorker::pause);
    connect(this, &AudioEngine::requestStop,      m_worker, &AudioWorker::stop);
    connect(this, &AudioEngine::requestSeek,      m_worker, &AudioWorker::seek);
    connect(this, &AudioEngine::requestSetSource, m_worker, &AudioWorker::setSource);
    connect(this, &AudioEngine::requestSetVolume, m_worker, &AudioWorker::setVolume);
    connect(this, &AudioEngine::requestSetSyncCalibrationMs,
            m_worker, &AudioWorker::setSyncCalibrationMs);
    connect(this, &AudioEngine::requestPlayAt,    m_worker, &AudioWorker::playAt);
    connect(this, &AudioEngine::requestEnqueueAt, m_worker, &AudioWorker::enqueueAt);

    // Push the initial calibration bias (whatever the cache was seeded
    // with — main.cpp may have called setSyncCalibrationMs() to restore
    // a user preference before m_thread->start() spins the audio thread).
    // Auto-queued: the worker will pick this up after init() runs.
    emit requestSetSyncCalibrationMs(m_syncCalibrationMsCache.load());

#ifdef Q_OS_MACOS
    // Build the clocks here on the main thread; they're attached later.
    // AudioClock::attach() is deferred until the worker has finished its
    // first sink->start() (engineReady), so the AUHAL output is alive on
    // the same default device. DisplayClock::attach() runs from
    // attachToWindow() once QML has built its window.
    m_audioClock   = std::make_unique<plyr::sync::AudioClock>();
    m_displayClock = std::make_unique<plyr::sync::DisplayClock>();
#endif

    connect(this, &AudioEngine::requestStartPreviewStream,
            m_worker, &AudioWorker::startPreviewStream);
    connect(this, &AudioEngine::requestPushPreviewPcm,
            m_worker, &AudioWorker::pushPreviewPcm);
    connect(this, &AudioEngine::requestStopPreviewStream,
            m_worker, &AudioWorker::stopPreviewStream);

    m_thread->start();
}


AudioEngine::~AudioEngine()
{
    if (m_thread && m_thread->isRunning()) {
        // Tear down worker's Qt objects on the worker's thread, then
        // stop the event loop. BlockingQueued waits for shutdown() to
        // return before we continue.
        QMetaObject::invokeMethod(m_worker, "shutdown",
                                  Qt::BlockingQueuedConnection);
        m_thread->quit();
        m_thread->wait();
    }
    // Worker's thread is stopped; safe to delete from main. Qt may warn
    // about cross-thread destruction but there's no pending work.
    delete m_worker;
    m_worker = nullptr;
    // m_thread is a child of this; Qt deletes it automatically.
}


void AudioEngine::setFftProcessor(FftProcessor* fft)
{
    // Safe: the worker exists (constructed in our ctor) and hasn't touched
    // its m_fft yet (init() runs when the thread starts and connections
    // deliver events). A single pointer store pre-thread-start is fine.
    if (m_worker) m_worker->setFftProcessor(fft);
}


void AudioEngine::setAudioFeatures(AudioFeatures* af)
{
    // Same lifetime story as setFftProcessor. Both must be called before
    // app.exec() spins the event loops, which is what main.cpp does.
    if (m_worker) m_worker->setAudioFeatures(af);
}


// ---- Property reads ------------------------------------------------------

QUrl AudioEngine::source() const
{
    QMutexLocker lk(&m_sourceMutex);
    return m_sourceCache;
}


// ---- Setters & invokables (fire-and-forget to worker) -------------------

void AudioEngine::setSource(const QUrl& u)
{
    emit requestSetSource(u);
}

void AudioEngine::setVolume(float v)
{
    if (std::abs(m_volumeCache.load() - v) < 1e-6f) return;
    m_volumeCache.store(v);
    emit volumeChanged();            // QML reacts to cache change immediately
    emit requestSetVolume(v);        // worker applies to sink on its thread
}

void AudioEngine::setSyncCalibrationMs(int ms)
{
    if (ms < -300) ms = -300;
    if (ms >  300) ms =  300;
    if (m_syncCalibrationMsCache.load() == ms) return;
    m_syncCalibrationMsCache.store(ms);
    emit syncCalibrationMsChanged();
    emit requestSetSyncCalibrationMs(ms);
}

void AudioEngine::attachToWindow(QObject* window)
{
#ifdef Q_OS_MACOS
    auto* qw = qobject_cast<QQuickWindow*>(window);
    if (!qw) {
        qWarning() << "[AudioEngine] attachToWindow: not a QQuickWindow";
        return;
    }
    if (!m_displayClock) return;

    auto doAttach = [this, qw]() {
        // Bail if the window has gone away between scheduling and now.
        if (!qw) return;
        if (m_displayClock && m_displayClock->attach(qw)) {
            // Pass the clocks down to the worker now that both ends are
            // alive. The worker's tick path is allocation-free; pointers
            // are stored by atomic store on the main thread, loaded by
            // atomic acquire on the audio thread.
            if (m_worker) {
                m_worker->setClocksForLookahead(m_audioClock.get(),
                                                m_displayClock.get());
            }
        }
    };

    // The CAMetalLayer behind QQuickWindow's NSView doesn't exist until
    // Qt's RHI has run through its first frame. sceneGraphInitialized
    // fires on the render thread, but it guarantees the RHI is up — at
    // which point queueing back to the GUI thread lets us safely poke
    // the NSView. If it has already fired (re-attach case), schedule
    // directly.
    if (qw->isSceneGraphInitialized()) {
        QMetaObject::invokeMethod(this, doAttach, Qt::QueuedConnection);
    } else {
        // Single-shot connect: disconnects itself after firing once.
        auto* conn = new QMetaObject::Connection;
        *conn = QObject::connect(
            qw, &QQuickWindow::sceneGraphInitialized,
            this,
            [this, qw, doAttach, conn]() {
                QObject::disconnect(*conn);
                delete conn;
                QMetaObject::invokeMethod(this, doAttach, Qt::QueuedConnection);
            });
    }
#else
    Q_UNUSED(window);
#endif
}

void AudioEngine::play()                                      { emit requestPlay(); }
void AudioEngine::pause()                                     { emit requestPause(); }
void AudioEngine::stop()                                      { emit requestStop(); }
void AudioEngine::seek(qint64 ms)                             { emit requestSeek(ms); }
void AudioEngine::playAt(int idx, const QUrl& url)            { emit requestPlayAt(idx, url); }
void AudioEngine::enqueueAt(int idx, const QUrl& url)         { emit requestEnqueueAt(idx, url); }

void AudioEngine::startPreviewStream(qint64 totalDurationMs, qint64 startOffsetMs)
{ emit requestStartPreviewStream(totalDurationMs, startOffsetMs); }
void AudioEngine::pushPreviewPcm(const QByteArray& bytes)     { emit requestPushPreviewPcm(bytes); }
void AudioEngine::stopPreviewStream()                         { emit requestStopPreviewStream(); }

void AudioEngine::playTestSinePulses(int durationSec)
{
    // A/V-sync calibration tone: a 30-second click track of 1 Hz
    // sine pulses, fed through the streaming-preview path so no file
    // lands on disk and no playlist state is mutated.
    //
    // Each pulse is a 50 ms Hann-windowed 1 kHz sine at ~40% scale.
    // 1 kHz is high enough to feel like a click; the Hann envelope
    // eliminates the start/end discontinuity that would otherwise add
    // spectral hash. Pulses repeat exactly every second — easy to
    // mentally lock to.
    constexpr int    kSampleRate   = 44100;
    constexpr int    kChannels     = 2;
    constexpr double kPulseHz      = 1000.0;
    constexpr double kPulseDurSec  = 0.05;
    constexpr double kPulseAmp     = 0.40;

    durationSec = std::clamp(durationSec, 5, 120);
    const qint64 totalFrames = qint64(durationSec) * kSampleRate;

    QByteArray pcm;
    pcm.resize(int(totalFrames) * kChannels * int(sizeof(qint16)));
    qint16* out = reinterpret_cast<qint16*>(pcm.data());

    for (qint64 i = 0; i < totalFrames; ++i) {
        const double t = double(i) / double(kSampleRate);
        const double pulsePhase = t - std::floor(t);  // 0..1 in each second
        double sample = 0.0;
        if (pulsePhase < kPulseDurSec) {
            // Hann window over the pulse, sin carrier underneath.
            const double env = 0.5 * (1.0 -
                std::cos(2.0 * M_PI * pulsePhase / kPulseDurSec));
            sample = env
                   * std::sin(2.0 * M_PI * kPulseHz * t)
                   * kPulseAmp;
        }
        const qint16 s16 = qint16(std::clamp(sample, -1.0, 1.0) * 32767.0);
        out[i * 2 + 0] = s16;
        out[i * 2 + 1] = s16;
    }

    // Kick the engine into pull-from-pipe mode (totalDurationMs drives
    // the seek slider's range), then push the whole buffer in one go.
    // QAudioSink will consume it at sample rate.
    startPreviewStream(qint64(durationSec) * 1000, 0);
    pushPreviewPcm(pcm);

    // Auto-stop just after the test finishes so the engine doesn't sit
    // in preview mode forever waiting for more bytes.
    QTimer::singleShot(durationSec * 1000 + 300, this,
                       [this]() { stopPreviewStream(); });
}


// ---- Worker → Engine slots ----------------------------------------------

void AudioEngine::onWorkerEngineReady()
{
    m_eqCache.store(m_worker ? m_worker->eqEngine() : nullptr);
#ifdef Q_OS_MACOS
    // Audio clock can attach now — QAudioSink has been constructed and
    // (after the first play) is running on the system default output
    // device. Even if the user hasn't pressed play yet, the AUHAL we
    // stand up here binds to the same device and its render-notify
    // callback starts ticking immediately (silent buffers).
    if (m_audioClock && !m_audioClock->attach()) {
        qWarning() << "[AudioEngine] AudioClock attach failed; "
                      "falling back to static lookahead.";
    }
    if (m_audioClock) {
        m_outputIsBluetoothCache.store(m_audioClock->isBluetooth());
        emit outputIsBluetoothChanged();
    }
    if (m_worker) {
        m_worker->setClocksForLookahead(m_audioClock.get(),
                                        m_displayClock.get());
    }
#endif
    emit engineReady();
}

void AudioEngine::onWorkerSource(QUrl url)
{
    {
        QMutexLocker lk(&m_sourceMutex);
        m_sourceCache = url;
    }
    emit sourceChanged();
}

void AudioEngine::onWorkerPosition(qint64 ms)
{
    m_positionCache.store(ms);
    emit positionChanged();
}

void AudioEngine::onWorkerDuration(qint64 ms)
{
    m_durationCache.store(ms);
    emit durationChanged();
}

void AudioEngine::onWorkerPlaying(bool p)
{
    m_playingCache.store(p);
    emit playingChanged();
}

void AudioEngine::onWorkerVolume(float v)
{
    // Cache may already match (we updated it in setVolume). Still emit in
    // case the worker adjusted it for some reason (clamping, etc.).
    if (std::abs(m_volumeCache.load() - v) > 1e-6f) {
        m_volumeCache.store(v);
        emit volumeChanged();
    }
}

void AudioEngine::onWorkerTrackEnded()
{
    emit trackEnded();
}

void AudioEngine::onWorkerActiveTrack(int idx)
{
    m_activeIndexCache.store(idx);
    emit activeTrackChanged(idx);
}

void AudioEngine::onWorkerReadyForNextTrack()
{
    emit readyForNextTrack();
}

void AudioEngine::onWorkerOutputDeviceChanged()
{
#ifdef Q_OS_MACOS
    if (!m_audioClock) return;
    // Tell the worker to drop its pointer first; detach + re-attach the
    // probe AUHAL bound to the new default device, then hand the
    // (possibly-still-the-same-instance) pointer back. The atomic store
    // in setClocksForLookahead synchronizes the audio thread's reader.
    if (m_worker) {
        m_worker->setClocksForLookahead(nullptr, m_displayClock.get());
    }
    m_audioClock->detach();
    if (!m_audioClock->attach()) {
        qWarning() << "[AudioEngine] AudioClock re-attach failed after "
                      "output device change; falling back to static "
                      "lookahead.";
        return;
    }
    m_outputIsBluetoothCache.store(m_audioClock->isBluetooth());
    emit outputIsBluetoothChanged();
    if (m_worker) {
        m_worker->setClocksForLookahead(m_audioClock.get(),
                                        m_displayClock.get());
    }
#endif
}
