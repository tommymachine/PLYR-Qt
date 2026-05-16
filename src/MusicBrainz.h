// MusicBrainz lookup for the CD-rip pipeline. Two hops:
//
//   1. Disc-ID lookup —
//      GET /ws/2/discid/<id>?inc=artist-credits+recordings+release-groups
//      Returns the candidate releases the disc-ID maps to.
//
//   2. Release second-hop (one MBID, chosen by the scorer) —
//      GET /ws/2/release/<mbid>?inc=artist-credits+recordings+labels+
//                                  release-groups+media+work-rels+
//                                  recording-level-rels+work-level-rels+
//                                  artist-rels
//      Returns work-rels and typed artist-rels — composers,
//      conductors, performing orchestras, soloists.
//
// The Vorbis-friendly flattening (Release JSON → AlbumMeta) lives in
// MusicBrainz.cpp::flattenRelease — used by MetadataResolver during
// Stage 1b. The legacy `lookupByDiscId` shim is preserved for the
// existing RipWorker call site so the rip flow keeps compiling.

#pragma once

#include "MetadataModel.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>

#include <string>
#include <vector>

namespace musicbrainz {

// Legacy shapes — kept for the existing RipWorker call. Stage 1 still
// emits these so the disc-identification UI doesn't have to change.

struct Track {
    int position = 0;
    std::string title;
    int durationMs = 0;
    std::string recordingId;
};
struct Disc {
    int position = 1;
    int totalCount = 1;
    std::vector<Track> tracks;
};
struct Release {
    std::string id;
    std::string releaseGroupId;
    std::string title;
    std::string artist;
    std::string date;
    std::string country;
    Disc disc;
};

// Synchronous disc-ID lookup. Kept for the existing RipWorker; new code
// uses Client (async). Internally synchronous via a nested event loop —
// a known footgun in worker threads; new pipeline avoids it.
std::vector<Release> lookupByDiscId(QNetworkAccessManager& nam,
                                    const std::string& discId,
                                    int trackCount = 0,
                                    const std::string& userAgent
                                        = "concerto-arverify/0.1");

// ---------------------------------------------------------------------
// New async client (used by MetadataResolver).

class Client : public QObject {
    Q_OBJECT
public:
    explicit Client(QNetworkAccessManager* nam, QObject* parent = nullptr);

    void setUserAgent(const QString& ua) { m_userAgent = ua; }

    // Stage 1: disc-ID lookup. Emits discIdResolved(...) with the raw
    // releases array (may be empty) or discIdFailed(...) on transport
    // failure. Both terminate the call.
    void fetchDiscId(const QString& discId);

    // Stage 1b: release second-hop. Emits releaseResolved(...) with the
    // raw release object or releaseFailed(...) on transport failure.
    void fetchRelease(const QString& releaseMbid);

signals:
    void discIdResolved(QString discId, QJsonArray releases);
    void discIdFailed(QString discId, int httpStatus, QString message);

    void releaseResolved(QString releaseMbid, QJsonObject release);
    void releaseFailed(QString releaseMbid, int httpStatus, QString message);

private:
    QNetworkAccessManager* m_nam;
    QString                m_userAgent = QStringLiteral(
        "concerto-metadata/0.1 ( https://github.com/tfletcher/plyr-qt )");
};

// Pure flattener. Given a release JSON (from Stage 1b second-hop), a
// disc-ID, and the queried track count, build a complete `AlbumMeta`:
//   - pick the correct medium (disc-ID > track-count > medium[0])
//   - extract release-level fields (date, country, barcode, label,
//     catalog-number, release-group, ASIN)
//   - per track: title, recording MBID, work MBID + composer (via
//     work.relations[type=composer]), typed performers (conductor,
//     performing orchestra, instrument, vocal)
//   - heuristic movement extraction: split track title on ": " and use
//     the right-hand side as movementName; number movements within each
//     work-group 1..N
QString primaryArtistName(const QJsonObject& release);
concerto::metadata::AlbumMeta flattenRelease(const QJsonObject& release,
                                         const QString& discId,
                                         int trackCount);

} // namespace musicbrainz
