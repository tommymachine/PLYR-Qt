#include "MetadataCache.h"

#include "SystemPaths.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QSaveFile>

namespace concerto::metadata {

namespace {

QJsonObject performerToJson(const Performer& p) {
    QJsonObject o;
    o.insert(QStringLiteral("name"), p.name);
    if (!p.sortName.isEmpty())     o.insert(QStringLiteral("sortName"),    p.sortName);
    if (!p.role.isEmpty())         o.insert(QStringLiteral("role"),        p.role);
    if (!p.mbArtistId.isEmpty())   o.insert(QStringLiteral("mbArtistId"),  p.mbArtistId);
    if (!p.attrs.isEmpty())
        o.insert(QStringLiteral("attrs"),
                 QJsonArray::fromStringList(p.attrs));
    return o;
}

Performer performerFromJson(const QJsonObject& o) {
    Performer p;
    p.name        = o.value(QStringLiteral("name")).toString();
    p.sortName    = o.value(QStringLiteral("sortName")).toString();
    p.role        = o.value(QStringLiteral("role")).toString();
    p.mbArtistId  = o.value(QStringLiteral("mbArtistId")).toString();
    for (const QJsonValue& v : o.value(QStringLiteral("attrs")).toArray())
        p.attrs << v.toString();
    return p;
}

QJsonObject trackToJson(const TrackMeta& t) {
    QJsonObject o;
    o.insert(QStringLiteral("position"),       t.position);
    o.insert(QStringLiteral("title"),          t.title);
    if (!t.workTitle.isEmpty())     o.insert(QStringLiteral("workTitle"),      t.workTitle);
    if (!t.movementName.isEmpty())  o.insert(QStringLiteral("movementName"),   t.movementName);
    if (t.movementNumber > 0)       o.insert(QStringLiteral("movementNumber"), t.movementNumber);
    if (t.movementTotal  > 0)       o.insert(QStringLiteral("movementTotal"),  t.movementTotal);
    if (t.durationMs > 0)           o.insert(QStringLiteral("durationMs"),     double(t.durationMs));
    if (!t.recordingId.isEmpty())   o.insert(QStringLiteral("recordingId"),    t.recordingId);
    if (!t.workId.isEmpty())        o.insert(QStringLiteral("workId"),         t.workId);
    if (!t.composerId.isEmpty())    o.insert(QStringLiteral("composerId"),     t.composerId);
    if (!t.composerName.isEmpty())  o.insert(QStringLiteral("composerName"),   t.composerName);
    if (!t.composerSort.isEmpty())  o.insert(QStringLiteral("composerSort"),   t.composerSort);
    if (!t.isrc.isEmpty())          o.insert(QStringLiteral("isrc"),           t.isrc);
    QJsonArray performers;
    for (const Performer& p : t.performers) performers.append(performerToJson(p));
    if (!performers.isEmpty()) o.insert(QStringLiteral("performers"), performers);
    return o;
}

TrackMeta trackFromJson(const QJsonObject& o) {
    TrackMeta t;
    t.position       = o.value(QStringLiteral("position")).toInt();
    t.title          = o.value(QStringLiteral("title")).toString();
    t.workTitle      = o.value(QStringLiteral("workTitle")).toString();
    t.movementName   = o.value(QStringLiteral("movementName")).toString();
    t.movementNumber = o.value(QStringLiteral("movementNumber")).toInt();
    t.movementTotal  = o.value(QStringLiteral("movementTotal")).toInt();
    t.durationMs     = static_cast<qint64>(o.value(QStringLiteral("durationMs")).toDouble());
    t.recordingId    = o.value(QStringLiteral("recordingId")).toString();
    t.workId         = o.value(QStringLiteral("workId")).toString();
    t.composerId     = o.value(QStringLiteral("composerId")).toString();
    t.composerName   = o.value(QStringLiteral("composerName")).toString();
    t.composerSort   = o.value(QStringLiteral("composerSort")).toString();
    t.isrc           = o.value(QStringLiteral("isrc")).toString();
    for (const QJsonValue& v : o.value(QStringLiteral("performers")).toArray())
        t.performers.append(performerFromJson(v.toObject()));
    return t;
}

QJsonObject albumToJson(const AlbumMeta& a) {
    QJsonObject o;
    o.insert(QStringLiteral("releaseId"),       a.releaseId);
    o.insert(QStringLiteral("releaseGroupId"),  a.releaseGroupId);
    o.insert(QStringLiteral("title"),           a.title);
    o.insert(QStringLiteral("artistCredit"),    a.artistCredit);
    o.insert(QStringLiteral("albumArtist"),     a.albumArtist);
    o.insert(QStringLiteral("albumArtistId"),   a.albumArtistId);
    o.insert(QStringLiteral("date"),            a.date);
    o.insert(QStringLiteral("originalDate"),    a.originalDate);
    o.insert(QStringLiteral("country"),         a.country);
    o.insert(QStringLiteral("barcode"),         a.barcode);
    o.insert(QStringLiteral("catalogNumber"),   a.catalogNumber);
    o.insert(QStringLiteral("label"),           a.label);
    o.insert(QStringLiteral("asin"),            a.asin);
    o.insert(QStringLiteral("discPosition"),    a.discPosition);
    o.insert(QStringLiteral("discTotalCount"),  a.discTotalCount);
    o.insert(QStringLiteral("discSubtitle"),    a.discSubtitle);
    o.insert(QStringLiteral("mbDiscId"),        a.mbDiscId);
    o.insert(QStringLiteral("coverArtUrl"),     a.coverArtUrl);
    if (!a.genreNames.isEmpty())
        o.insert(QStringLiteral("genreNames"),
                 QJsonArray::fromStringList(a.genreNames));
    o.insert(QStringLiteral("confidence"),      a.confidence);
    o.insert(QStringLiteral("sourceTag"),       a.sourceTag);
    o.insert(QStringLiteral("pickReason"),      a.pickReason);
    QJsonArray tracks;
    for (const TrackMeta& t : a.tracks) tracks.append(trackToJson(t));
    o.insert(QStringLiteral("tracks"), tracks);
    return o;
}

AlbumMeta albumFromJson(const QJsonObject& o) {
    AlbumMeta a;
    a.releaseId       = o.value(QStringLiteral("releaseId")).toString();
    a.releaseGroupId  = o.value(QStringLiteral("releaseGroupId")).toString();
    a.title           = o.value(QStringLiteral("title")).toString();
    a.artistCredit    = o.value(QStringLiteral("artistCredit")).toString();
    a.albumArtist     = o.value(QStringLiteral("albumArtist")).toString();
    a.albumArtistId   = o.value(QStringLiteral("albumArtistId")).toString();
    a.date            = o.value(QStringLiteral("date")).toString();
    a.originalDate    = o.value(QStringLiteral("originalDate")).toString();
    a.country         = o.value(QStringLiteral("country")).toString();
    a.barcode         = o.value(QStringLiteral("barcode")).toString();
    a.catalogNumber   = o.value(QStringLiteral("catalogNumber")).toString();
    a.label           = o.value(QStringLiteral("label")).toString();
    a.asin            = o.value(QStringLiteral("asin")).toString();
    a.discPosition    = o.value(QStringLiteral("discPosition")).toInt(1);
    a.discTotalCount  = o.value(QStringLiteral("discTotalCount")).toInt(1);
    a.discSubtitle    = o.value(QStringLiteral("discSubtitle")).toString();
    a.mbDiscId        = o.value(QStringLiteral("mbDiscId")).toString();
    a.coverArtUrl     = o.value(QStringLiteral("coverArtUrl")).toString();
    for (const QJsonValue& v : o.value(QStringLiteral("genreNames")).toArray())
        a.genreNames << v.toString();
    a.confidence      = o.value(QStringLiteral("confidence")).toInt();
    a.sourceTag       = o.value(QStringLiteral("sourceTag")).toString();
    a.pickReason      = o.value(QStringLiteral("pickReason")).toString();
    for (const QJsonValue& v : o.value(QStringLiteral("tracks")).toArray())
        a.tracks.append(trackFromJson(v.toObject()));
    return a;
}

QString discFilePath(const QString& root, const QString& discId) {
    // disc-IDs are URL-safe base64 by construction, so they're fine for
    // a flat filename. Belt-and-braces sanitize anyway.
    QString safe = discId;
    safe.replace(QLatin1Char('/'), QLatin1Char('_'));
    return root + QStringLiteral("/disc/") + safe + QStringLiteral(".json");
}

QString releaseAliasPath(const QString& root,
                         const QString& releaseMbid,
                         int mediumPosition) {
    return root + QStringLiteral("/release/")
         + releaseMbid + QStringLiteral("__")
         + QString::number(mediumPosition)
         + QStringLiteral(".json");
}

std::optional<MetadataCache::Entry> loadEntry(const QString& path, int maxAgeDays) {
    QFile f(path);
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) return std::nullopt;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) return std::nullopt;
    const QJsonObject root = doc.object();

    if (maxAgeDays > 0) {
        const QDateTime cached = QDateTime::fromString(
            root.value(QStringLiteral("cachedAt")).toString(), Qt::ISODate);
        if (cached.isValid()) {
            const QDateTime cutoff = QDateTime::currentDateTimeUtc()
                                         .addDays(-maxAgeDays);
            if (cached < cutoff) return std::nullopt;
        }
    }

    MetadataCache::Entry e;
    e.album   = albumFromJson(root.value(QStringLiteral("albumMeta")).toObject());
    e.rawMb   = root.value(QStringLiteral("rawMb")).toObject();
    e.cachedAt = root.value(QStringLiteral("cachedAt")).toString();
    // Rehydrate the scoringLog / pipelineLog onto the AlbumMeta so callers
    // see the same shape the resolver emits.
    for (const QJsonValue& v : root.value(QStringLiteral("scoringLog")).toArray())
        e.album.scoringLog.append(v.toVariant());
    for (const QJsonValue& v : root.value(QStringLiteral("pipelineLog")).toArray())
        e.album.pipelineLog.append(v.toVariant());
    return e;
}

} // namespace

QString MetadataCache::defaultDir() {
    // Path is `<appDataDir>/metadata-cache` — appDataDir() reads the
    // running app's applicationName(), so a product rename is a
    // one-line change in main.cpp rather than every cache module.
    return concerto::paths::appDataDir() + QStringLiteral("/metadata-cache");
}

MetadataCache::MetadataCache(QString dir) : m_dir(std::move(dir)) {}

std::optional<MetadataCache::Entry> MetadataCache::getByDiscId(
    const QString& mbDiscId, int maxAgeDays) const
{
    if (mbDiscId.isEmpty()) return std::nullopt;
    return loadEntry(discFilePath(m_dir, mbDiscId), maxAgeDays);
}

std::optional<MetadataCache::Entry> MetadataCache::getByRelease(
    const QString& releaseMbid, int mediumPosition, int maxAgeDays) const
{
    if (releaseMbid.isEmpty()) return std::nullopt;
    return loadEntry(releaseAliasPath(m_dir, releaseMbid, mediumPosition),
                     maxAgeDays);
}

void MetadataCache::put(const QString& mbDiscId,
                        const AlbumMeta& album,
                        const QJsonObject& rawMb)
{
    if (mbDiscId.isEmpty()) return;

    QDir().mkpath(m_dir + QStringLiteral("/disc"));
    QDir().mkpath(m_dir + QStringLiteral("/release"));

    QJsonObject root;
    root.insert(QStringLiteral("albumMeta"), albumToJson(album));
    root.insert(QStringLiteral("rawMb"),     rawMb);
    QJsonArray scoring;
    for (const QVariant& v : album.scoringLog)
        scoring.append(QJsonValue::fromVariant(v));
    root.insert(QStringLiteral("scoringLog"), scoring);
    QJsonArray pipeline;
    for (const QVariant& v : album.pipelineLog)
        pipeline.append(QJsonValue::fromVariant(v));
    root.insert(QStringLiteral("pipelineLog"), pipeline);
    root.insert(QStringLiteral("cachedAt"),
                QDateTime::currentDateTimeUtc().toString(Qt::ISODate));

    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);

    {
        QSaveFile f(discFilePath(m_dir, mbDiscId));
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(bytes);
            f.commit();
        }
    }
    if (!album.releaseId.isEmpty()) {
        QSaveFile f(releaseAliasPath(m_dir, album.releaseId, album.discPosition));
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(bytes);
            f.commit();
        }
    }
}

void MetadataCache::invalidate(const QString& mbDiscId) {
    QFile::remove(discFilePath(m_dir, mbDiscId));
}

} // namespace concerto::metadata
