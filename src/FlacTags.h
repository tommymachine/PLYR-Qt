// Minimal FLAC metadata reader. Parses the `STREAMINFO` and
// `VORBIS_COMMENT` blocks directly — no external FLAC library required.
// Port of the Swift `FLACTags` reader from PLYR.
//
// Usage:
//   auto tags = FlacTags::read("/path/to/file.flac");
//   if (tags.has_value()) {
//       qDebug() << (*tags).get("TITLE");
//       qDebug() << (*tags).duration;
//   }
//
// The classical-side rip pipeline also writes Vorbis comments via the
// libFLAC encoder. The mapping from a resolved AlbumMeta → flacencode
// VorbisTag list lives at the bottom of this header (Picard 2.x tag
// mapping, METADATA_PLAN.md §2.4).

#pragma once

#include "FlacEncode.h"
#include "MetadataModel.h"

#include <QByteArray>
#include <QHash>
#include <QString>
#include <optional>
#include <vector>

struct FlacTags {
    // Vorbis-comment key/value pairs. Keys are stored upper-cased,
    // matching how callers tend to want to look them up (`TITLE`,
    // `ARTIST`, `ALBUM`, `COMPOSER`, `TRACKNUMBER`, `RECORDINGDATE`, …).
    QHash<QString, QString> tags;

    // Track duration in seconds, from the STREAMINFO block.
    double duration = 0.0;

    // Returns the tag for `key` (case-insensitive), or a default value.
    QString get(const QString& key, const QString& fallback = {}) const {
        return tags.value(key.toUpper(), fallback);
    }

    // Reads and parses a FLAC file. Returns std::nullopt if the file
    // cannot be opened or is missing the `fLaC` magic.
    static std::optional<FlacTags> read(const QString& path);

private:
    static void parseStreamInfo(const QByteArray& data, FlacTags& out);
    static void parseVorbisComment(const QByteArray& data, FlacTags& out);
};


namespace concerto::metadata {

// Build the per-track Vorbis comment list for libFLAC. Maps every
// populated AlbumMeta / TrackMeta field to its Picard canonical Vorbis
// key (METADATA_PLAN.md §2.4 + §2.4.1). PERFORMER and GENRE are
// multi-valued; other fields produce zero or one tag each.
//
// `trackIndex0Based` indexes into `album.tracks`. Out-of-range indices
// produce a partial tag set (album-level only) — useful for the disc
// CUE sheet, etc.
std::vector<flacencode::VorbisTag> buildVorbisTags(
    const AlbumMeta& album, int trackIndex0Based);

} // namespace concerto::metadata
