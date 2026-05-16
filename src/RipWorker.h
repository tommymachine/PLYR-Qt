// RipWorker — the rip pipeline running on its own QThread.
//
// Port of `cdrip_main.cpp::runRip` shaped for the GUI. The Ripper QObject
// owns a QThread and a RipWorker on that thread; the RipWorker exposes
// slots invoked via queued connections from the Ripper, and emits signals
// (also queued) that the Ripper translates into Q_PROPERTY updates for
// QML.
//
// The rip flow inside `doRip()`:
//
//   1. Open the CdDevice on `bsdName`, read TOC, classify (audio-only).
//   2. Compute disc IDs (AccurateRip / CDDB / MusicBrainz).
//   3. Look up drive offset in the bundled AR DB.
//   4. emit discIdentified(...) — TOC + drive info, before MB lookup.
//   5. MusicBrainz lookup (HTTP). emit mbResolved / mbUnavailable.
//   6. Allocate the padded in-memory disc buffer and read all sectors,
//      retrying transient errors and zero-filling unrecoverable ones.
//   7. Eject the drive (encode / verify don't need it).
//   8. Encode each track to FLAC inside a temp directory under
//      ~/Library/Application Support/Concerto/rip_in_progress/<uuid>/,
//      with MB tags if available.
//   9. AccurateRip + CTDB lookups; per-track v1/v2 + CRC32 checksums at
//      offset 0 (the drive offset was applied at read time). Track
//      statuses become ok / warn / fail / unknown.
//  10. emit readyToSave(tempDir, suggestedFolderName).
//  11. On doSave(parent, name) from the Ripper, std::filesystem::rename
//      the temp dir into place, emit discSaved(finalPath).
//
// The worker is reusable across discs in a batch — `doRip()` runs once,
// goes idle until `doSave()` completes, and is ready for the next disc.

#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>

namespace plyr::cd {

class RipWorker : public QObject {
    Q_OBJECT
public:
    explicit RipWorker(QObject* parent = nullptr);
    ~RipWorker() override;

public slots:
    // The full rip flow against the drive at `bsdName` (e.g. "disk5").
    // `preferredParentFolder` lets a batch hand in the parent it's
    // already saved discs to — when set (and MB matches), the worker
    // skips its AppSupport temp dir and writes the FLACs directly into
    // <preferredParentFolder>/<suggestedName>/, so the GUI sees the
    // disc folder appear in the right place during the rip.
    // Empty means "use the AppSupport temp area + prompt to save at
    // the end" (the old behavior, kept for standalone rips and the
    // first disc of a brand-new batch).
    void doRip(const QString& bsdName,
               const QString& preferredParentFolder = {});

    // Move the rip-in-progress temp dir into `parentFolder/folderName`.
    // Both must be valid and non-empty; the folder name is auto-derived
    // by the Ripper from MB. Emits discSaved(finalPath) on success.
    void doSave(const QString& parentFolder, const QString& folderName);

    // Discard the in-progress rip. Sets an atomic the read loop polls;
    // also removes the temp dir if one exists. Direct-call safe from the
    // main thread (the atomic is the only shared state touched). Emits
    // ripCancelled() once the worker comes to rest.
    void doCancel();

    // Drop the currently-staged temp dir (post-rip, pre-save). Called by
    // the Ripper when the user dismisses the save picker mid-SavePending
    // and confirms "Delete rip".
    void discardStagedRip();

signals:
    // ---- Identification phase ---------------------------------------
    // map contains: driveName, vendor, product, revision,
    //   driveOffsetSamples (int), driveOffsetFromDb (bool),
    //   trackCount (int), totalDurationSec (int),
    //   tracks (QVariantList of {number, durationSec,
    //                            startFraction, endFraction,
    //                            status="pending"}),
    //   mbDiscId, accurateRipId.
    void discIdentified(QVariantMap info);

    // map: hasMatch (bool), albumTitle, artist, date, country,
    //   discPosition (int), discTotalCount (int),
    //   releaseGroupId, releaseId,
    //   tracks (parallel to discIdentified.tracks but with `title`
    //           filled in from MB).
    void mbResolved(QVariantMap match);
    void mbUnavailable();

    // ---- Read phase --------------------------------------------------
    // Fired once at the start of the read loop so the Ripper can switch
    // state Identifying -> Reading.
    void readStarted();

    // Heartbeat — fired roughly 5–10× per disc.
    void readProgress(int currentLba,
                      double secPerSec,
                      double multiplier,
                      int etaSec,
                      int currentTrackNumber,
                      double readFraction);

    // A range of sectors was zero-filled because read retries didn't
    // recover. Map: { fraction (0..1), sectors (int), lba (int) }.
    void zeroFilled(QVariantMap range);

    // Free-form text shown as a toast / inline message in QML.
    void warning(QString message);

    // Live-PCM preview hooks. The Ripper relays these to the
    // AudioEngine's streaming-preview slots so the user can hear the
    // disc within ~2 s of insertion — drive read returns ~360 ms of
    // CDDA per chunk, which we forward straight to a QAudioSink.
    // `totalDurationMs` is the disc's full audio duration, derived
    // from the TOC; the AudioEngine uses it to populate the synthetic
    // segment's durationMs so the seek slider + duration label work
    // identically to file playback.
    void previewStreamStart(qint64 totalDurationMs);
    void previewPcm(QByteArray int16Bytes);
    void previewStreamStop();

    // ---- Encode + Verify phases -------------------------------------
    // Encoding runs inline with the read loop now — each track is FLAC-
    // encoded as soon as its sectors are buffered, so the GUI can start
    // playback of track 1 while track 2 is still being read.
    void encodingStarted();
    void encodedTrack(int trackNumber, QString flacPath);
    void encodingComplete();

    void verifyingStarted();
    void verifyTrackResult(int trackNumber, QString status);
    void verifyComplete(QString summary);

    // ---- Save phase --------------------------------------------------
    // The temp directory's name and the auto-derived folder name (which
    // the Ripper passes back into `doSave`). Auto name is
    // "<AlbumTitle>_Disc_NN" for multi-disc releases, "<AlbumTitle>"
    // otherwise, "Untitled_CD_<ts>" if MB had no match.
    void readyToSave(QString tempDirPath, QString suggestedFolderName);

    // Both paths emit this. The Ripper relays it to main.cpp, which
    // either remaps the playlist (streaming-preview was already playing
    // from `fromTempDir`) or cold-opens `finalPath` from scratch.
    void discSaved(QString fromTempDir, QString finalPath);

    // ---- Terminal -----------------------------------------------------
    // Recoverable cancellations from doCancel(); the temp dir is dropped
    // and the worker is ready for another doRip().
    void ripCancelled();

    // Unrecoverable failures (open failed, TOC unreadable, mixed-mode
    // disc, etc.). Same end state as cancellation, but the Ripper
    // surfaces the message via the Failed state.
    void failed(QString message);

private:
    std::atomic<bool> m_cancel{false};
    QString           m_currentTempDir;  // populated by doRip
};

} // namespace plyr::cd
