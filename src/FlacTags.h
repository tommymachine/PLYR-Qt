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

#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <optional>

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
