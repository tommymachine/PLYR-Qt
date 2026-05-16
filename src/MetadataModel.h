// POD types for the resolved-metadata layer.
//
// Mirrors §6.2 of METADATA_PLAN.md. AlbumMeta is the runtime shape every
// stage of the pipeline produces; FlacTagBuilder maps it to Vorbis
// comments, and MetadataResolver emits it once after the chain settles.
//
// No Qt parent/child semantics, no QObject — these are value types.

#pragma once

#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

namespace concerto::metadata {

// One credited performer on a recording (classical CDs typically have
// several per track). `role` is the canonical MB relation type:
//   "conductor"            — recording.relations[type=conductor]
//   "performing orchestra" — recording.relations[type=performing orchestra]
//   "instrument"           — recording.relations[type=instrument]; attrs[]
//                            holds the instrument string ("piano", "violin")
//   "vocal"                — recording.relations[type=vocal]; attrs[] holds
//                            the range ("soprano", "tenor")
struct Performer {
    QString     name;        // target-credit ?: artist.name
    QString     sortName;
    QString     role;
    QStringList attrs;
    QString     mbArtistId;
};

struct TrackMeta {
    int     position      = 0;   // 1-based within the medium
    QString title;               // movement-level (the track's own title)
    QString workTitle;           // parent work — e.g. "Shéhérazade"
    QString movementName;        // "Asie"
    int     movementNumber = 0;  // 0 = unknown
    int     movementTotal  = 0;  // 0 = unknown
    qint64  durationMs    = 0;

    QString recordingId;         // MB Recording MBID
    QString workId;              // MB Work MBID

    QString composerId;          // MB Artist MBID
    QString composerName;
    QString composerSort;

    QVector<Performer> performers;

    QString isrc;
};

struct AlbumMeta {
    QString releaseId;           // MB Release MBID
    QString releaseGroupId;      // MB Release-Group MBID
    QString title;
    QString artistCredit;        // joined "Composer; Soloist, Orchestra, Conductor"
    QString albumArtist;         // the release-level artist-credit phrase
    QString albumArtistId;       // the first/primary album-artist MBID
    QString date;
    QString originalDate;
    QString country;
    QString barcode;
    QString catalogNumber;
    QString label;
    QString asin;

    int     discPosition   = 1;
    int     discTotalCount = 1;
    QString discSubtitle;        // the medium's "title" — e.g. "Songs"

    QString mbDiscId;            // the disc-ID we resolved from
    QString coverArtUrl;
    QStringList genreNames;      // Apple-only in v1; empty under MB-only

    QVector<TrackMeta> tracks;

    int     confidence = 0;      // 0..100
    QString sourceTag;           // "musicbrainz" | "cd-text" | "acoustid" | "stub"

    // Diagnostics. Captured during resolution, written to the cache.
    QString    pickReason;       // human-readable for the scoringLog header
    QVariantList scoringLog;     // [{releaseId, score, components: {...}}, ...]
    QVariantList pipelineLog;    // [{stage, outcome, durationMs}, ...]

    bool isEmpty() const { return title.isEmpty() && tracks.isEmpty(); }
};

// Pretty-print a single track's tag bundle for diagnostics — the order
// matches the example in §2.4.1 of METADATA_PLAN.md so test output
// reads naturally next to the plan.
QString debugDumpTrack(const AlbumMeta& album, int trackIndex0Based);

} // namespace concerto::metadata
