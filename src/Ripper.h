// Ripper — QObject orchestrating the GUI-driven CD-rip flow.
//
// Wraps the runRip pipeline (TOC → MusicBrainz → drive offset → read with
// retry/zero-fill → offset-corrected encode → AR/CTDB verify → save) behind
// a state machine that QML can bind to. The actual disc I/O and encoding
// runs on a worker thread; this class lives on the GUI thread and is the
// single source of truth for everything the rip view displays.
//
// Stub note: phase implementations are stand-ins for now. They emit canned
// progress so the QML rip view can be built and iterated against. The real
// pipeline gets ported into RipWorker (a QObject moved onto a QThread) once
// the visual side is settled.
#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <qqmlregistration.h>

namespace plyr::cd {

class Ripper : public QObject {
    Q_OBJECT
    QML_ELEMENT

public:
    // State machine. The QML rip view picks its layout off this enum; one
    // value, one screen-mode. Transitions are linear except Cancelling /
    // Failed, which can be entered from any active phase.
    enum class State {
        // No rip session is open. Header pill is hidden, rip view closed.
        Idle,
        // Rip view is open. Waiting for a disc to be inserted. In a batch
        // context this is the "Insert disc N of M" screen.
        WaitingForDisc,
        // Disc claimed, reading TOC + computing disc IDs + MusicBrainz
        // lookup + drive-offset DB lookup. Fast (~1-3s on a warm cache).
        Identifying,
        // Sectors being read from the disc into the in-memory PCM buffer.
        // The long phase: ~8-15 min for a full disc on a SuperDrive.
        Reading,
        // Per-track FLAC encoding from the PCM buffer. <30s for a full
        // disc; uses the same CD canvas as Reading, but the read fill
        // freezes and tracks light up briefly as they're written.
        Encoding,
        // AccurateRip + CTDB lookups + per-track checksum match. Each
        // track shows ✓ / ⚠ / — as results come in.
        Verifying,
        // Rip is complete in a temp dir. Waiting for the user to pick a
        // destination folder via the save dialog. The save button is the
        // only thing the user can interact with from the rip view now.
        SavePending,
        // The temp dir is being moved into the chosen destination. Brief.
        Saving,
        // Single-disc done, or final disc of a batch done. Rip view shows
        // the summary and offers to close. The just-saved folder has
        // already been opened in the playlist and playback started.
        Done,
        // User pressed stop. Worker thread is winding down; the in-progress
        // disc's PCM buffer is discarded. The current disc reverts to
        // "pending" in batch state, if a batch is active.
        Cancelling,
        // Unrecoverable error. errorMessage() carries the diagnostic.
        Failed,
    };
    Q_ENUM(State)

    explicit Ripper(QObject* parent = nullptr);
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
    // Each entry: { number, title, durationSec, startFraction, endFraction,
    //               status } where startFraction/endFraction are 0..1 over
    //               the audio body (used by the CD canvas to draw track
    //               boundary radii) and status is one of "pending",
    //               "reading", "read", "encoded", "ok", "warn", "fail",
    //               "unknown" (the latter four are verify-time states).
    QVariantList tracks() const          { return m_tracks; }

    // ---- Read / Encode / Verify progress (0..1) --------------------
    double readProgress() const          { return m_readProgress; }
    double encodeProgress() const        { return m_encodeProgress; }
    double verifyProgress() const        { return m_verifyProgress; }

    // Live read-stage telemetry (Reading state only).
    int    currentLba() const            { return m_currentLba; }
    double currentSpeedSecPerSec() const { return m_currentSpeedSecPerSec; }
    double currentMultiplier() const     { return m_currentMultiplier; }
    int    etaSec() const                { return m_etaSec; }
    int    currentTrackNumber() const    { return m_currentTrackNumber; }

    // Verify summary line shown after verify completes. e.g.
    // "16/16 ACCURATE  ·  AR offset 0  ·  CTDB confidence 12".
    QString verifySummary() const        { return m_verifySummary; }

    // Zero-fill marker positions for the CD canvas, each entry:
    // { fraction (0..1 over audio body), sectors }.
    QVariantList zeroFilledRanges() const { return m_zeroFilledRanges; }

    // ---- Batch context --------------------------------------------
    bool    inBatch() const              { return m_inBatch; }
    QString batchAlbumTitle() const      { return m_batchAlbumTitle; }
    int     batchDoneCount() const       { return m_batchDoneCount; }
    int     batchTotalCount() const      { return m_batchTotalCount; }
    int     batchExpectedDisc() const    { return m_batchExpectedDisc; }
    QVariantList resumableBatches() const { return m_resumableBatches; }

    // ---- Q_PROPERTY block (declared after accessors so the macros can
    //      reference them without forward-declaration noise). ---------
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

    Q_PROPERTY(bool     inBatch         READ inBatch         NOTIFY batchChanged)
    Q_PROPERTY(QString  batchAlbumTitle READ batchAlbumTitle NOTIFY batchChanged)
    Q_PROPERTY(int      batchDoneCount  READ batchDoneCount  NOTIFY batchChanged)
    Q_PROPERTY(int      batchTotalCount READ batchTotalCount NOTIFY batchChanged)
    Q_PROPERTY(int      batchExpectedDisc READ batchExpectedDisc NOTIFY batchChanged)
    Q_PROPERTY(QVariantList resumableBatches READ resumableBatches NOTIFY resumableBatchesChanged)

public slots:
    // Open the rip view. If a disc is in the drive, kicks off identification
    // immediately; otherwise sits in WaitingForDisc. If `resumeBatchId` is
    // non-empty, the named batch is the active context.
    void startSession(const QString& resumeBatchId = {});

    // Close the rip view. Any in-progress disc rip is cancelled; the batch
    // state (if active) is preserved on disk as "Resume later".
    void endSession();

    // Stop the current disc rip but keep the batch resumable. Used by the
    // top-left X and the bottom-center "Not now". `deleteBatch=true` also
    // removes the batch state file (already-saved disc folders untouched).
    void stopRip(bool deleteBatch = false);

    // After SavePending, commit the temp dir into `parentFolder`. The
    // subfolder name is auto-generated from MB; pass an explicit name to
    // override (empty = use auto). Triggers playback of the saved folder.
    void saveTo(const QUrl& parentFolder, const QString& folderNameOverride = {});

    // Eject the inserted disc — usable from the WaitingForDisc state when a
    // wrong disc is in the drive (e.g. user inserted disc 5 of the batch
    // but we expect disc 3).
    void ejectDisc();

    // Delete a resumable batch by id without entering it. Used from the
    // batch picker. Already-saved disc folders are not touched.
    void deleteResumableBatch(const QString& batchId);

    // ---- Stub-only: demo stage stepping --------------------------
    // Used by the design-review UI to tab through every visible phase
    // without waiting on the stub timer. Will be removed when the real
    // RipWorker lands.
    Q_INVOKABLE void demoStep(int delta);
    Q_INVOKABLE void demoToggleAutoAdvance();
    Q_INVOKABLE int  demoStepIndex() const { return m_demoStep; }
    Q_INVOKABLE int  demoStepCount() const;

signals:
    void stateChanged();
    void discChanged();
    void tracksChanged();
    void progressChanged();
    void zeroFilledRangesChanged();
    void batchChanged();
    void resumableBatchesChanged();

    // One-shot notifications routed to the QML side. The rip view turns
    // these into toasts / inline messages.
    void warning(const QString& message);
    // Fired when SavePending → Saving → Done completes and the just-saved
    // folder is ready to be opened in the playlist. The Main.qml wiring
    // calls playlist.openFolder(savedPath) + setCurrentIndex(0).
    void discSaved(const QString& savedPath);

private:
    // ---- Stub pipeline: walks through every state on a QTimer, emitting
    //      progress so the QML can be built and iterated. The real
    //      pipeline lands in RipWorker once the visual side is settled.
    void stubKickoff();
    void stubAdvance();
    void setState(State s);

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
    QString m_batchId;
    QString m_batchAlbumTitle;
    int     m_batchDoneCount      = 0;
    int     m_batchTotalCount     = 1;
    int     m_batchExpectedDisc   = 1;
    QVariantList m_resumableBatches;

    // Stub-pipeline timer state. Removed when the real RipWorker lands.
    QTimer* m_stubTimer           = nullptr;
    int     m_stubTick            = 0;
    int     m_demoStep            = 0;
    void    applyDemoStep(int idx);
};

} // namespace plyr::cd
