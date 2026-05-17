// Sidecar SQLite store for the folder-time library metadata pipeline.
//
// Schema lives in CREATE TABLE statements (see open() / applyV1Schema()).
// The cache file at metadata-cache/ remains the rip-time response store;
// this DB is the user-facing collection — every release the user has
// ever opened a folder for lands here, keyed by MB release MBID, with
// per-file rows keyed by audio-frame content_hash.
//
// Schema follows §6.3 of docs/LIBRARY_METADATA_PLAN.md, with the §A.5
// additions for the trust-marker / writeback story
// (`releases.pipeline_version`, `files.writeback_at`).
//
// Threading: methods are not reentrant. Wrap your usage in a single
// owning thread; the existing rip-pipeline pattern (one worker thread
// owns one resolver + cache) carries over.

#pragma once

#include "MetadataModel.h"

#include <QHash>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <optional>

namespace concerto::library {

// Current schema version. Bumped when the migration step adds a new
// numbered migration. v1 lands the §6.3 schema + the §A.5 trust columns
// up front so no migration is necessary on initial create.
constexpr int kLibrarySchemaVersion = 1;

// Mirrors the §A.6 constant from the plan; consumed by Stage Z.
constexpr int kConcertoPipelineVersion = 1;

struct ReleaseRow {
    QString releaseMbid;
    QString releaseGroupMbid;
    QString title;
    QString artistCredit;
    QString albumArtist;
    QString albumArtistMbid;
    QString date;
    QString originalDate;
    QString country;
    QString barcode;
    QString catalogNumber;
    QString label;
    QString asin;
    QString coverArtUrl;
    QString discSubtitle;
    int     discPosition   = 1;
    int     discTotalCount = 1;
    QString mbDiscId;
    QString sourceTag;        // 'musicbrainz' | 'cd-text' | 'acoustid' | 'unresolved' | 'user'
    int     confidence     = 0;
    QString pickReason;
    QString scoringLogJson;   // JSON blob of AlbumMeta::scoringLog
    QString pipelineLogJson;
    QString rawMbJson;
    int     pipelineVersion = kConcertoPipelineVersion;
    qint64  cachedAt       = 0;
};

struct FileRow {
    QString contentHash;      // primary key — SHA-256 over audio frames only
    QString path;
    QString format;           // 'flac' | 'mp3' | 'm4a' | 'ogg' | 'wav' | 'aiff'
    double  durationSec   = 0.0;
    int     bitDepth      = 0;
    int     sampleRate    = 0;
    int     channelCount  = 0;
    qint64  sizeBytes     = 0;
    qint64  firstSeenTs   = 0;
    qint64  lastSeenTs    = 0;
    QString releaseMbid;      // nullable — set after identification
    int     trackPosition = 0;
    int     mediumPosition = 0;
    qint64  writebackAt   = 0; // 0 = never written back
};

struct FolderRow {
    qint64  id           = 0;  // SQLite AUTOINCREMENT rowid
    QString path;
    QString lastIdentifySource;  // 'marker-trust' | 'mb-id-quorum' | 'mb-discid' | 'unidentified'
    QString releaseMbid;         // nullable
    qint64  lastScannedTs = 0;
    int     fileCount    = 0;
    bool    isLocked     = false;
};

// One row per MB recording on a release (movement-level).
struct TrackRow {
    QString releaseMbid;
    int     mediumPosition = 0;
    int     trackPosition  = 0;
    QString recordingMbid;
    QString title;
    QString workMbid;
    QString movementName;
    int     movementNumber = 0;
    int     movementTotal  = 0;
    qint64  durationMs    = 0;
    QString isrc;
};

class LibraryDatabase {
public:
    // Default: <appDataDir>/library/library.db. Honours the rebrand —
    // appDataDir() reads applicationName(), so on macOS this lands at
    // ~/Library/Application Support/Concerto/library/library.db.
    static QString defaultDbPath();

    explicit LibraryDatabase(QString dbPath = defaultDbPath());
    ~LibraryDatabase();

    LibraryDatabase(const LibraryDatabase&) = delete;
    LibraryDatabase& operator=(const LibraryDatabase&) = delete;

    // Open + run migrations. Returns false if SQLite refused or schema
    // upgrade failed. Safe to call multiple times — second call is a
    // no-op if already open against the same path.
    bool open();

    QString dbPath() const { return m_dbPath; }
    bool    isOpen() const { return m_isOpen; }

    // ---- releases ----
    std::optional<int64_t>   findReleaseRowidByMbid(const QString& mbid) const;
    std::optional<ReleaseRow> findReleaseByMbid(const QString& mbid) const;
    int64_t                  upsertRelease(const ReleaseRow& row);

    // Convenience: convert an AlbumMeta to a ReleaseRow and upsert.
    // Also persists per-track rows. Returns the release rowid or 0 on
    // failure.
    int64_t                  upsertAlbumMeta(
        const concerto::metadata::AlbumMeta& album,
        const QString& rawMbJson = QString());

    // ---- tracks ----
    int                      upsertTracksForRelease(
        const QString& releaseMbid,
        const QVector<TrackRow>& tracks);

    // ---- files ----
    std::optional<FileRow>   findFileByHash(const QString& contentHash) const;
    std::optional<FileRow>   findFileByPath(const QString& path) const;
    int64_t                  upsertFile(const FileRow& row);

    // Files that have an associated release_mbid AND whose
    // releases.pipeline_version >= the consumer's expectation.
    // Used by Stage Z's DB-as-authority check (plan §A.4).
    bool                     isFileTrusted(const QString& contentHash,
                                           int requiredVersion
                                               = kConcertoPipelineVersion) const;

    // ---- folders ----
    std::optional<FolderRow> findFolderByPath(const QString& path) const;
    int64_t                  upsertFolder(const FolderRow& row);

    // ---- diagnostics ----
    int schemaVersion() const;
    QString lastErrorString() const { return m_lastError; }

private:
    bool applyV1Schema();
    bool ensureSchemaVersionTable();
    bool recordSchemaVersion(int v);

    QString m_dbPath;
    QString m_connectionName;
    QString m_lastError;
    bool    m_isOpen = false;
};

} // namespace concerto::library
