// Deterministic ranking for the Stage-1 multi-release case.
//
// Pure functions over raw MB JSON: given an array of release candidates
// returned from `/ws/2/discid/<id>` AND the queried TOC, pick exactly one.
// Identical input → identical output on every machine.
//
// The score-component breakdown is exported for the cache scoringLog so
// the dev can inspect why a particular release won.
//
// Algorithm: METADATA_PIPELINE_AUTOMATED.md "Scoring function".

#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <cstdint>
#include <vector>

namespace concerto::metadata::scoring {

// The TOC properties scoring looks at — full ArVerify TOC is overkill.
struct TocSummary {
    QString discId;
    int     trackCount = 0;
    // Per-track length in seconds, derived from the TOC. Optional; if
    // empty the duration-similarity rule contributes 0.
    std::vector<int> trackLengthsSec;
};

// Per-component breakdown of why a release scored what it did. Lives in
// the cache's scoringLog rows.
struct Components {
    int composerHit    = 0;  // +50
    int conductor      = 0;  // +20
    int barcode        = 0;  // +30
    int country        = 0;  // +15
    int artistIds      = 0;  // +10 per artist-credit entry with .artist.id
    int releaseGroupAlbum = 0;  // +5
    int durationDefender  = 0;  // 0..+30
    int statusPenalty  = 0;  // -20
    int coverArtPenalty = 0; // -10
    int total          = 0;
};

// Score one release. `release` is the raw MB JSON object as returned by
// the disc-ID endpoint (no second-hop required). Returns the breakdown.
Components score(const QJsonObject& release, const TocSummary& toc);

// Result of pick(): the chosen release MBID + the full ranked log for
// the cache. Log entries are in score-descending order.
struct Pick {
    QString      releaseId;
    QJsonObject  release;       // the chosen release JSON (for second-hop)
    QVariantList scoringLog;    // [{releaseId, score, components: {...}}]
    QString      reason;        // short human label, e.g. "1 candidate; auto"
};

// Rank `releases` deterministically and return the winner.
//   key 1: score, descending
//   key 2: release-group.first-release-date, ascending (earliest wins)
//   key 3: release.id, ascending (lex MBID — final tiebreak guaranteed
//          to produce the same answer on every machine)
// Empty input → empty Pick (releaseId.isEmpty()).
Pick pick(const QJsonArray& releases, const TocSummary& toc);

// Given a release JSON and the queried disc-ID, pick the medium that
// matches our disc. Precedence: disc-ID match, then track-count match,
// then medium[0]. Returns the medium JSON and writes the medium position
// into `outPosition` (1-based, defaults to 1).
//
// Multi-medium correct-disc selector — METADATA_PIPELINE_AUTOMATED.md
// "Multi-medium correct-disc selector".
QJsonObject pickMedium(const QJsonObject& release,
                       const QString& discId,
                       int trackCount,
                       int* outPosition);

} // namespace concerto::metadata::scoring
