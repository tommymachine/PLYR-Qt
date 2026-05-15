// MusicBrainz lookup for the CD-rip pipeline. Single-shot HTTPS GET against
// `musicbrainz.org/ws/2/discid/<id>`, parsed into the minimum metadata the
// tagger needs: album title, artist, date, per-track titles, and the disc's
// position in any multi-disc release.
//
// Replaces the Python in `rip_cd.sh` (`musicbrainzngs.get_releases_by_discid`)
// with a Qt-only equivalent — same medium-matching precedence: by disc ID
// first, then by track count, last-resort medium[0] of release[0].
#pragma once

#include <QNetworkAccessManager>

#include <string>
#include <vector>

namespace musicbrainz {

struct Track {
    int position = 0;       // 1-based position within the disc
    std::string title;
    int durationMs = 0;     // 0 if MB didn't have a length for this recording
    std::string recordingId; // MusicBrainz MBID; empty if not present
};

// The medium (one disc of a release) we matched against the query.
struct Disc {
    int position = 1;       // 1-based; which disc of a multi-disc release
    int totalCount = 1;     // total discs in the release
    std::vector<Track> tracks;
};

struct Release {
    std::string id;         // MBID of the release
    std::string title;
    std::string artist;     // joined artist-credit phrase
    std::string date;       // "YYYY", "YYYY-MM-DD" — whatever MB has
    std::string country;
    Disc disc;
};

// Look up releases for a given MusicBrainz disc ID. Synchronous (runs a
// nested event loop on `nam`). Returns the empty vector on HTTP failure
// or "disc not found in MB".
//
// `trackCount` is used as a fallback medium-matching key when the disc ID
// doesn't match any `discs[].id` in the response — typical for old
// submissions where the disc-id linkage was never recorded. Pass 0 to
// skip the fallback (then we use medium[0] of release[0], the last resort).
std::vector<Release> lookupByDiscId(QNetworkAccessManager& nam,
                                    const std::string& discId,
                                    int trackCount = 0,
                                    const std::string& userAgent
                                        = "plyr-arverify/0.1");

} // namespace musicbrainz
