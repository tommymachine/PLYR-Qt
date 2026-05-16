// Append-only log of discs that fell all the way through to Stage 4.
//
// One JSONL row per stub-tagged rip. The dev periodically exports the
// file and submits the listed disc-IDs to MusicBrainz under their own
// account (manual; no in-app submission). The user never reads it.
//
// METADATA_PIPELINE_AUTOMATED.md "Stage outcomes the cache records".

#pragma once

#include <QJsonArray>
#include <QString>

#include <cstdint>
#include <vector>

namespace concerto::metadata {

struct TocRow {
    int                   firstTrack = 0;
    int                   lastTrack  = 0;
    uint32_t              leadoutLba = 0;
    std::vector<uint32_t> offsets;     // 1-based, in absolute frame offsets
                                       // (kLeadInFrames-padded); matches the
                                       // disc-ID algorithm's input shape
};

class PendingSubmissions {
public:
    // Default: ~/Library/Application Support/Concerto/pending-submissions.jsonl
    static QString defaultPath();

    // Append one row. Best-effort — silently no-ops on I/O failure.
    static void append(const QString& mbDiscId,
                       const TocRow& toc,
                       const QString& ripDir,
                       const QStringList& stageFailures,
                       const QString& path = defaultPath());
};

} // namespace concerto::metadata
