// PCM -> FLAC encoding for the CD-rip pipeline, built on libFLAC. The CD
// case is the only consumer right now, so the API is locked to Red Book
// audio (44.1 kHz / stereo / 16-bit). The encoder writes a FLAC file in
// one call — callers hold the whole track in memory anyway (the verifier
// already does this), so a streaming API would be more surface for no
// benefit yet.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace flacencode {

struct EncoderConfig {
    // Encoder compression level, 0..8. 8 = `flac -8` (the level the existing
    // rip_cd.sh script uses and what AccurateRip submitters typically pick).
    int compressionLevel = 8;
};

// One Vorbis-comment entry: a field name (e.g. "TITLE") and its value. The
// field name is case-insensitive per the Vorbis spec; conventional writers
// use UPPERCASE — TITLE, ARTIST, ALBUM, ALBUMARTIST, DATE, TRACKNUMBER,
// TRACKTOTAL, DISCNUMBER, DISCTOTAL, MUSICBRAINZ_DISCID, MUSICBRAINZ_ALBUMID,
// MUSICBRAINZ_TRACKID. Values are UTF-8.
using VorbisTag = std::pair<std::string, std::string>;

// Encode interleaved 16-bit stereo PCM (44.1 kHz) to a FLAC file at
// `outPath`. `pcm` points at `frames` L/R pairs (2 * frames int16 samples).
// If `tags` is non-empty, a Vorbis comment block is written ahead of the
// audio. Returns true on success; on failure, any partially-written output
// file is removed before returning.
bool encodeCdAudioToFile(const std::string& outPath,
                         const int16_t* pcm, uint64_t frames,
                         const EncoderConfig& config = {},
                         const std::vector<VorbisTag>& tags = {});

} // namespace flacencode
