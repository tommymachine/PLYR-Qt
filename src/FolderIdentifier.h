// Folder → release MBID strategy chain (plan §2.2).
//
// Mirrors `MetadataResolver`'s shape: async, single `identify()` call
// per Request, emits `identified(Result)` exactly once. Internally
// walks the stages in order:
//
//   Z. Trust marker     — CONCERTO_PIPELINE_VERSION + MUSICBRAINZ_ALBUMID
//                          quorum (in-file marker; DB-as-authority via
//                          LibraryDatabase::isFileTrusted is the §A.4
//                          fallback for the no-writeback case).
//   A. MB ID quorum     — MUSICBRAINZ_ALBUMID consistent across ≥60%
//                          of files → direct release fetch.
//   B. Disc-ID lookup   — MUSICBRAINZ_DISCID present → check the rip
//                          cache (MetadataCache::getByDiscId) first,
//                          then fall back to MB disc-ID fetch.
//   C. Search           — STUB, returns nullopt. Wired for the next
//                          implementation pass.
//   D. Duration finger­ — STUB.
//      print
//
// Stages C and D leave their interface defined so callers can extend
// later without modifying this header. Each Stage's outcome lands in
// `Result::diagnostic` so the CLI test harness can show the chain.
//
// Threading: lives on the caller's thread. The downstream MB Client
// uses fully-async QNetworkAccessManager, so we never spin a nested
// event loop — same property MetadataResolver maintains.

#pragma once

#include "MetadataCache.h"
#include "MetadataModel.h"
#include "MusicBrainz.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

namespace concerto::library {

class LibraryDatabase;

class FolderIdentifier : public QObject {
    Q_OBJECT
public:
    struct Request {
        QString     folderPath;
        QStringList filePaths;     // sorted; canonical audio files only
    };

    struct Result {
        bool        identified = false;
        QString     source;        // "marker-trust" | "mb-id-quorum" | "mb-discid" | "unidentified"
        concerto::metadata::AlbumMeta album;
        QString     diagnostic;    // multiline log of which stages tried + outcome
        QString     folderPath;
        QStringList filePaths;     // echoed back for downstream persisting
    };

    // Both pointers are non-owning. mb may be nullptr for purely
    // marker-trust / cache-only flows; cache may be nullptr to skip
    // the cache lookup in Stage B.
    explicit FolderIdentifier(
        musicbrainz::Client* mb,
        concerto::metadata::MetadataCache* cache,
        LibraryDatabase* db = nullptr,
        QObject* parent = nullptr);

    // Kick off one identification. Emits identified(...) exactly once.
    // Re-entrant on the same instance only after the prior call has
    // emitted; the caller is expected to serialize, as MetadataResolver
    // is.
    void identify(const Request& req);

    // Quorum thresholds. Plan §2.3: ≥60% MBID quorum for Stage A,
    // ≥1 file for Stage Z (one Concerto-touched file in a mixed
    // folder spoofs the whole folder, so we use the same ≥60%).
    static constexpr double kAlbumIdQuorum = 0.60;
    static constexpr double kMarkerTrustQuorum = 0.60;
    static constexpr double kDiscIdQuorum  = 0.50;

signals:
    void identified(concerto::library::FolderIdentifier::Result result);

private slots:
    // Stage A path — release-id-quorum direct fetch.
    void onReleaseResolved(QString releaseMbid, QJsonObject release);
    void onReleaseFailed(QString releaseMbid, int httpStatus, QString message);

    // Stage B disc-ID web fallback.
    void onDiscIdResolved(QString discId, QJsonArray releases);
    void onDiscIdFailed(QString discId, int httpStatus, QString message);

private:
    // Stage Z: scan tags for the §A.1 marker quorum AND the DB-as-
    // authority equivalent via LibraryDatabase::isFileTrusted on the
    // audio-frame content_hash. If trusted, populates Result, emits,
    // returns true.
    bool tryStageZ(const QStringList& files,
                   const QString& folderPath);

    // Stage A: MUSICBRAINZ_ALBUMID quorum. Returns the consensus MBID
    // (or empty string if no quorum). Stages A's downstream is async
    // via fetchRelease.
    QString stageAQuorumMbid(const QStringList& files);

    // Stage B: MUSICBRAINZ_DISCID quorum. Returns the consensus disc-ID.
    QString stageBQuorumDiscId(const QStringList& files);

    void emitIdentified(Result r);
    void appendDiag(const QString& line);

    musicbrainz::Client*               m_mb;
    concerto::metadata::MetadataCache* m_cache;
    LibraryDatabase*                   m_db;

    // Per-call state. We never overlap calls (caller serializes).
    Request m_req;
    Result  m_result;
    QString m_stageInflight;  // "stageA-mb" | "stageB-mb-discid"
    int     m_inflightTrackCount = 0;
};

} // namespace concerto::library

// Required so the signal/slot system can marshal Result through queued
// connections (the QML side or background threads if any).
Q_DECLARE_METATYPE(concerto::library::FolderIdentifier::Result)
