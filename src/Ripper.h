// Ripper — QObject orchestrating the GUI-driven CD-rip flow.
//
// Owns a QThread + RipWorker; the worker runs the rip pipeline
// (TOC → MusicBrainz → drive offset → read with retry/zero-fill →
//  offset-corrected encode → AR/CTDB verify → save-at-end). The Ripper
// itself lives on the GUI thread and is the single source of truth for
// every property the rip view binds to.
//
// CdShield is also passed in: the Ripper registers a disc-appeared
// listener so insertion of a CD wakes the worker (no polling).
#pragma once

#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <qqmlregistration.h>

#include "RipBatchStore.h"

class QThread;

namespace plyr::cd {

class CdShield;
class RipWorker;

class Ripper : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Instantiated in C++ and exposed as a context property")

public:
    enum class State {
        Idle,
        WaitingForDisc,
        Identifying,
        Reading,
        Encoding,
        Verifying,
        SavePending,
        Saving,
        Done,
        Cancelling,
        Failed,
    };
    Q_ENUM(State)

    explicit Ripper(CdShield* shield = nullptr, QObject* parent = nullptr);
    ~Ripper() override;

    // ---- State ------------------------------------------------------
    State state() const                  { return m_state; }
    QString errorMessage() const         { return m_errorMessage; }

    // ---- Disc + drive (filled during Identifying) ------------------
    bool    discPresent() const          { return m_discPresent; }
    QString driveName() const            { return m_driveName; }
    int     driveOffsetSamples() const   { return m_driveOffsetSamples; }
    bool    driveOffsetFromDb() const    { return m_driveOffsetFromDb; }
    int     trackCount() const           { return m_trackCount; }
    int     totalDurationSec() const     { return m_totalDurationSec; }

    // ---- MusicBrainz match -----------------------------------------
    bool    hasMatch() const             { return m_hasMatch; }
    QString albumTitle() const           { return m_albumTitle; }
    QString artist() const               { return m_artist; }
    QString date() const                 { return m_date; }
    int     discPosition() const         { return m_discPosition; }
    int     discTotalCount() const       { return m_discTotalCount; }
    QString mbDiscId() const             { return m_mbDiscId; }
    QString accurateRipId() const        { return m_accurateRipId; }

    // ---- Tracks ----------------------------------------------------
    QVariantList tracks() const          { return m_tracks; }

    // ---- Read / Encode / Verify progress (0..1) --------------------
    double readProgress() const          { return m_readProgress; }
    double encodeProgress() const        { return m_encodeProgress; }
    double verifyProgress() const        { return m_verifyProgress; }

    int    currentLba() const            { return m_currentLba; }
    double currentSpeedSecPerSec() const { return m_currentSpeedSecPerSec; }
    double currentMultiplier() const     { return m_currentMultiplier; }
    int    etaSec() const                { return m_etaSec; }
    int    currentTrackNumber() const    { return m_currentTrackNumber; }
    QString verifySummary() const        { return m_verifySummary; }

    QVariantList zeroFilledRanges() const { return m_zeroFilledRanges; }

    // The path the playlist is currently rooted at for THIS rip's
    // streaming-preview / post-save playback. Empty when nothing from
    // this disc is being played (e.g. WaitingForDisc, or mid-rip before
    // track 1 lands). The QML track list uses it to decide whether
    // `playlist.currentTrackNumber` refers to a track in this view's
    // list, vs an unrelated album playing underneath.
    QString currentDiscPlaybackPath() const { return m_currentDiscPlaybackPath; }

    // ---- Batch context --------------------------------------------
    bool    inBatch() const              { return m_inBatch; }
    QString batchAlbumTitle() const      { return m_batchAlbumTitle; }
    QString batchArtist() const          { return m_batch.artist; }
    int     batchDoneCount() const       { return m_batchDoneCount; }
    int     batchTotalCount() const      { return m_batchTotalCount; }
    int     batchExpectedDisc() const    { return m_batchExpectedDisc; }
    QString batchParentFolder() const    { return m_batch.parentFolder; }
    QVariantList resumableBatches() const { return m_resumableBatches; }

    Q_PROPERTY(State    state           READ state           NOTIFY stateChanged)
    Q_PROPERTY(QString  errorMessage    READ errorMessage    NOTIFY stateChanged)

    Q_PROPERTY(bool     discPresent     READ discPresent     NOTIFY discChanged)
    Q_PROPERTY(QString  driveName       READ driveName       NOTIFY discChanged)
    Q_PROPERTY(int      driveOffsetSamples READ driveOffsetSamples NOTIFY discChanged)
    Q_PROPERTY(bool     driveOffsetFromDb  READ driveOffsetFromDb  NOTIFY discChanged)
    Q_PROPERTY(int      trackCount      READ trackCount      NOTIFY discChanged)
    Q_PROPERTY(int      totalDurationSec READ totalDurationSec NOTIFY discChanged)

    Q_PROPERTY(bool     hasMatch        READ hasMatch        NOTIFY discChanged)
    Q_PROPERTY(QString  albumTitle      READ albumTitle      NOTIFY discChanged)
    Q_PROPERTY(QString  artist          READ artist          NOTIFY discChanged)
    Q_PROPERTY(QString  date            READ date            NOTIFY discChanged)
    Q_PROPERTY(int      discPosition    READ discPosition    NOTIFY discChanged)
    Q_PROPERTY(int      discTotalCount  READ discTotalCount  NOTIFY discChanged)
    Q_PROPERTY(QString  mbDiscId        READ mbDiscId        NOTIFY discChanged)
    Q_PROPERTY(QString  accurateRipId   READ accurateRipId   NOTIFY discChanged)

    Q_PROPERTY(QVariantList tracks      READ tracks          NOTIFY tracksChanged)

    Q_PROPERTY(double   readProgress    READ readProgress    NOTIFY progressChanged)
    Q_PROPERTY(double   encodeProgress  READ encodeProgress  NOTIFY progressChanged)
    Q_PROPERTY(double   verifyProgress  READ verifyProgress  NOTIFY progressChanged)
    Q_PROPERTY(int      currentLba      READ currentLba      NOTIFY progressChanged)
    Q_PROPERTY(double   currentSpeedSecPerSec READ currentSpeedSecPerSec NOTIFY progressChanged)
    Q_PROPERTY(double   currentMultiplier     READ currentMultiplier     NOTIFY progressChanged)
    Q_PROPERTY(int      etaSec          READ etaSec          NOTIFY progressChanged)
    Q_PROPERTY(int      currentTrackNumber READ currentTrackNumber NOTIFY progressChanged)
    Q_PROPERTY(QString  verifySummary   READ verifySummary   NOTIFY progressChanged)

    Q_PROPERTY(QVariantList zeroFilledRanges READ zeroFilledRanges NOTIFY zeroFilledRangesChanged)
    Q_PROPERTY(QString currentDiscPlaybackPath READ currentDiscPlaybackPath NOTIFY currentDiscPlaybackPathChanged)

    Q_PROPERTY(bool     inBatch         READ inBatch         NOTIFY batchChanged)
    Q_PROPERTY(QString  batchAlbumTitle READ batchAlbumTitle NOTIFY batchChanged)
    Q_PROPERTY(QString  batchArtist     READ batchArtist     NOTIFY batchChanged)
    Q_PROPERTY(int      batchDoneCount  READ batchDoneCount  NOTIFY batchChanged)
    Q_PROPERTY(int      batchTotalCount READ batchTotalCount NOTIFY batchChanged)
    Q_PROPERTY(int      batchExpectedDisc READ batchExpectedDisc NOTIFY batchChanged)
    Q_PROPERTY(QString  batchParentFolder READ batchParentFolder NOTIFY batchChanged)
    Q_PROPERTY(QVariantList resumableBatches READ resumableBatches NOTIFY resumableBatchesChanged)

public slots:
    // Open the rip view. If a disc is in the drive, identification kicks
    // off immediately. If `resumeBatchId` is non-empty, the named batch
    // becomes the active context.
    void startSession(const QString& resumeBatchId = {});

    // Close the rip view. Any in-progress disc rip is cancelled but the
    // batch state stays resumable.
    void endSession();

    // Stop the current disc rip, keep the batch resumable (unless
    // `deleteBatch=true`, which also drops the JSON file).
    void stopRip(bool deleteBatch = false);

    // After SavePending, commit the temp dir into `parentFolder`. The
    // subfolder name is auto-generated from MB (or `folderNameOverride`
    // if provided). Triggers playback of the saved folder.
    void saveTo(const QUrl& parentFolder, const QString& folderNameOverride = {});

    // The user dismissed the save picker and confirmed "Delete rip".
    // Drops the staged temp dir and ends the session.
    void discardStagedRip();

    // Eject from the WaitingForDisc state when a wrong disc is inserted.
    void ejectDisc();

    // Mark the disc the batch is currently expecting as `skipped` and
    // bump the expected disc to the next pending one. Persists to the
    // batch JSON so the skip survives restarts. Used when the user
    // doesn't own a particular disc in the box set, or wants to skip
    // it and come back later — folder memory (parent_folder) persists.
    void skipCurrentDisc();

    // Drop a resumable batch by id from the resume picker.
    void deleteResumableBatch(const QString& batchId);

    // Refresh `resumableBatches` from disk. Called by QML before
    // showing the resume picker so the property is always current.
    void refreshResumableBatches();

signals:
    void stateChanged();
    void discChanged();
    void tracksChanged();
    void progressChanged();
    void zeroFilledRangesChanged();
    void batchChanged();
    void resumableBatchesChanged();
    void currentDiscPlaybackPathChanged();

    void warning(const QString& message);

    // Per-track-encoded hook — main.cpp uses it to add the encoded FLAC
    // to the playlist so the user can later rewind to that track. The
    // ACTUAL playback path during rip is the streaming-preview signals
    // below (raw PCM straight off the drive); this signal is just for
    // playlist housekeeping.
    void discTrackReady(const QString& tempDir,
                        const QString& flacPath,
                        int trackNumber);

    // Live raw-PCM preview signals. These get wired to the AudioEngine
    // streaming slots in main.cpp; the user hears CDDA bytes within
    // ~2 s of insertion — drive spin-up + first 27-sector read.
    void previewStreamStart(qint64 totalDurationMs, qint64 startOffsetMs);
    void previewPcm(QByteArray int16Bytes);
    void previewStreamStop();

    // Disc moved into its final home. `fromTempDir` is the temp parent
    // streaming-preview was rooted at; `finalPath` is the destination
    // folder. main.cpp uses fromTempDir to know whether the playlist
    // can be remapped (preserving playback position) instead of cold-
    // opened.
    void discSaved(const QString& fromTempDir,
                   const QString& finalPath);

private slots:
    // Receivers for RipWorker signals. Run on the GUI thread via queued
    // connections; update state + re-emit to QML.
    void onDiscIdentified(QVariantMap info);
    void onMbResolved(QVariantMap match);
    void onMbUnavailable();
    void onRipStarting(QString tempDir);
    void onReadStarted();
    void onReadProgress(int currentLba, double secPerSec, double multiplier,
                        int etaSec, int currentTrackNumber, double readFraction);
    void onZeroFilled(QVariantMap range);
    void onEncodingStarted();
    void onEncodedTrack(int trackNumber, QString flacPath);
    void onEncodingComplete();
    void onVerifyingStarted();
    void onVerifyTrackResult(int trackNumber, QString status);
    void onVerifyComplete(QString summary);
    void onReadyToSave(QString tempDir, QString suggestedFolderName);
    void onWorkerDiscSaved(QString fromTempDir, QString finalPath);
    void onRipCancelled();
    void onFailed(QString message);

private:
    void setState(State s);
    void setTrackStatus(int oneBased, const QString& status);
    void resetDiscState();
    void onCdShieldDiscAppeared(const std::string& bsdName);
    void onCdShieldDiscDisappeared(const std::string& bsdName);

    // Worker / thread machinery.
    QThread*   m_thread = nullptr;
    RipWorker* m_worker = nullptr;
    CdShield*  m_shield = nullptr;

    // Active rip context — populated as discIdentified / mbResolved land.
    QString    m_currentBsdName;            // disc the worker is on
    QString    m_currentTempDir;            // populated by encodedTrack/readyToSave
    QString    m_suggestedFolderName;       // populated by readyToSave
    QString    m_currentReleaseGroupId;     // for batch lookup at save time
    QString    m_currentDiscPlaybackPath;   // playlist's rip-folder root
    QString    m_resumeTempDir;             // non-empty when startSession
                                            // loaded a batch with an
                                            // in_progress disc; handed to
                                            // doRip if the inserted disc
                                            // matches its mbDiscId.
    QString    m_resumeMbDiscId;            // mbDiscId of the in-progress
                                            // disc, used to gate the
                                            // resume on the inserted disc

    // Batch context — persisted via RipBatchStore.
    RipBatch m_batch;       // empty when m_inBatch == false

    // Backing state.
    State   m_state               = State::Idle;
    QString m_errorMessage;

    bool    m_discPresent         = false;
    QString m_driveName;
    int     m_driveOffsetSamples  = 0;
    bool    m_driveOffsetFromDb   = false;
    int     m_trackCount          = 0;
    int     m_totalDurationSec    = 0;

    bool    m_hasMatch            = false;
    QString m_albumTitle;
    QString m_artist;
    QString m_date;
    int     m_discPosition        = 1;
    int     m_discTotalCount      = 1;
    QString m_mbDiscId;
    QString m_accurateRipId;

    QVariantList m_tracks;

    double  m_readProgress        = 0.0;
    double  m_encodeProgress      = 0.0;
    double  m_verifyProgress      = 0.0;
    int     m_currentLba          = 0;
    double  m_currentSpeedSecPerSec = 0.0;
    double  m_currentMultiplier   = 0.0;
    int     m_etaSec              = 0;
    int     m_currentTrackNumber  = 0;
    QString m_verifySummary;
    QVariantList m_zeroFilledRanges;

    bool    m_inBatch             = false;
    QString m_batchAlbumTitle;
    int     m_batchDoneCount      = 0;
    int     m_batchTotalCount     = 1;
    int     m_batchExpectedDisc   = 1;
    QVariantList m_resumableBatches;
};

} // namespace plyr::cd
