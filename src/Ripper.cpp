// Ripper — facade over RipWorker. See Ripper.h for the long-form intent.

#include "Ripper.h"

#include "CdShield.h"
#include "RipBatchStore.h"
#include "RipWorker.h"

#include <QDir>
#include <QFileInfo>
#include <QThread>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>

namespace plyr::cd {

namespace {

void setTrackStatusIn(QVariantList& tracks, int oneBased, const QString& status) {
    const int i = oneBased - 1;
    if (i < 0 || i >= tracks.size()) return;
    auto m = tracks[i].toMap();
    m["status"] = status;
    tracks[i] = m;
}

QVariantMap batchToVariant(const RipBatch& b) {
    QVariantMap m;
    m["id"]              = b.id;
    m["albumTitle"]      = b.albumTitle;
    m["artist"]          = b.artist;
    m["releaseGroupId"]  = b.releaseGroupId;
    m["totalDiscs"]      = b.totalDiscs;
    int done = 0;
    QVariantList discs;
    for (const auto& d : b.discs) {
        QVariantMap dm;
        dm["position"]   = d.position;
        dm["status"]     = d.status;
        dm["savedPath"]  = d.savedPath;
        discs.append(dm);
        // Resume-picker "X of Y done" — counts user-resolved discs
        // (anything they're not coming back for: ripped + skipped).
        if (d.status == QStringLiteral("done")
            || d.status == QStringLiteral("skipped"))
        {
            ++done;
        }
    }
    m["doneCount"]   = done;
    m["discs"]       = discs;
    m["updatedAt"]   = b.updatedAt;
    return m;
}

} // namespace

Ripper::Ripper(CdShield* shield, QObject* parent)
    : QObject(parent)
    , m_shield(shield)
{
    // Worker on a dedicated thread. Queued connections cross the
    // boundary; the worker drives a slow read loop (~10 min per disc),
    // so a separate thread keeps the GUI responsive.
    m_thread = new QThread(this);
    m_worker = new RipWorker();
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);

    connect(m_worker, &RipWorker::discIdentified,
            this, &Ripper::onDiscIdentified);
    connect(m_worker, &RipWorker::mbResolved,
            this, &Ripper::onMbResolved);
    connect(m_worker, &RipWorker::mbUnavailable,
            this, &Ripper::onMbUnavailable);
    connect(m_worker, &RipWorker::readStarted,
            this, &Ripper::onReadStarted);
    connect(m_worker, &RipWorker::readProgress,
            this, &Ripper::onReadProgress);
    connect(m_worker, &RipWorker::zeroFilled,
            this, &Ripper::onZeroFilled);
    connect(m_worker, &RipWorker::warning,
            this, [this](const QString& m) { emit warning(m); });
    connect(m_worker, &RipWorker::previewStreamStart,
            this, [this](qint64 ms) { emit previewStreamStart(ms); });
    connect(m_worker, &RipWorker::previewPcm,
            this, [this](QByteArray b) { emit previewPcm(std::move(b)); });
    connect(m_worker, &RipWorker::previewStreamStop,
            this, [this]() { emit previewStreamStop(); });
    connect(m_worker, &RipWorker::encodingStarted,
            this, &Ripper::onEncodingStarted);
    connect(m_worker, &RipWorker::encodedTrack,
            this, &Ripper::onEncodedTrack);
    connect(m_worker, &RipWorker::encodingComplete,
            this, &Ripper::onEncodingComplete);
    connect(m_worker, &RipWorker::verifyingStarted,
            this, &Ripper::onVerifyingStarted);
    connect(m_worker, &RipWorker::verifyTrackResult,
            this, &Ripper::onVerifyTrackResult);
    connect(m_worker, &RipWorker::verifyComplete,
            this, &Ripper::onVerifyComplete);
    connect(m_worker, &RipWorker::readyToSave,
            this, &Ripper::onReadyToSave);
    connect(m_worker, &RipWorker::discSaved,
            this, &Ripper::onWorkerDiscSaved);
    connect(m_worker, &RipWorker::ripCancelled,
            this, &Ripper::onRipCancelled);
    connect(m_worker, &RipWorker::failed,
            this, &Ripper::onFailed);

    m_thread->start();
}

Ripper::~Ripper() {
    if (m_worker) m_worker->doCancel();
    m_thread->quit();
    m_thread->wait();
    // m_worker deletes via QThread::finished → deleteLater connection.
}

// ---- State helpers ---------------------------------------------------

void Ripper::setState(State s) {
    if (m_state == s) return;
    m_state = s;
    emit stateChanged();
}

void Ripper::setTrackStatus(int oneBased, const QString& status) {
    setTrackStatusIn(m_tracks, oneBased, status);
}

void Ripper::resetDiscState() {
    m_discPresent          = false;
    m_driveName.clear();
    m_driveOffsetSamples   = 0;
    m_driveOffsetFromDb    = false;
    m_trackCount           = 0;
    m_totalDurationSec     = 0;
    m_hasMatch             = false;
    m_albumTitle.clear();
    m_artist.clear();
    m_date.clear();
    m_discPosition         = m_inBatch ? m_batchExpectedDisc : 1;
    m_discTotalCount       = m_inBatch ? m_batchTotalCount   : 1;
    m_mbDiscId.clear();
    m_accurateRipId.clear();
    m_tracks.clear();
    m_readProgress = m_encodeProgress = m_verifyProgress = 0.0;
    m_currentLba = 0;
    m_currentSpeedSecPerSec = 0.0;
    m_currentMultiplier = 0.0;
    m_etaSec = 0;
    m_currentTrackNumber = 0;
    m_verifySummary.clear();
    m_zeroFilledRanges.clear();
    m_currentBsdName.clear();
    m_currentTempDir.clear();
    m_suggestedFolderName.clear();
    m_currentReleaseGroupId.clear();
    // Keep m_currentDiscPlaybackPath intact across disc-boundary resets
    // — the previous disc's saved tracks are still playing under the
    // rip view, and the QML uses the path to decide whether
    // `playlist.currentTrackNumber` belongs to this disc's track list.
    // It gets cleared only on endSession.
}

// ---- Public slots ----------------------------------------------------

void Ripper::startSession(const QString& resumeBatchId) {
    if (m_state != State::Idle) return;

    resetDiscState();
    m_errorMessage.clear();

    if (!resumeBatchId.isEmpty()) {
        for (const auto& b : RipBatchStore::listResumable()) {
            if (b.id == resumeBatchId) {
                m_inBatch         = true;
                m_batch           = b;
                m_batchAlbumTitle = b.albumTitle;
                m_batchTotalCount = b.totalDiscs;
                int resolved = 0, nextPending = 0;
                for (const auto& d : b.discs) {
                    if (d.status == QStringLiteral("done")
                        || d.status == QStringLiteral("skipped"))
                    {
                        ++resolved;
                    } else if (nextPending == 0) {
                        nextPending = d.position;
                    }
                }
                m_batchDoneCount    = resolved;
                m_batchExpectedDisc = nextPending > 0 ? nextPending : 1;
                emit batchChanged();
                break;
            }
        }
    }

    setState(State::WaitingForDisc);
    emit discChanged();
    emit tracksChanged();
    emit progressChanged();
    emit zeroFilledRangesChanged();

    // Register disc listeners. Setting the appeared listener also
    // synchronously replays any disc currently in a drive — so if the
    // user already has the next disc loaded, identification kicks off
    // immediately without further user action.
    if (m_shield) {
        m_shield->setOnDiscAppeared(
            [this](const std::string& bsd) {
                onCdShieldDiscAppeared(bsd);
            });
        m_shield->setOnDiscDisappeared(
            [this](const std::string& bsd) {
                onCdShieldDiscDisappeared(bsd);
            });
    }
}

void Ripper::endSession() {
    // Drop disc listeners — we don't care about insertions outside an
    // active session.
    if (m_shield) {
        m_shield->setOnDiscAppeared({});
        m_shield->setOnDiscDisappeared({});
    }

    if (m_state == State::Reading
        || m_state == State::Encoding
        || m_state == State::Verifying)
    {
        // Mid-rip: signal cancel; the worker emits ripCancelled which
        // resets us. (The popup-close path in QML already calls
        // stopRip first; this is the safety net.)
        m_worker->doCancel();
    }
    if (m_state == State::SavePending) {
        // Same as discardStagedRip but explicit end of session.
        QMetaObject::invokeMethod(m_worker, "discardStagedRip",
                                  Qt::QueuedConnection);
    }

    m_inBatch = false;
    m_batch   = {};
    emit batchChanged();

    resetDiscState();
    if (!m_currentDiscPlaybackPath.isEmpty()) {
        m_currentDiscPlaybackPath.clear();
        emit currentDiscPlaybackPathChanged();
    }
    emit discChanged();
    emit tracksChanged();
    emit progressChanged();
    emit zeroFilledRangesChanged();
    setState(State::Idle);
}

void Ripper::stopRip(bool deleteBatch) {
    if (m_state == State::Idle) {
        if (deleteBatch && !m_batch.id.isEmpty()) {
            RipBatchStore::remove(m_batch.id);
            m_inBatch = false;
            m_batch   = {};
            emit batchChanged();
        }
        return;
    }

    setState(State::Cancelling);
    m_worker->doCancel();

    if (m_state == State::SavePending) {
        QMetaObject::invokeMethod(m_worker, "discardStagedRip",
                                  Qt::QueuedConnection);
    }

    if (deleteBatch && !m_batch.id.isEmpty()) {
        RipBatchStore::remove(m_batch.id);
        m_inBatch = false;
        m_batch   = {};
        emit batchChanged();
    }
    // The worker emits ripCancelled which transitions us back to Idle
    // via onRipCancelled. The popup's close handler is responsible for
    // calling endSession after the dialog dismisses.
}

void Ripper::saveTo(const QUrl& parentFolder, const QString& folderNameOverride) {
    if (m_state != State::SavePending) return;

    QString name = folderNameOverride.isEmpty()
        ? m_suggestedFolderName
        : folderNameOverride;
    if (name.isEmpty()) name = QStringLiteral("Untitled");

    setState(State::Saving);
    QMetaObject::invokeMethod(m_worker, "doSave", Qt::QueuedConnection,
                              Q_ARG(QString, parentFolder.toLocalFile()),
                              Q_ARG(QString, name));
}

void Ripper::discardStagedRip() {
    if (m_state != State::SavePending) return;
    QMetaObject::invokeMethod(m_worker, "discardStagedRip",
                              Qt::QueuedConnection);
    // Treat as a soft cancel — drop back to WaitingForDisc if we're in
    // a batch (so the disc can be re-ripped later), otherwise Idle.
    if (m_inBatch) {
        resetDiscState();
        emit discChanged();
        emit tracksChanged();
        emit progressChanged();
        emit zeroFilledRangesChanged();
        setState(State::WaitingForDisc);
    } else {
        endSession();
    }
}

void Ripper::ejectDisc() {
    // The shield holds the DA claim; the actual eject is a worker-side
    // operation. Since we don't currently have an idle "eject only"
    // pathway on the worker, this is a stub for now — the user can use
    // the physical eject button while the rip is between discs.
    // (cdrip_cli has --eject; that path could be hoisted into the
    // worker as a separate slot in v1.1.)
    emit warning(tr("Use the drive's physical eject button to swap the disc."));
}

void Ripper::skipCurrentDisc() {
    if (!m_inBatch) return;
    if (m_state != State::WaitingForDisc) return;

    bool changed = false;
    for (auto& d : m_batch.discs) {
        if (d.position == m_batchExpectedDisc) {
            if (d.status != QStringLiteral("done")) {
                d.status = QStringLiteral("skipped");
                changed = true;
            }
            break;
        }
    }
    if (!changed) return;

    RipBatchStore::save(m_batch);

    // Bump expected to the next pending. done + skipped both count as
    // "not pending" so the user only sees discs they could still rip.
    int nextPending = 0;
    int doneOrSkipped = 0;
    for (const auto& d : m_batch.discs) {
        const auto s = d.status;
        if (s == QStringLiteral("done")) {
            ++doneOrSkipped;
        } else if (s == QStringLiteral("skipped")) {
            ++doneOrSkipped;
        } else if (nextPending == 0) {
            nextPending = d.position;
        }
    }
    m_batchDoneCount    = doneOrSkipped;
    m_batchExpectedDisc = nextPending > 0 ? nextPending : doneOrSkipped;
    emit batchChanged();

    // If we've exhausted the batch, close the session — nothing left
    // to rip in this set.
    if (nextPending == 0) {
        endSession();
    }
}

void Ripper::deleteResumableBatch(const QString& batchId) {
    RipBatchStore::remove(batchId);
    refreshResumableBatches();
}

void Ripper::refreshResumableBatches() {
    QVariantList out;
    for (const auto& b : RipBatchStore::listResumable()) {
        out.append(batchToVariant(b));
    }
    m_resumableBatches = std::move(out);
    emit resumableBatchesChanged();
}

// ---- CdShield wiring -------------------------------------------------

void Ripper::onCdShieldDiscAppeared(const std::string& bsdName) {
    if (m_state != State::WaitingForDisc) return;
    if (bsdName.empty()) return;

    setState(State::Identifying);
    m_currentBsdName = QString::fromStdString(bsdName);

    // For a resumable batch with a known parent folder, hand the
    // worker that parent so it can write FLACs straight into the
    // user's library rather than the AppSupport temp dir. The
    // playlist sees the disc materialize where it'll live forever.
    const QString preferredParent = (m_inBatch && !m_batch.parentFolder.isEmpty())
        ? m_batch.parentFolder
        : QString();
    QMetaObject::invokeMethod(m_worker, "doRip", Qt::QueuedConnection,
                              Q_ARG(QString, m_currentBsdName),
                              Q_ARG(QString, preferredParent));
}

void Ripper::onCdShieldDiscDisappeared(const std::string& /*bsdName*/) {
    if (m_state == State::WaitingForDisc) {
        // Track the "no disc in drive" state explicitly so QML can show
        // the right prompt; the disc info itself stays cleared.
        if (m_discPresent) {
            m_discPresent = false;
            emit discChanged();
        }
    }
    // During Identifying/Reading/etc the worker is already holding the
    // disc data (or about to fail with EIO if the user yanked the disc
    // mid-read); we let the worker surface those errors.
}

// ---- Worker → Ripper slots ------------------------------------------

void Ripper::onDiscIdentified(QVariantMap info) {
    m_discPresent         = true;
    m_driveName           = info.value("driveName").toString();
    m_driveOffsetSamples  = info.value("driveOffsetSamples").toInt();
    m_driveOffsetFromDb   = info.value("driveOffsetFromDb").toBool();
    m_trackCount          = info.value("trackCount").toInt();
    m_totalDurationSec    = info.value("totalDurationSec").toInt();
    m_tracks              = info.value("tracks").toList();
    m_mbDiscId            = info.value("mbDiscId").toString();
    m_accurateRipId       = info.value("accurateRipId").toString();

    // No MB match yet (mbResolved fires after this), so position/title
    // stay at their defaults.
    emit discChanged();
    emit tracksChanged();
}

void Ripper::onMbResolved(QVariantMap match) {
    m_hasMatch        = match.value("hasMatch", true).toBool();
    m_albumTitle      = match.value("albumTitle").toString();
    m_artist          = match.value("artist").toString();
    m_date            = match.value("date").toString();
    m_discPosition    = match.value("discPosition", 1).toInt();
    m_discTotalCount  = match.value("discTotalCount", 1).toInt();
    m_currentReleaseGroupId = match.value("releaseGroupId").toString();
    m_tracks          = match.value("tracks").toList();

    // Look up an existing batch for this release-group. If we're not
    // already in a batch and this is a multi-disc release, create one
    // implicitly so the resume flow has something to find later.
    if (!m_inBatch
        && m_discTotalCount > 1
        && !m_currentReleaseGroupId.isEmpty())
    {
        const auto existing = RipBatchStore::lookupByReleaseGroup(
            m_currentReleaseGroupId);
        if (existing) {
            m_batch = *existing;
            m_inBatch = true;
        } else {
            m_batch.id = RipBatchStore::newBatchId();
            m_batch.albumTitle     = m_albumTitle;
            m_batch.artist         = m_artist;
            m_batch.releaseGroupId = m_currentReleaseGroupId;
            m_batch.totalDiscs     = m_discTotalCount;
            m_batch.discs.clear();
            for (int i = 1; i <= m_discTotalCount; ++i) {
                RipBatchDisc d;
                d.position = i;
                m_batch.discs.append(d);
            }
            m_inBatch = true;
            // Persist the new batch shell so resume works even if the
            // first disc rip is cancelled.
            RipBatchStore::save(m_batch);
        }
        m_batchAlbumTitle   = m_batch.albumTitle;
        m_batchTotalCount   = m_batch.totalDiscs;
        m_batchDoneCount    = 0;
        for (const auto& d : m_batch.discs) {
            if (d.status == QStringLiteral("done")
                || d.status == QStringLiteral("skipped"))
            {
                ++m_batchDoneCount;
            }
        }
        m_batchExpectedDisc = m_discPosition;
        emit batchChanged();
    } else if (m_inBatch
               && !m_currentReleaseGroupId.isEmpty()
               && m_batch.releaseGroupId == m_currentReleaseGroupId)
    {
        // Already in this batch — refresh the "expected disc" for the
        // pill.
        m_batchExpectedDisc = m_discPosition;
        emit batchChanged();
    }

    emit discChanged();
    emit tracksChanged();
}

void Ripper::onMbUnavailable() {
    m_hasMatch = false;
    emit discChanged();
}

void Ripper::onReadStarted() {
    setState(State::Reading);
}

void Ripper::onReadProgress(int currentLba, double secPerSec, double multiplier,
                            int etaSec, int currentTrackNumber,
                            double readFraction) {
    m_currentLba            = currentLba;
    m_currentSpeedSecPerSec = secPerSec;
    m_currentMultiplier     = multiplier;
    m_etaSec                = etaSec;
    m_readProgress          = readFraction;

    if (currentTrackNumber != m_currentTrackNumber) {
        // Light up the track as "reading"; mark previous as "read".
        if (m_currentTrackNumber > 0)
            setTrackStatus(m_currentTrackNumber, QStringLiteral("read"));
        m_currentTrackNumber = currentTrackNumber;
        if (currentTrackNumber > 0)
            setTrackStatus(currentTrackNumber, QStringLiteral("reading"));
        emit tracksChanged();
    }
    emit progressChanged();
}

void Ripper::onZeroFilled(QVariantMap range) {
    m_zeroFilledRanges.append(range);
    emit zeroFilledRangesChanged();
}

void Ripper::onEncodingStarted() {
    // Finalize any in-progress "reading" state.
    if (m_currentTrackNumber > 0)
        setTrackStatus(m_currentTrackNumber, QStringLiteral("read"));
    m_currentTrackNumber = 0;
    m_readProgress = 1.0;
    m_currentSpeedSecPerSec = 0.0;
    m_currentMultiplier     = 0.0;
    m_etaSec                = 0;
    emit progressChanged();
    emit tracksChanged();
    setState(State::Encoding);
}

void Ripper::onEncodedTrack(int trackNumber, QString flacPath) {
    setTrackStatus(trackNumber, QStringLiteral("encoded"));
    const int n = m_tracks.size();
    int encoded = 0;
    for (const auto& v : m_tracks) {
        const auto s = v.toMap().value("status").toString();
        if (s == QLatin1String("encoded")
            || s == QLatin1String("ok") || s == QLatin1String("warn")
            || s == QLatin1String("fail") || s == QLatin1String("unknown"))
        {
            ++encoded;
        }
    }
    m_encodeProgress = n > 0 ? double(encoded) / double(n) : 0.0;
    emit progressChanged();
    emit tracksChanged();

    // Streaming-preview hook: main.cpp listens and either opens the
    // temp dir (on track 1) or appends the new track to the playlist.
    // The temp dir isn't known to the Ripper yet (it's set later, when
    // readyToSave fires), so derive it from the track's parent.
    const QString tempDir = QFileInfo(flacPath).path();
    if (m_currentTempDir.isEmpty()) m_currentTempDir = tempDir;
    if (m_currentDiscPlaybackPath != tempDir) {
        m_currentDiscPlaybackPath = tempDir;
        emit currentDiscPlaybackPathChanged();
    }
    emit discTrackReady(tempDir, flacPath, trackNumber);
}

void Ripper::onEncodingComplete() {
    m_encodeProgress = 1.0;
    emit progressChanged();
}

void Ripper::onVerifyingStarted() {
    setState(State::Verifying);
}

void Ripper::onVerifyTrackResult(int trackNumber, QString status) {
    setTrackStatus(trackNumber, status);
    const int n = m_tracks.size();
    int verified = 0;
    for (const auto& v : m_tracks) {
        const QString s = v.toMap().value("status").toString();
        if (s == QLatin1String("ok") || s == QLatin1String("warn")
            || s == QLatin1String("fail") || s == QLatin1String("unknown"))
            ++verified;
    }
    m_verifyProgress = n > 0 ? double(verified) / double(n) : 0.0;
    emit progressChanged();
    emit tracksChanged();
}

void Ripper::onVerifyComplete(QString summary) {
    m_verifyProgress = 1.0;
    m_verifySummary  = std::move(summary);
    emit progressChanged();
}

void Ripper::onReadyToSave(QString tempDir, QString suggestedFolderName) {
    m_currentTempDir      = std::move(tempDir);
    m_suggestedFolderName = std::move(suggestedFolderName);

    // Inside-batch auto-save: if the batch already has a parent folder
    // (set when an earlier disc of this same release-group was saved)
    // AND the just-ripped disc matches that release-group, drop the
    // user-prompted picker and rename straight into place. The first
    // disc of a batch still hits SavePending because parent_folder is
    // unknown at that point.
    if (m_inBatch
        && !m_batch.parentFolder.isEmpty()
        && !m_currentReleaseGroupId.isEmpty()
        && m_batch.releaseGroupId == m_currentReleaseGroupId)
    {
        setState(State::Saving);
        QMetaObject::invokeMethod(m_worker, "doSave", Qt::QueuedConnection,
                                  Q_ARG(QString, m_batch.parentFolder),
                                  Q_ARG(QString, m_suggestedFolderName));
        return;
    }

    setState(State::SavePending);
}

void Ripper::onWorkerDiscSaved(QString fromTempDir, QString finalPath) {
    setState(State::Saving);

    // The streaming playback was rooted at fromTempDir; main.cpp's
    // playlist will remapFolder() it to finalPath. Reflect that on our
    // side so the track-list "playing" highlight follows the disc into
    // its final home.
    if (m_currentDiscPlaybackPath == fromTempDir) {
        m_currentDiscPlaybackPath = finalPath;
        emit currentDiscPlaybackPathChanged();
    }

    // Update batch state: this disc is done. Only if the disc we just
    // saved actually belongs to the current batch (release-group match).
    // Out-of-set discs are saved alongside but don't pollute the batch.
    const bool inThisBatch = m_inBatch
        && !m_currentReleaseGroupId.isEmpty()
        && m_batch.releaseGroupId == m_currentReleaseGroupId;
    if (inThisBatch && m_discPosition > 0) {
        bool found = false;
        for (auto& d : m_batch.discs) {
            if (d.position == m_discPosition) {
                d.status    = QStringLiteral("done");
                d.savedPath = finalPath;
                d.mbDiscId  = m_mbDiscId;
                found = true;
                break;
            }
        }
        if (!found) {
            // Shouldn't happen in normal flow but tolerate it for
            // robustness — append the disc.
            RipBatchDisc d;
            d.position  = m_discPosition;
            d.status    = QStringLiteral("done");
            d.savedPath = finalPath;
            d.mbDiscId  = m_mbDiscId;
            m_batch.discs.append(d);
        }
        if (m_batch.parentFolder.isEmpty()) {
            m_batch.parentFolder = QDir(finalPath).path()
                                       .section(QChar('/'), 0, -2);
        }
        int resolved = 0, nextPending = 0;
        for (const auto& d : m_batch.discs) {
            if (d.status == QStringLiteral("done")
                || d.status == QStringLiteral("skipped"))
            {
                ++resolved;
            } else if (nextPending == 0) {
                nextPending = d.position;
            }
        }
        m_batchDoneCount    = resolved;
        m_batchExpectedDisc = nextPending > 0 ? nextPending : resolved;
        RipBatchStore::save(m_batch);
        emit batchChanged();
    }

    emit discSaved(fromTempDir, finalPath);

    // If there are more discs in the batch, return to WaitingForDisc;
    // otherwise we're done.
    if (m_inBatch && m_batchDoneCount < m_batchTotalCount) {
        resetDiscState();
        emit discChanged();
        emit tracksChanged();
        emit progressChanged();
        emit zeroFilledRangesChanged();
        setState(State::WaitingForDisc);
    } else {
        setState(State::Done);
    }
}

void Ripper::onRipCancelled() {
    // Worker has finished cleaning up. Return to either WaitingForDisc
    // (if a batch is active and the user wants to retry) or Idle.
    resetDiscState();
    emit discChanged();
    emit tracksChanged();
    emit progressChanged();
    emit zeroFilledRangesChanged();
    setState(m_inBatch ? State::WaitingForDisc : State::Idle);
}

void Ripper::onFailed(QString message) {
    m_errorMessage = std::move(message);
    setState(State::Failed);
}

} // namespace plyr::cd
