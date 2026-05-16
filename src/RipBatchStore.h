// JSON-backed persistence for multi-disc rip batches.
//
// One file per batch under
// `~/Library/Application Support/Concerto/rip_batches/<batch_id>.json`,
// keyed at lookup time by the release-group's MusicBrainz id. The file
// is small (a few KB per batch) and read on demand — no in-memory cache
// beyond what callers hold themselves.
//
// Lifecycle:
//   1. First disc of a batch identified  -> save() with a new batch
//      (status of disc N = "done", others = "pending").
//   2. Subsequent discs in the same batch get loaded by
//      lookupByReleaseGroup(), updated with the new disc's saved_path,
//      saved back.
//   3. Cancel mid-rip -> the in-progress disc reverts to "pending" and
//      the batch is saved; the user can resume later.
//   4. The "delete batch" path in the UI removes the JSON file (the
//      already-saved disc folders themselves are not touched).
//
// Status values:
//   "pending"   — disc hasn't been ripped yet.
//   "done"      — disc has been ripped and saved to saved_path.
//   "in_progress" — disc is being ripped right now (transitional;
//                   becomes "done" on save or "pending" on cancel).
//
// Schema (see SCANCERTO_PLAN.md for the canonical reference):
//   { "batch_id":              "01J...",
//     "album_title":           "Ravel — Complete Edition",
//     "artist":                "Maurice Ravel",
//     "mb_release_group_id":   "abc-...",
//     "total_discs":           14,
//     "parent_folder":         "/Users/.../Music/Ravel_Edition",
//     "discs": [
//       { "position": 1, "mb_disc_id": "...", "status": "done",
//         "saved_path": ".../Disc_01" },
//       { "position": 2, "status": "pending" } ],
//     "created_at":            "2026-05-12T...",
//     "updated_at":            "2026-05-15T..." }
#pragma once

#include <QDateTime>
#include <QString>
#include <QVector>

#include <optional>

namespace plyr::cd {

struct RipBatchDisc {
    int     position    = 0;       // 1-based
    QString mbDiscId;              // MusicBrainz disc id (empty until ripped)
    QString status      = QStringLiteral("pending");
    QString savedPath;             // absolute path to the disc's folder
};

struct RipBatch {
    QString id;                    // batch_id (uuid v4 / ulid)
    QString albumTitle;
    QString artist;
    QString releaseGroupId;
    int     totalDiscs   = 0;
    QString parentFolder;          // absolute path where saved discs live
    QVector<RipBatchDisc> discs;
    QDateTime createdAt;
    QDateTime updatedAt;
};

class RipBatchStore {
public:
    // Default location: ~/Library/Application Support/Concerto/rip_batches/
    // Pass an alternate path for tests / one-off scratch runs.
    static QString defaultRoot();

    // Save (creates the directory if needed). Updates `updatedAt` to now,
    // and `createdAt` to now if it's null. Returns true on success.
    static bool save(const RipBatch& batch, const QString& root = defaultRoot());

    // Look up by release-group id. Returns the first match. The store
    // doesn't enforce uniqueness — if the user resumes a release group
    // twice with different physical pressings, both files coexist and
    // this returns the most-recently-updated one.
    static std::optional<RipBatch> lookupByReleaseGroup(
        const QString& releaseGroupId,
        const QString& root = defaultRoot());

    // List every batch on disk that has at least one pending disc, in
    // updated_at-descending order. For the resume picker.
    static QVector<RipBatch> listResumable(const QString& root = defaultRoot());

    // Delete a batch's JSON file. Already-saved disc folders are NOT
    // touched. Returns true if the file was deleted (or never existed).
    static bool remove(const QString& batchId,
                       const QString& root = defaultRoot());

    // Generate a new batch id. Random + timestamped, no external deps.
    static QString newBatchId();
};

} // namespace plyr::cd
