#include "LibraryDatabase.h"

#include "SystemPaths.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QUuid>
#include <QVariant>

namespace concerto::library {

namespace {

QString uniqueConnectionName() {
    // Unique per LibraryDatabase instance so multiple instances can
    // coexist (e.g. the CLI + a test) without QSqlDatabase warnings.
    return QStringLiteral("concerto-library-")
         + QUuid::createUuid().toString(QUuid::Id128);
}

QString jsonFromVariantList(const QVariantList& list) {
    if (list.isEmpty()) return QString();
    return QString::fromUtf8(
        QJsonDocument::fromVariant(list).toJson(QJsonDocument::Compact));
}

QVariantList variantListFromJson(const QString& text) {
    if (text.isEmpty()) return {};
    const auto doc = QJsonDocument::fromJson(text.toUtf8());
    if (!doc.isArray()) return {};
    QVariantList out;
    for (const QJsonValue& v : doc.array()) out.append(v.toVariant());
    return out;
}

bool execOrLog(QSqlQuery& q, const char* what) {
    if (!q.exec()) {
        qWarning() << "[LibraryDatabase]" << what
                   << "failed:" << q.lastError().text()
                   << "for query:" << q.executedQuery();
        return false;
    }
    return true;
}

} // namespace

QString LibraryDatabase::defaultDbPath() {
    return concerto::paths::appDataDir()
         + QStringLiteral("/library/library.db");
}

LibraryDatabase::LibraryDatabase(QString dbPath)
    : m_dbPath(std::move(dbPath))
    , m_connectionName(uniqueConnectionName())
{}

LibraryDatabase::~LibraryDatabase() {
    if (QSqlDatabase::contains(m_connectionName)) {
        {
            QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
            if (db.isOpen()) db.close();
        }
        QSqlDatabase::removeDatabase(m_connectionName);
    }
}

bool LibraryDatabase::open() {
    if (m_isOpen) return true;

    const QFileInfo fi(m_dbPath);
    QDir().mkpath(fi.absolutePath());

    QSqlDatabase db = QSqlDatabase::addDatabase(
        QStringLiteral("QSQLITE"), m_connectionName);
    db.setDatabaseName(m_dbPath);
    if (!db.open()) {
        m_lastError = db.lastError().text();
        qWarning() << "[LibraryDatabase] open failed:" << m_lastError
                   << "path:" << m_dbPath;
        return false;
    }

    // WAL + foreign-keys per §6.4 of the plan.
    QSqlQuery q(db);
    q.exec(QStringLiteral("PRAGMA journal_mode = WAL;"));
    q.exec(QStringLiteral("PRAGMA foreign_keys = ON;"));
    q.exec(QStringLiteral("PRAGMA synchronous = NORMAL;"));

    if (!ensureSchemaVersionTable()) return false;
    const int v = schemaVersion();
    if (v == 0) {
        if (!applyV1Schema()) return false;
        if (!recordSchemaVersion(1)) return false;
    }
    // Future migrations would chain here (e.g. v == 1 → applyV2Schema).
    m_isOpen = true;
    return true;
}

bool LibraryDatabase::ensureSchemaVersionTable() {
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS schema_version ("
        "  version INTEGER PRIMARY KEY,"
        "  applied_at INTEGER NOT NULL"
        ");"));
    return execOrLog(q, "create schema_version");
}

bool LibraryDatabase::recordSchemaVersion(int v) {
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO schema_version(version, applied_at) "
        "VALUES (?, ?);"));
    q.addBindValue(v);
    q.addBindValue(QDateTime::currentSecsSinceEpoch());
    return execOrLog(q, "record schema_version");
}

int LibraryDatabase::schemaVersion() const {
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral(
            "SELECT MAX(version) FROM schema_version;"))) return 0;
    if (!q.next()) return 0;
    bool ok = false;
    const int v = q.value(0).toInt(&ok);
    return ok ? v : 0;
}

bool LibraryDatabase::applyV1Schema() {
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    db.transaction();

    const QStringList stmts = {
        // releases — per-MBID rich metadata. pipeline_version is the
        // §A.5 trust column.
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS releases ("
            "  release_mbid        TEXT PRIMARY KEY,"
            "  release_group_mbid  TEXT,"
            "  title               TEXT,"
            "  artist_credit       TEXT,"
            "  album_artist        TEXT,"
            "  album_artist_mbid   TEXT,"
            "  date                TEXT,"
            "  original_date       TEXT,"
            "  country             TEXT,"
            "  barcode             TEXT,"
            "  catalog_number      TEXT,"
            "  label               TEXT,"
            "  asin                TEXT,"
            "  cover_art_url       TEXT,"
            "  disc_subtitle       TEXT,"
            "  disc_position       INTEGER NOT NULL DEFAULT 1,"
            "  disc_total_count    INTEGER NOT NULL DEFAULT 1,"
            "  mb_disc_id          TEXT,"
            "  source_tag          TEXT NOT NULL,"
            "  confidence          INTEGER NOT NULL DEFAULT 0,"
            "  pick_reason         TEXT,"
            "  scoring_log_json    TEXT,"
            "  pipeline_log_json   TEXT,"
            "  raw_mb_json         TEXT,"
            "  pipeline_version    INTEGER NOT NULL DEFAULT 1,"
            "  cached_at           INTEGER NOT NULL"
            ");"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_releases_mbid ON releases(release_mbid);"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_releases_rg ON releases(release_group_mbid);"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_releases_album_artist ON releases(album_artist);"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_releases_label ON releases(label);"),

        // tracks — one row per (release, medium, position).
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS tracks ("
            "  release_mbid     TEXT NOT NULL,"
            "  medium_position  INTEGER NOT NULL,"
            "  track_position   INTEGER NOT NULL,"
            "  recording_mbid   TEXT,"
            "  title            TEXT,"
            "  work_mbid        TEXT,"
            "  movement_name    TEXT,"
            "  movement_number  INTEGER,"
            "  movement_total   INTEGER,"
            "  duration_ms      INTEGER,"
            "  isrc             TEXT,"
            "  PRIMARY KEY (release_mbid, medium_position, track_position),"
            "  FOREIGN KEY (release_mbid) REFERENCES releases(release_mbid) ON DELETE CASCADE"
            ");"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_tracks_release ON tracks(release_mbid);"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_tracks_recording ON tracks(recording_mbid);"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_tracks_work ON tracks(work_mbid);"),

        // performers — recording_mbid + artist_mbid + role. composer is
        // stored on the tracks row via work_mbid; this table is for the
        // typed performer rels (conductor, performing orchestra, etc).
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS performers ("
            "  recording_mbid      TEXT NOT NULL,"
            "  artist_mbid         TEXT,"
            "  artist_name         TEXT NOT NULL,"
            "  artist_sort         TEXT,"
            "  role                TEXT NOT NULL,"
            "  ordinal             INTEGER NOT NULL,"
            "  instrument_or_voice TEXT,"
            "  PRIMARY KEY (recording_mbid, role, ordinal)"
            ");"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_performers_artist ON performers(artist_mbid);"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_performers_role ON performers(role);"),

        // track_performers — bridge for the per-(release, track, performer)
        // join. Lets a recording shared across compilations keep one
        // performer set while each compilation references it cleanly.
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS track_performers ("
            "  release_mbid      TEXT NOT NULL,"
            "  medium_position   INTEGER NOT NULL,"
            "  track_position    INTEGER NOT NULL,"
            "  recording_mbid    TEXT NOT NULL,"
            "  PRIMARY KEY (release_mbid, medium_position, track_position),"
            "  FOREIGN KEY (release_mbid, medium_position, track_position) "
            "    REFERENCES tracks(release_mbid, medium_position, track_position)"
            "    ON DELETE CASCADE"
            ");"),

        // works — composer rel sits here. composer_name is denormalized
        // for fast sidebar grouping.
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS works ("
            "  work_mbid           TEXT PRIMARY KEY,"
            "  title               TEXT,"
            "  composer_mbid       TEXT,"
            "  composer_name       TEXT,"
            "  composer_sort       TEXT,"
            "  parent_work_mbid    TEXT"
            ");"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_works_composer ON works(composer_mbid);"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_works_parent ON works(parent_work_mbid);"),

        // files — keyed by content_hash (audio-frame SHA-256). Per §6.2.
        // writeback_at is the §A.5 trust-side column.
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS files ("
            "  content_hash      TEXT PRIMARY KEY,"
            "  path              TEXT NOT NULL,"
            "  format            TEXT NOT NULL,"
            "  duration_sec      REAL NOT NULL DEFAULT 0,"
            "  bit_depth         INTEGER,"
            "  sample_rate       INTEGER,"
            "  channel_count     INTEGER,"
            "  size_bytes        INTEGER,"
            "  first_seen_ts     INTEGER NOT NULL,"
            "  last_seen_ts      INTEGER NOT NULL,"
            "  release_mbid      TEXT,"
            "  track_position    INTEGER,"
            "  medium_position   INTEGER,"
            "  writeback_at      INTEGER"
            ");"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_files_path ON files(path);"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_files_release ON files(release_mbid, medium_position, track_position);"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_files_folder_release ON files(release_mbid);"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_files_content_hash ON files(content_hash);"),

        // folders — one row per scanned folder path. NOT the canonical
        // grouping (a folder can hold mixed releases — see §8.2); just
        // a "last scanned this folder" memo for the sweeper.
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS folders ("
            "  id                    INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  path                  TEXT NOT NULL UNIQUE,"
            "  last_identify_source  TEXT,"
            "  release_mbid          TEXT,"
            "  last_scanned_ts       INTEGER NOT NULL DEFAULT 0,"
            "  file_count            INTEGER NOT NULL DEFAULT 0,"
            "  is_locked             INTEGER NOT NULL DEFAULT 0"
            ");"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_folders_release ON folders(release_mbid);"),
    };

    for (const QString& stmt : stmts) {
        QSqlQuery q(db);
        q.prepare(stmt);
        if (!execOrLog(q, "v1 schema stmt")) {
            db.rollback();
            return false;
        }
    }

    db.commit();
    return true;
}

// ---------- releases ----------

std::optional<int64_t> LibraryDatabase::findReleaseRowidByMbid(
    const QString& mbid) const
{
    if (mbid.isEmpty()) return std::nullopt;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT rowid FROM releases WHERE release_mbid = ? LIMIT 1;"));
    q.addBindValue(mbid);
    if (!q.exec() || !q.next()) return std::nullopt;
    return q.value(0).toLongLong();
}

std::optional<ReleaseRow> LibraryDatabase::findReleaseByMbid(
    const QString& mbid) const
{
    if (mbid.isEmpty()) return std::nullopt;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT release_mbid, release_group_mbid, title, artist_credit, "
        "       album_artist, album_artist_mbid, date, original_date, "
        "       country, barcode, catalog_number, label, asin, "
        "       cover_art_url, disc_subtitle, disc_position, "
        "       disc_total_count, mb_disc_id, source_tag, confidence, "
        "       pick_reason, scoring_log_json, pipeline_log_json, "
        "       raw_mb_json, pipeline_version, cached_at "
        "FROM releases WHERE release_mbid = ? LIMIT 1;"));
    q.addBindValue(mbid);
    if (!q.exec() || !q.next()) return std::nullopt;

    ReleaseRow r;
    int i = 0;
    r.releaseMbid       = q.value(i++).toString();
    r.releaseGroupMbid  = q.value(i++).toString();
    r.title             = q.value(i++).toString();
    r.artistCredit      = q.value(i++).toString();
    r.albumArtist       = q.value(i++).toString();
    r.albumArtistMbid   = q.value(i++).toString();
    r.date              = q.value(i++).toString();
    r.originalDate      = q.value(i++).toString();
    r.country           = q.value(i++).toString();
    r.barcode           = q.value(i++).toString();
    r.catalogNumber     = q.value(i++).toString();
    r.label             = q.value(i++).toString();
    r.asin              = q.value(i++).toString();
    r.coverArtUrl       = q.value(i++).toString();
    r.discSubtitle      = q.value(i++).toString();
    r.discPosition      = q.value(i++).toInt();
    r.discTotalCount    = q.value(i++).toInt();
    r.mbDiscId          = q.value(i++).toString();
    r.sourceTag         = q.value(i++).toString();
    r.confidence        = q.value(i++).toInt();
    r.pickReason        = q.value(i++).toString();
    r.scoringLogJson    = q.value(i++).toString();
    r.pipelineLogJson   = q.value(i++).toString();
    r.rawMbJson         = q.value(i++).toString();
    r.pipelineVersion   = q.value(i++).toInt();
    r.cachedAt          = q.value(i++).toLongLong();
    return r;
}

int64_t LibraryDatabase::upsertRelease(const ReleaseRow& row) {
    if (row.releaseMbid.isEmpty()) return 0;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    // Upsert via ON CONFLICT DO UPDATE so a re-resolve refreshes the row.
    q.prepare(QStringLiteral(
        "INSERT INTO releases ("
        "  release_mbid, release_group_mbid, title, artist_credit, "
        "  album_artist, album_artist_mbid, date, original_date, "
        "  country, barcode, catalog_number, label, asin, "
        "  cover_art_url, disc_subtitle, disc_position, "
        "  disc_total_count, mb_disc_id, source_tag, confidence, "
        "  pick_reason, scoring_log_json, pipeline_log_json, "
        "  raw_mb_json, pipeline_version, cached_at"
        ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(release_mbid) DO UPDATE SET "
        "  release_group_mbid = excluded.release_group_mbid,"
        "  title              = excluded.title,"
        "  artist_credit      = excluded.artist_credit,"
        "  album_artist       = excluded.album_artist,"
        "  album_artist_mbid  = excluded.album_artist_mbid,"
        "  date               = excluded.date,"
        "  original_date      = excluded.original_date,"
        "  country            = excluded.country,"
        "  barcode            = excluded.barcode,"
        "  catalog_number     = excluded.catalog_number,"
        "  label              = excluded.label,"
        "  asin               = excluded.asin,"
        "  cover_art_url      = excluded.cover_art_url,"
        "  disc_subtitle      = excluded.disc_subtitle,"
        "  disc_position      = excluded.disc_position,"
        "  disc_total_count   = excluded.disc_total_count,"
        "  mb_disc_id         = excluded.mb_disc_id,"
        "  source_tag         = excluded.source_tag,"
        "  confidence         = excluded.confidence,"
        "  pick_reason        = excluded.pick_reason,"
        "  scoring_log_json   = excluded.scoring_log_json,"
        "  pipeline_log_json  = excluded.pipeline_log_json,"
        "  raw_mb_json        = excluded.raw_mb_json,"
        "  pipeline_version   = excluded.pipeline_version,"
        "  cached_at          = excluded.cached_at;"));
    q.addBindValue(row.releaseMbid);
    q.addBindValue(row.releaseGroupMbid);
    q.addBindValue(row.title);
    q.addBindValue(row.artistCredit);
    q.addBindValue(row.albumArtist);
    q.addBindValue(row.albumArtistMbid);
    q.addBindValue(row.date);
    q.addBindValue(row.originalDate);
    q.addBindValue(row.country);
    q.addBindValue(row.barcode);
    q.addBindValue(row.catalogNumber);
    q.addBindValue(row.label);
    q.addBindValue(row.asin);
    q.addBindValue(row.coverArtUrl);
    q.addBindValue(row.discSubtitle);
    q.addBindValue(row.discPosition);
    q.addBindValue(row.discTotalCount);
    q.addBindValue(row.mbDiscId);
    q.addBindValue(row.sourceTag);
    q.addBindValue(row.confidence);
    q.addBindValue(row.pickReason);
    q.addBindValue(row.scoringLogJson);
    q.addBindValue(row.pipelineLogJson);
    q.addBindValue(row.rawMbJson);
    q.addBindValue(row.pipelineVersion);
    q.addBindValue(row.cachedAt > 0 ? row.cachedAt
                                    : QDateTime::currentSecsSinceEpoch());
    if (!execOrLog(q, "upsert release")) return 0;
    auto rowid = findReleaseRowidByMbid(row.releaseMbid);
    return rowid.value_or(0);
}

int64_t LibraryDatabase::upsertAlbumMeta(
    const concerto::metadata::AlbumMeta& a,
    const QString& rawMbJson)
{
    if (a.releaseId.isEmpty()) return 0;
    ReleaseRow r;
    r.releaseMbid      = a.releaseId;
    r.releaseGroupMbid = a.releaseGroupId;
    r.title            = a.title;
    r.artistCredit     = a.artistCredit;
    r.albumArtist      = a.albumArtist;
    r.albumArtistMbid  = a.albumArtistId;
    r.date             = a.date;
    r.originalDate     = a.originalDate;
    r.country          = a.country;
    r.barcode          = a.barcode;
    r.catalogNumber    = a.catalogNumber;
    r.label            = a.label;
    r.asin             = a.asin;
    r.coverArtUrl      = a.coverArtUrl;
    r.discSubtitle     = a.discSubtitle;
    r.discPosition     = a.discPosition;
    r.discTotalCount   = a.discTotalCount;
    r.mbDiscId         = a.mbDiscId;
    r.sourceTag        = a.sourceTag;
    r.confidence       = a.confidence;
    r.pickReason       = a.pickReason;
    r.scoringLogJson   = jsonFromVariantList(a.scoringLog);
    r.pipelineLogJson  = jsonFromVariantList(a.pipelineLog);
    r.rawMbJson        = rawMbJson;
    r.pipelineVersion  = kConcertoPipelineVersion;
    r.cachedAt         = QDateTime::currentSecsSinceEpoch();

    const int64_t rowid = upsertRelease(r);
    if (rowid == 0) return 0;

    // Per-track rows.
    QVector<TrackRow> tracks;
    tracks.reserve(a.tracks.size());
    for (const auto& t : a.tracks) {
        TrackRow tr;
        tr.releaseMbid    = a.releaseId;
        tr.mediumPosition = a.discPosition;
        tr.trackPosition  = t.position;
        tr.recordingMbid  = t.recordingId;
        tr.title          = t.title;
        tr.workMbid       = t.workId;
        tr.movementName   = t.movementName;
        tr.movementNumber = t.movementNumber;
        tr.movementTotal  = t.movementTotal;
        tr.durationMs     = t.durationMs;
        tr.isrc           = t.isrc;
        tracks.append(tr);
    }
    upsertTracksForRelease(a.releaseId, tracks);
    return rowid;
}

int LibraryDatabase::upsertTracksForRelease(
    const QString& releaseMbid,
    const QVector<TrackRow>& tracks)
{
    if (releaseMbid.isEmpty() || tracks.isEmpty()) return 0;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    db.transaction();
    int count = 0;
    for (const TrackRow& t : tracks) {
        QSqlQuery q(db);
        q.prepare(QStringLiteral(
            "INSERT INTO tracks ("
            "  release_mbid, medium_position, track_position, "
            "  recording_mbid, title, work_mbid, movement_name, "
            "  movement_number, movement_total, duration_ms, isrc"
            ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(release_mbid, medium_position, track_position) DO UPDATE SET "
            "  recording_mbid  = excluded.recording_mbid,"
            "  title           = excluded.title,"
            "  work_mbid       = excluded.work_mbid,"
            "  movement_name   = excluded.movement_name,"
            "  movement_number = excluded.movement_number,"
            "  movement_total  = excluded.movement_total,"
            "  duration_ms     = excluded.duration_ms,"
            "  isrc            = excluded.isrc;"));
        q.addBindValue(t.releaseMbid);
        q.addBindValue(t.mediumPosition);
        q.addBindValue(t.trackPosition);
        q.addBindValue(t.recordingMbid);
        q.addBindValue(t.title);
        q.addBindValue(t.workMbid);
        q.addBindValue(t.movementName);
        q.addBindValue(t.movementNumber);
        q.addBindValue(t.movementTotal);
        q.addBindValue(static_cast<qlonglong>(t.durationMs));
        q.addBindValue(t.isrc);
        if (q.exec()) ++count;
    }
    db.commit();
    return count;
}

// ---------- files ----------

namespace {

FileRow fileRowFromQuery(QSqlQuery& q) {
    FileRow f;
    int i = 0;
    f.contentHash    = q.value(i++).toString();
    f.path           = q.value(i++).toString();
    f.format         = q.value(i++).toString();
    f.durationSec    = q.value(i++).toDouble();
    f.bitDepth       = q.value(i++).toInt();
    f.sampleRate     = q.value(i++).toInt();
    f.channelCount   = q.value(i++).toInt();
    f.sizeBytes      = q.value(i++).toLongLong();
    f.firstSeenTs    = q.value(i++).toLongLong();
    f.lastSeenTs     = q.value(i++).toLongLong();
    f.releaseMbid    = q.value(i++).toString();
    f.trackPosition  = q.value(i++).toInt();
    f.mediumPosition = q.value(i++).toInt();
    f.writebackAt    = q.value(i++).toLongLong();
    return f;
}

constexpr const char* kFileCols =
    "content_hash, path, format, duration_sec, bit_depth, sample_rate, "
    "channel_count, size_bytes, first_seen_ts, last_seen_ts, "
    "release_mbid, track_position, medium_position, writeback_at";

} // namespace

std::optional<FileRow> LibraryDatabase::findFileByHash(
    const QString& contentHash) const
{
    if (contentHash.isEmpty()) return std::nullopt;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT %1 FROM files WHERE content_hash = ? LIMIT 1;")
              .arg(QString::fromLatin1(kFileCols)));
    q.addBindValue(contentHash);
    if (!q.exec() || !q.next()) return std::nullopt;
    return fileRowFromQuery(q);
}

std::optional<FileRow> LibraryDatabase::findFileByPath(
    const QString& path) const
{
    if (path.isEmpty()) return std::nullopt;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT %1 FROM files WHERE path = ? LIMIT 1;")
              .arg(QString::fromLatin1(kFileCols)));
    q.addBindValue(path);
    if (!q.exec() || !q.next()) return std::nullopt;
    return fileRowFromQuery(q);
}

int64_t LibraryDatabase::upsertFile(const FileRow& row) {
    if (row.contentHash.isEmpty()) return 0;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO files ("
        "  content_hash, path, format, duration_sec, bit_depth, "
        "  sample_rate, channel_count, size_bytes, first_seen_ts, "
        "  last_seen_ts, release_mbid, track_position, medium_position, "
        "  writeback_at"
        ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(content_hash) DO UPDATE SET "
        "  path             = excluded.path,"
        "  format           = excluded.format,"
        "  duration_sec     = excluded.duration_sec,"
        "  bit_depth        = COALESCE(excluded.bit_depth, files.bit_depth),"
        "  sample_rate      = COALESCE(excluded.sample_rate, files.sample_rate),"
        "  channel_count    = COALESCE(excluded.channel_count, files.channel_count),"
        "  size_bytes       = excluded.size_bytes,"
        "  last_seen_ts     = excluded.last_seen_ts,"
        "  release_mbid     = COALESCE(excluded.release_mbid, files.release_mbid),"
        "  track_position   = COALESCE(excluded.track_position, files.track_position),"
        "  medium_position  = COALESCE(excluded.medium_position, files.medium_position),"
        "  writeback_at     = COALESCE(excluded.writeback_at, files.writeback_at);"));

    const qint64 now = QDateTime::currentSecsSinceEpoch();
    q.addBindValue(row.contentHash);
    q.addBindValue(row.path);
    q.addBindValue(row.format);
    q.addBindValue(row.durationSec);
    q.addBindValue(row.bitDepth   ? QVariant(row.bitDepth)   : QVariant());
    q.addBindValue(row.sampleRate ? QVariant(row.sampleRate) : QVariant());
    q.addBindValue(row.channelCount ? QVariant(row.channelCount) : QVariant());
    q.addBindValue(static_cast<qlonglong>(row.sizeBytes));
    q.addBindValue(static_cast<qlonglong>(row.firstSeenTs ? row.firstSeenTs : now));
    q.addBindValue(static_cast<qlonglong>(row.lastSeenTs ? row.lastSeenTs : now));
    q.addBindValue(row.releaseMbid.isEmpty() ? QVariant() : QVariant(row.releaseMbid));
    q.addBindValue(row.trackPosition  > 0 ? QVariant(row.trackPosition)  : QVariant());
    q.addBindValue(row.mediumPosition > 0 ? QVariant(row.mediumPosition) : QVariant());
    q.addBindValue(row.writebackAt > 0
                       ? QVariant(static_cast<qlonglong>(row.writebackAt))
                       : QVariant());
    if (!execOrLog(q, "upsert file")) return 0;
    return 1;
}

bool LibraryDatabase::isFileTrusted(const QString& contentHash,
                                    int requiredVersion) const
{
    if (contentHash.isEmpty()) return false;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT r.pipeline_version FROM files f "
        "JOIN releases r ON r.release_mbid = f.release_mbid "
        "WHERE f.content_hash = ? LIMIT 1;"));
    q.addBindValue(contentHash);
    if (!q.exec() || !q.next()) return false;
    return q.value(0).toInt() >= requiredVersion;
}

// ---------- folders ----------

std::optional<FolderRow> LibraryDatabase::findFolderByPath(
    const QString& path) const
{
    if (path.isEmpty()) return std::nullopt;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT id, path, last_identify_source, release_mbid, "
        "       last_scanned_ts, file_count, is_locked "
        "FROM folders WHERE path = ? LIMIT 1;"));
    q.addBindValue(path);
    if (!q.exec() || !q.next()) return std::nullopt;
    FolderRow r;
    r.id                 = q.value(0).toLongLong();
    r.path               = q.value(1).toString();
    r.lastIdentifySource = q.value(2).toString();
    r.releaseMbid        = q.value(3).toString();
    r.lastScannedTs      = q.value(4).toLongLong();
    r.fileCount          = q.value(5).toInt();
    r.isLocked           = q.value(6).toBool();
    return r;
}

int64_t LibraryDatabase::upsertFolder(const FolderRow& row) {
    if (row.path.isEmpty()) return 0;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO folders ("
        "  path, last_identify_source, release_mbid, "
        "  last_scanned_ts, file_count, is_locked"
        ") VALUES (?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(path) DO UPDATE SET "
        "  last_identify_source = excluded.last_identify_source,"
        "  release_mbid         = excluded.release_mbid,"
        "  last_scanned_ts      = excluded.last_scanned_ts,"
        "  file_count           = excluded.file_count,"
        "  is_locked            = excluded.is_locked;"));
    q.addBindValue(row.path);
    q.addBindValue(row.lastIdentifySource);
    q.addBindValue(row.releaseMbid.isEmpty() ? QVariant() : QVariant(row.releaseMbid));
    q.addBindValue(static_cast<qlonglong>(
        row.lastScannedTs ? row.lastScannedTs : QDateTime::currentSecsSinceEpoch()));
    q.addBindValue(row.fileCount);
    q.addBindValue(row.isLocked ? 1 : 0);
    if (!execOrLog(q, "upsert folder")) return 0;
    auto f = findFolderByPath(row.path);
    return f ? f->id : 0;
}

} // namespace concerto::library
