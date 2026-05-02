#include "AudioEngine.h"
#include "AudioWorker.h"

#include <QMutexLocker>
#include <QThread>


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

    // --- Engine → Worker: commands (auto-queued, audio thread). ---
    connect(this, &AudioEngine::requestPlay,      m_worker, &AudioWorker::play);
    connect(this, &AudioEngine::requestPause,     m_worker, &AudioWorker::pause);
    connect(this, &AudioEngine::requestStop,      m_worker, &AudioWorker::stop);
    connect(this, &AudioEngine::requestSeek,      m_worker, &AudioWorker::seek);
    connect(this, &AudioEngine::requestSetSource, m_worker, &AudioWorker::setSource);
    connect(this, &AudioEngine::requestSetVolume, m_worker, &AudioWorker::setVolume);
    connect(this, &AudioEngine::requestPlayAt,    m_worker, &AudioWorker::playAt);
    connect(this, &AudioEngine::requestEnqueueAt, m_worker, &AudioWorker::enqueueAt);

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

void AudioEngine::play()                                      { emit requestPlay(); }
void AudioEngine::pause()                                     { emit requestPause(); }
void AudioEngine::stop()                                      { emit requestStop(); }
void AudioEngine::seek(qint64 ms)                             { emit requestSeek(ms); }
void AudioEngine::playAt(int idx, const QUrl& url)            { emit requestPlayAt(idx, url); }
void AudioEngine::enqueueAt(int idx, const QUrl& url)         { emit requestEnqueueAt(idx, url); }


// ---- Worker → Engine slots ----------------------------------------------

void AudioEngine::onWorkerEngineReady()
{
    m_eqCache.store(m_worker ? m_worker->eqEngine() : nullptr);
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
