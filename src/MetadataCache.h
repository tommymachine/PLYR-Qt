// Per-disc metadata cache. JSON-on-disk, following RipBatchStore's
// pattern (one file per record, no SQLite — the volume is tiny).
//
// File layout per §6.3 of METADATA_PLAN.md:
//   <AppDataLocation>/metadata-cache/<discid>.json
//     {
//       "albumMeta":    {...},      // runtime AlbumMeta
//       "rawMb":        {...},      // raw second-hop release JSON
//       "scoringLog":   [...],      // per-release component breakdown
//       "pipelineLog":  [...],      // per-stage outcome rows
//       "cachedAt":     "ISO-8601"
//     }
//
// Stored as both `(discId, mediumPosition)` AND `(releaseMbid,
// mediumPosition)` aliases — boxsets that share a release across discs
// don't re-pay the second-hop fetch.

#pragma once

#include "MetadataModel.h"

#include <QJsonObject>
#include <QString>
#include <optional>

namespace concerto::metadata {

class MetadataCache {
public:
    // Default: ~/Library/Application Support/Concerto/metadata-cache.
    static QString defaultDir();

    explicit MetadataCache(QString dir = defaultDir());

    struct Entry {
        AlbumMeta   album;
        QJsonObject rawMb;
        QString     cachedAt;
    };

    // Look up by disc-ID. The medium position is encoded inside the
    // stored AlbumMeta — callers verify the position matches what they
    // computed themselves.
    std::optional<Entry> getByDiscId(const QString& mbDiscId,
                                     int maxAgeDays = 30) const;

    // Look up by (releaseMbid, mediumPosition). Used when a different
    // disc of the same multi-medium release has already been resolved
    // — we can skip the second-hop fetch and just relocate the medium.
    std::optional<Entry> getByRelease(const QString& releaseMbid,
                                      int mediumPosition,
                                      int maxAgeDays = 30) const;

    // Write the cache entry for (discId, mediumPosition). Also drops a
    // small alias file under release/<mbid>__<pos>.json so the next
    // disc of the same box-set release can read from the same cache.
    void put(const QString& mbDiscId,
             const AlbumMeta& album,
             const QJsonObject& rawMb);

    // Drop the entry for a disc-ID. Aliases are left in place — they're
    // keyed by release MBID, not disc-ID, and serving a stale alias is
    // strictly preferable to re-paying the MB hop for a sibling disc.
    void invalidate(const QString& mbDiscId);

    QString dir() const { return m_dir; }

private:
    QString m_dir;
};

} // namespace concerto::metadata
