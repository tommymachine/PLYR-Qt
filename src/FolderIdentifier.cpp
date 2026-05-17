#include "FolderIdentifier.h"

#include "AudioFrameHash.h"
#include "AudioTagIo.h"
#include "LibraryDatabase.h"
#include "MetadataScoring.h"

#include <QDebug>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace concerto::library {

namespace {

constexpr const char* kKeyAlbumId   = "MUSICBRAINZ_ALBUMID";
constexpr const char* kKeyDiscId    = "MUSICBRAINZ_DISCID";
constexpr const char* kKeyVersion   = "CONCERTO_PIPELINE_VERSION";

// UUID-shape: standard 8-4-4-4-12 hex with dashes. MB MBIDs are
// canonical UUIDs, so we filter junk tag values out before honoring
// them as MBIDs. Compile once, reuse across all calls.
const QRegularExpression& uuidRe() {
    static const QRegularExpression re(
        QStringLiteral("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-"
                       "[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"));
    return re;
}

bool isUuidShaped(const QString& s) {
    return uuidRe().match(s).hasMatch();
}

// Count occurrences of (key → value) across a list of tag-maps. The
// returned hash is value → count.
QHash<QString, int> countTag(
    const std::vector<std::map<QString, QStringList>>& bundles,
    const QString& key)
{
    QHash<QString, int> counts;
    for (const auto& m : bundles) {
        const auto it = m.find(key);
        if (it == m.end() || it->second.isEmpty()) continue;
        const QString v = it->second.first().trimmed();
        if (v.isEmpty()) continue;
        ++counts[v];
    }
    return counts;
}

// Returns the value with the most occurrences if its share is >= quorum.
// Empty string on no quorum.
QString quorumValue(const QHash<QString, int>& counts,
                    int totalFiles,
                    double quorum)
{
    if (totalFiles <= 0) return {};
    QString best;
    int bestCount = 0;
    for (auto it = counts.constBegin(); it != counts.constEnd(); ++it) {
        if (it.value() > bestCount) {
            bestCount = it.value();
            best = it.key();
        }
    }
    const double share = double(bestCount) / double(totalFiles);
    return share >= quorum ? best : QString();
}

} // namespace

FolderIdentifier::FolderIdentifier(
    musicbrainz::Client* mb,
    concerto::metadata::MetadataCache* cache,
    LibraryDatabase* db,
    QObject* parent)
    : QObject(parent), m_mb(mb), m_cache(cache), m_db(db)
{
    if (m_mb) {
        connect(m_mb, &musicbrainz::Client::releaseResolved,
                this, &FolderIdentifier::onReleaseResolved);
        connect(m_mb, &musicbrainz::Client::releaseFailed,
                this, &FolderIdentifier::onReleaseFailed);
        connect(m_mb, &musicbrainz::Client::discIdResolved,
                this, &FolderIdentifier::onDiscIdResolved);
        connect(m_mb, &musicbrainz::Client::discIdFailed,
                this, &FolderIdentifier::onDiscIdFailed);
    }
}

void FolderIdentifier::identify(const Request& req)
{
    m_req = req;
    m_result = Result{};
    m_result.folderPath = req.folderPath;
    m_result.filePaths  = req.filePaths;
    m_stageInflight.clear();
    m_inflightTrackCount = req.filePaths.size();

    if (req.filePaths.isEmpty()) {
        m_result.identified = false;
        m_result.source = QStringLiteral("unidentified");
        appendDiag(QStringLiteral("no audio files in folder"));
        emitIdentified(m_result);
        return;
    }
    appendDiag(QStringLiteral("scanning %1 file(s)").arg(req.filePaths.size()));

    // Stage Z — trust-marker fast path. Reads tags for every file once;
    // these tag-maps are reused by Stages A and B.
    std::vector<std::map<QString, QStringList>> bundles;
    bundles.reserve(req.filePaths.size());
    for (const QString& p : req.filePaths) {
        auto t = AudioTagIo::read(p);
        bundles.push_back(t ? *t : std::map<QString, QStringList>{});
    }

    // In-file marker quorum.
    int withMarker = 0;
    QHash<QString, int> markerAlbumCounts;
    for (const auto& m : bundles) {
        const auto vit = m.find(QString::fromLatin1(kKeyVersion));
        const auto aid = m.find(QString::fromLatin1(kKeyAlbumId));
        if (vit == m.end() || vit->second.isEmpty()) continue;
        bool ok = false;
        const int v = vit->second.first().toInt(&ok);
        if (!ok || v < kConcertoPipelineVersion) continue;
        if (aid == m.end() || aid->second.isEmpty()) continue;
        const QString id = aid->second.first().trimmed();
        if (!isUuidShaped(id)) continue;
        ++withMarker;
        ++markerAlbumCounts[id];
    }
    const double markerShare =
        double(withMarker) / double(req.filePaths.size());
    if (markerShare >= kMarkerTrustQuorum) {
        const QString id = quorumValue(markerAlbumCounts,
                                       req.filePaths.size(),
                                       kMarkerTrustQuorum);
        if (!id.isEmpty()) {
            appendDiag(QStringLiteral(
                "Stage Z (in-file marker): version+albumid quorum %1/%2 → %3")
                .arg(withMarker).arg(req.filePaths.size()).arg(id));

            // Try cache first — same release MBID may already be cached
            // from an earlier rip-pipeline pass. Costs zero web.
            if (m_cache) {
                auto hit = m_cache->getByRelease(id, /*mediumPosition=*/1);
                if (hit) {
                    m_result.album = hit->album;
                    m_result.album.releaseId = id;
                    m_result.identified = true;
                    m_result.source = QStringLiteral("marker-trust");
                    appendDiag(QStringLiteral(
                        "Stage Z: cache hit by release MBID — no web"));
                    emitIdentified(m_result);
                    return;
                }
            }

            // No cache hit, but we trust the file's tags. Emit a
            // minimally-populated AlbumMeta with what's in the tags.
            // Stage Z is the "skip web entirely" path; the consumer
            // can decide to fetch later if it wants richer fields.
            concerto::metadata::AlbumMeta a;
            a.releaseId    = id;
            a.title        = AudioTagIo::readField(bundles.front(), QStringLiteral("ALBUM"));
            a.artistCredit = AudioTagIo::readField(bundles.front(), QStringLiteral("ARTIST"));
            a.albumArtist  = AudioTagIo::readField(bundles.front(),
                                                   QStringLiteral("ALBUMARTIST"),
                                                   a.artistCredit);
            a.date         = AudioTagIo::readField(bundles.front(), QStringLiteral("DATE"));
            a.barcode      = AudioTagIo::readField(bundles.front(), QStringLiteral("BARCODE"));
            a.catalogNumber = AudioTagIo::readField(bundles.front(), QStringLiteral("CATALOGNUMBER"));
            a.label        = AudioTagIo::readField(bundles.front(), QStringLiteral("LABEL"));
            a.mbDiscId     = AudioTagIo::readField(bundles.front(), QStringLiteral("MUSICBRAINZ_DISCID"));
            a.confidence   = 100;
            a.sourceTag    = QStringLiteral("marker-trust");

            // Reconstruct per-track rows from the tags we just read.
            int trackPos = 1;
            for (size_t i = 0; i < bundles.size(); ++i) {
                concerto::metadata::TrackMeta t;
                t.position    = AudioTagIo::readField(bundles[i], QStringLiteral("TRACKNUMBER"),
                                                      QString::number(trackPos)).toInt();
                if (t.position <= 0) t.position = trackPos;
                t.title       = AudioTagIo::readField(bundles[i], QStringLiteral("TITLE"));
                t.workTitle   = AudioTagIo::readField(bundles[i], QStringLiteral("WORK"));
                t.movementName= AudioTagIo::readField(bundles[i], QStringLiteral("MOVEMENTNAME"));
                t.movementNumber = AudioTagIo::readField(bundles[i], QStringLiteral("MOVEMENT")).toInt();
                t.movementTotal  = AudioTagIo::readField(bundles[i], QStringLiteral("MOVEMENTTOTAL")).toInt();
                t.composerName= AudioTagIo::readField(bundles[i], QStringLiteral("COMPOSER"));
                t.composerSort= AudioTagIo::readField(bundles[i], QStringLiteral("COMPOSERSORT"));
                t.recordingId = AudioTagIo::readField(bundles[i], QStringLiteral("MUSICBRAINZ_TRACKID"));
                t.workId      = AudioTagIo::readField(bundles[i], QStringLiteral("MUSICBRAINZ_WORKID"));
                t.composerId  = AudioTagIo::readField(bundles[i], QStringLiteral("MUSICBRAINZ_ARTISTID"));
                t.isrc        = AudioTagIo::readField(bundles[i], QStringLiteral("ISRC"));
                a.tracks.append(t);
                ++trackPos;
            }
            m_result.album = std::move(a);
            m_result.identified = true;
            m_result.source = QStringLiteral("marker-trust");
            emitIdentified(m_result);
            return;
        }
    }

    // DB-as-authority Stage Z (plan §A.4) — check the audio-frame
    // content_hash for each file. If a quorum of files have rows in
    // `files` linked to a release whose pipeline_version is high enough,
    // trust the DB. This is the writeback-off case.
    if (m_db && m_db->isOpen()) {
        int dbTrusted = 0;
        QHash<QString, int> dbReleaseCounts;
        for (const QString& p : req.filePaths) {
            // The DB row is keyed by content_hash. Skip the hash
            // computation entirely if there's no row by *path* — the
            // hash is expensive, the path lookup is fast.
            auto byPath = m_db->findFileByPath(p);
            if (!byPath || byPath->releaseMbid.isEmpty()) continue;
            if (m_db->isFileTrusted(byPath->contentHash)) {
                ++dbTrusted;
                ++dbReleaseCounts[byPath->releaseMbid];
            }
        }
        if (dbTrusted >= req.filePaths.size() * kMarkerTrustQuorum) {
            const QString id = quorumValue(dbReleaseCounts,
                                           req.filePaths.size(),
                                           kMarkerTrustQuorum);
            if (!id.isEmpty()) {
                if (auto r = m_db->findReleaseByMbid(id); r) {
                    appendDiag(QStringLiteral(
                        "Stage Z (DB-as-authority): %1/%2 files trusted → %3")
                        .arg(dbTrusted).arg(req.filePaths.size()).arg(id));
                    concerto::metadata::AlbumMeta a;
                    a.releaseId      = r->releaseMbid;
                    a.releaseGroupId = r->releaseGroupMbid;
                    a.title          = r->title;
                    a.artistCredit   = r->artistCredit;
                    a.albumArtist    = r->albumArtist;
                    a.albumArtistId  = r->albumArtistMbid;
                    a.date           = r->date;
                    a.originalDate   = r->originalDate;
                    a.country        = r->country;
                    a.barcode        = r->barcode;
                    a.catalogNumber  = r->catalogNumber;
                    a.label          = r->label;
                    a.asin           = r->asin;
                    a.coverArtUrl    = r->coverArtUrl;
                    a.discSubtitle   = r->discSubtitle;
                    a.discPosition   = r->discPosition;
                    a.discTotalCount = r->discTotalCount;
                    a.mbDiscId       = r->mbDiscId;
                    a.confidence     = 100;
                    a.sourceTag      = QStringLiteral("marker-trust");
                    // Try to reload the cached AlbumMeta with full
                    // track metadata. The DB row by itself only carries
                    // album-level fields; we want tracks too. The
                    // rip-time cache may still have it under the
                    // release-MBID alias.
                    if (m_cache) {
                        if (auto hit = m_cache->getByRelease(id, /*medium*/1); hit) {
                            a.tracks = hit->album.tracks;
                        }
                    }
                    m_result.album   = std::move(a);
                    m_result.identified = true;
                    m_result.source = QStringLiteral("marker-trust");
                    emitIdentified(m_result);
                    return;
                }
            }
        }
    }

    appendDiag(QStringLiteral(
        "Stage Z miss (marker quorum %1/%2; below %3%)")
        .arg(withMarker).arg(req.filePaths.size())
        .arg(int(kMarkerTrustQuorum * 100)));

    // Stage A — MUSICBRAINZ_ALBUMID quorum.
    const auto albumCounts = countTag(bundles, QString::fromLatin1(kKeyAlbumId));
    QString albumMbid = quorumValue(albumCounts,
                                    req.filePaths.size(),
                                    kAlbumIdQuorum);
    if (!albumMbid.isEmpty() && !isUuidShaped(albumMbid)) albumMbid.clear();
    if (!albumMbid.isEmpty()) {
        appendDiag(QStringLiteral(
            "Stage A: MBID quorum %1 / %2 files → %3")
            .arg(albumCounts.value(albumMbid))
            .arg(req.filePaths.size())
            .arg(albumMbid));
        // Cache check before issuing the web fetch — the rip pipeline
        // may already have this release cached.
        if (m_cache) {
            auto hit = m_cache->getByRelease(albumMbid, 1);
            if (hit) {
                m_result.album = hit->album;
                m_result.album.releaseId = albumMbid;
                m_result.identified = true;
                m_result.source = QStringLiteral("mb-id-quorum");
                appendDiag(QStringLiteral("Stage A: cache hit for release"));
                emitIdentified(m_result);
                return;
            }
        }
        if (m_mb) {
            m_stageInflight = QStringLiteral("stageA-mb");
            appendDiag(QStringLiteral("Stage A: cache miss → MB fetchRelease"));
            m_mb->fetchRelease(albumMbid);
            return;
        }
        appendDiag(QStringLiteral("Stage A: no MB client; cannot fetch"));
    } else {
        appendDiag(QStringLiteral("Stage A miss: no MBID quorum"));
    }

    // Stage B — MUSICBRAINZ_DISCID quorum (cache then MB).
    const auto discCounts = countTag(bundles, QString::fromLatin1(kKeyDiscId));
    const QString discId = quorumValue(discCounts,
                                       req.filePaths.size(),
                                       kDiscIdQuorum);
    if (!discId.isEmpty()) {
        appendDiag(QStringLiteral(
            "Stage B: disc-ID quorum %1 / %2 → %3")
            .arg(discCounts.value(discId))
            .arg(req.filePaths.size())
            .arg(discId));
        if (m_cache) {
            auto hit = m_cache->getByDiscId(discId);
            if (hit) {
                m_result.album = hit->album;
                m_result.identified = true;
                m_result.source = QStringLiteral("mb-discid");
                appendDiag(QStringLiteral("Stage B: cache hit by disc-ID"));
                emitIdentified(m_result);
                return;
            }
        }
        if (m_mb) {
            m_stageInflight = QStringLiteral("stageB-mb-discid");
            appendDiag(QStringLiteral("Stage B: cache miss → MB fetchDiscId"));
            m_mb->fetchDiscId(discId);
            return;
        }
        appendDiag(QStringLiteral("Stage B: no MB client; cannot fetch"));
    } else {
        appendDiag(QStringLiteral("Stage B miss: no disc-ID quorum"));
    }

    // Stages C and D are stubs at this scope. Fall through to
    // unidentified.
    appendDiag(QStringLiteral("Stage C (search): stub — not implemented"));
    appendDiag(QStringLiteral("Stage D (fingerprint): stub — not implemented"));
    m_result.identified = false;
    m_result.source = QStringLiteral("unidentified");
    emitIdentified(m_result);
}

QString FolderIdentifier::stageAQuorumMbid(const QStringList& files) {
    // Kept for callers that want to introspect without firing the
    // async fetch. Not used internally; identify() inlines the tag
    // bundle read for efficiency.
    std::vector<std::map<QString, QStringList>> bundles;
    bundles.reserve(files.size());
    for (const QString& p : files) {
        auto t = AudioTagIo::read(p);
        bundles.push_back(t ? *t : std::map<QString, QStringList>{});
    }
    const auto counts = countTag(bundles, QString::fromLatin1(kKeyAlbumId));
    QString v = quorumValue(counts, files.size(), kAlbumIdQuorum);
    if (!isUuidShaped(v)) v.clear();
    return v;
}

QString FolderIdentifier::stageBQuorumDiscId(const QStringList& files) {
    std::vector<std::map<QString, QStringList>> bundles;
    bundles.reserve(files.size());
    for (const QString& p : files) {
        auto t = AudioTagIo::read(p);
        bundles.push_back(t ? *t : std::map<QString, QStringList>{});
    }
    const auto counts = countTag(bundles, QString::fromLatin1(kKeyDiscId));
    return quorumValue(counts, files.size(), kDiscIdQuorum);
}

void FolderIdentifier::onReleaseResolved(QString releaseMbid, QJsonObject release)
{
    if (m_stageInflight != QLatin1String("stageA-mb")) return;
    m_stageInflight.clear();
    appendDiag(QStringLiteral("Stage A: MB release fetch OK"));

    // Flatten via the existing helper; track count comes from the file
    // count (Stage A doesn't have a TOC). pickMedium falls back to the
    // first medium when no disc-ID is supplied.
    concerto::metadata::AlbumMeta a = musicbrainz::flattenRelease(
        release, /*discId=*/QString(), m_inflightTrackCount);
    a.releaseId  = releaseMbid;
    a.confidence = 90;
    a.sourceTag  = QStringLiteral("mb-id-quorum");

    // Persist to the rip-time cache too so a subsequent Stage A hit
    // (different folder, same release) skips the web on next open.
    if (m_cache && !a.releaseId.isEmpty()) {
        m_cache->put(/*mbDiscId=*/a.releaseId + QStringLiteral("__library-stub"),
                     a, release);
    }

    m_result.album      = std::move(a);
    m_result.identified = true;
    m_result.source     = QStringLiteral("mb-id-quorum");
    emitIdentified(m_result);
}

void FolderIdentifier::onReleaseFailed(QString /*releaseMbid*/,
                                       int httpStatus, QString message)
{
    if (m_stageInflight != QLatin1String("stageA-mb")) return;
    m_stageInflight.clear();
    appendDiag(QStringLiteral("Stage A: MB release fetch FAILED (status=%1, %2)")
                  .arg(httpStatus).arg(message));
    // Falling through to Stage B would normally be a re-entry; for v1
    // we surface unidentified and let the caller re-invoke or move on.
    m_result.identified = false;
    m_result.source = QStringLiteral("unidentified");
    emitIdentified(m_result);
}

void FolderIdentifier::onDiscIdResolved(QString discId, QJsonArray releases)
{
    if (m_stageInflight != QLatin1String("stageB-mb-discid")) return;
    m_stageInflight.clear();

    if (releases.isEmpty()) {
        appendDiag(QStringLiteral("Stage B: MB disc-ID returned no releases"));
        m_result.identified = false;
        m_result.source = QStringLiteral("unidentified");
        emitIdentified(m_result);
        return;
    }

    // Run the multi-candidate scorer with whatever TOC info we have.
    // For a library folder we don't have track lengths plumbed through
    // yet (Stage D will land them); pass just trackCount so the scorer
    // applies its other components.
    concerto::metadata::scoring::TocSummary toc;
    toc.discId     = discId;
    toc.trackCount = m_inflightTrackCount;
    const auto picked = concerto::metadata::scoring::pick(releases, toc);
    if (picked.releaseId.isEmpty()) {
        appendDiag(QStringLiteral("Stage B: scoring failed to pick a release"));
        m_result.identified = false;
        m_result.source = QStringLiteral("unidentified");
        emitIdentified(m_result);
        return;
    }
    appendDiag(QStringLiteral("Stage B: scorer picked %1 (%2)")
                  .arg(picked.releaseId).arg(picked.reason));
    m_result.album.scoringLog = picked.scoringLog;
    m_result.album.pickReason = picked.reason;
    m_result.album.mbDiscId   = discId;

    // Second-hop release fetch — same shape as Stage A's
    // releaseResolved path, but tagged as discid in the source.
    m_stageInflight = QStringLiteral("stageA-mb");  // reuse the slot
    m_mb->fetchRelease(picked.releaseId);
}

void FolderIdentifier::onDiscIdFailed(QString /*discId*/, int httpStatus,
                                      QString message)
{
    if (m_stageInflight != QLatin1String("stageB-mb-discid")) return;
    m_stageInflight.clear();
    appendDiag(QStringLiteral("Stage B: MB disc-ID FAILED (status=%1, %2)")
                  .arg(httpStatus).arg(message));
    m_result.identified = false;
    m_result.source = QStringLiteral("unidentified");
    emitIdentified(m_result);
}

void FolderIdentifier::emitIdentified(Result r) {
    emit identified(std::move(r));
}

void FolderIdentifier::appendDiag(const QString& line) {
    if (!m_result.diagnostic.isEmpty())
        m_result.diagnostic += QLatin1Char('\n');
    m_result.diagnostic += line;
}

} // namespace concerto::library
