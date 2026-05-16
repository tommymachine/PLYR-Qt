// Stage 3 placeholder: AcoustID fingerprint lookup.
//
// The full implementation per METADATA_PIPELINE_AUTOMATED.md "AcoustID
// call shape" feeds track-1 PCM through Chromaprint, GETs
//   https://api.acoustid.org/v2/lookup?client=<key>&duration=&fingerprint=&format=json
// and walks the top result to a recording MBID + candidate releases.
//
// v1 ships the interface only — `lookup()` returns std::nullopt. No
// Chromaprint dep is added to CMake yet. The pipe degrades to Stage 4.

#pragma once

#include <QByteArray>
#include <QString>
#include <optional>

namespace concerto::metadata {

struct AcoustIdMatch {
    QString recordingMbid;       // MB Recording MBID — second-hop input
    double  score = 0.0;         // fingerprint confidence (0..1)
};

class AcoustIdProvider {
public:
    // `fingerprint` is the Chromaprint base64 string. `durationSec` is
    // the track duration in whole seconds (AcoustID rejects fractional).
    // Returns nullopt on miss, low score (<0.5), or "not implemented".
    static std::optional<AcoustIdMatch> lookup(const QByteArray& fingerprint,
                                               int durationSec);
};

} // namespace concerto::metadata
