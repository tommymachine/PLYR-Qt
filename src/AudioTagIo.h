// Single tag I/O surface for every format Concerto plays, backed by
// TagLib (dynamic-linked, LGPL-2.1 — see CMakeLists.txt note).
//
// Reads pull TagLib's `PropertyMap` plus the format-specific extension
// for MP4 freeform atoms (the `----:com.apple.iTunes:*` keys that some
// TagLib versions don't surface through `PropertyMap`). Writes go
// through TagLib's FileRef + PropertyMap.
//
// Logical keys follow §4.2 of docs/LIBRARY_METADATA_PLAN.md — they're
// the Picard 2.x canonical names ("COMPOSER", "PERFORMER",
// "MUSICBRAINZ_ALBUMID", "CONCERTO_PIPELINE_VERSION", etc.). Values are
// always multi-valued; readers consult [0] for single-valued keys.
//
// FLAC fast-path: this class always goes through TagLib for symmetry.
// The existing rip-time `FlacTags::read()` is kept untouched as the
// faster hot-path for that one consumer; the library flow uses
// AudioTagIo unconditionally.

#pragma once

#include <QString>
#include <QStringList>

#include <map>
#include <optional>

namespace concerto::library {

class AudioTagIo {
public:
    enum class Format {
        Unknown,
        Flac,
        Mp3,
        Mp4,     // M4A / ALAC / AAC in an MP4 container
        Ogg,
        Opus,
        Wav,
        Aiff,
    };

    // Detect the container format from file extension. Stable, fast,
    // pre-open. TagLib's FileRef::create dispatches the same way under
    // the hood; we mirror the dispatch so callers can short-circuit.
    static Format detect(const QString& path);

    // Returns the upper-cased format name (e.g. "flac", "mp3") for the
    // diagnostics column on files.format.
    static QString formatName(Format f);

    // Read every tag we care about. Keys follow the plan's mapping
    // table; values are multi-valued. Returns nullopt on file-open
    // failure or unsupported format.
    static std::optional<std::map<QString, QStringList>> read(const QString& path);

    // Convenience: get a single-value field, returning the first
    // element of the multi-valued result or fallback if absent.
    static QString readField(const std::map<QString, QStringList>& tags,
                             const QString& key,
                             const QString& fallback = QString());

    // Returns track duration in seconds, 0 on failure. Goes through
    // TagLib's audio-properties interface so any container is handled.
    static double readDurationSec(const QString& path);

    // Writeback path. FLAC / OGG go through Vorbis comments;
    // other formats currently stub out with a qWarning + return false.
    // Per §10 Step 7 of the plan, the other formats light up together
    // later via TagLib's PropertyMap.
    static bool write(const QString& path,
                      const std::map<QString, QStringList>& tags);
};

} // namespace concerto::library
