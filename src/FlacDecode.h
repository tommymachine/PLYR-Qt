// FLAC -> PCM decoding for the AccurateRip verification path, built on
// libFLAC. Two entry points: a fast metadata-only STREAMINFO read (used to
// reconstruct the disc TOC without decoding any audio) and a full decode to
// interleaved 16-bit PCM (used to compute the track checksums).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace flacdecode {

// STREAMINFO fields relevant to CD-rip verification.
struct StreamInfo {
    uint64_t totalSamples = 0;   // inter-channel samples, i.e. L/R frames
    uint32_t sampleRate = 0;
    uint32_t channels = 0;
    uint32_t bitsPerSample = 0;

    // True when the stream is Red Book CD audio: 44.1 kHz, stereo, 16-bit.
    bool isCdFormat() const {
        return sampleRate == 44100 && channels == 2 && bitsPerSample == 16;
    }
};

// Read just the STREAMINFO block — no audio is decoded. Fast; use this to
// size the TOC. Returns nullopt if the file can't be opened or has no
// STREAMINFO block.
std::optional<StreamInfo> readStreamInfo(const std::string& path);

// A fully decoded track: interleaved stereo 16-bit PCM.
struct DecodedAudio {
    std::vector<int16_t> pcm;   // L, R, L, R, ...
    uint64_t frames = 0;        // number of L/R pairs actually decoded
    StreamInfo info;
};

// Decode an entire FLAC file to PCM. Returns nullopt on decode failure or if
// the file is not Red Book CD audio (the AccurateRip checksum is only
// defined over 44.1 kHz / stereo / 16-bit streams).
std::optional<DecodedAudio> decodeFile(const std::string& path);

// Read the file's Vorbis comment block (if any) as a list of (field, value)
// pairs in the order libFLAC reports them. Returns an empty vector if the
// file has no comments, can't be opened, or the comment block is malformed.
std::vector<std::pair<std::string, std::string>> readVorbisComments(
    const std::string& path);

} // namespace flacdecode
