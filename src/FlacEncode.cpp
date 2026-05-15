#include "FlacEncode.h"

#include <FLAC/metadata.h>
#include <FLAC/stream_encoder.h>

#include <cstdio>
#include <string>
#include <vector>

namespace flacencode {

namespace {

// libFLAC's process_interleaved takes FLAC__int32 samples — one int32 per
// channel-sample — even for 16-bit input. We feed it in chunks instead of
// allocating a track-wide int32 buffer (a 5-min CD track is ~26M samples
// across L+R = ~100 MB of doubled-up data we don't need to hold at once).
constexpr uint64_t kChunkFrames = 65536;

} // namespace

bool encodeCdAudioToFile(const std::string& outPath,
                         const int16_t* pcm, uint64_t frames,
                         const EncoderConfig& config,
                         const std::vector<VorbisTag>& tags)
{
    FLAC__StreamEncoder* enc = FLAC__stream_encoder_new();
    if (!enc)
        return false;

    // Red Book CD audio is the only shape we encode. Hard-coding here keeps
    // the call sites honest — passing the wrong rate or bit depth at the
    // encoder layer would silently produce a non-CD FLAC.
    FLAC__stream_encoder_set_compression_level(enc, config.compressionLevel);
    FLAC__stream_encoder_set_sample_rate(enc, 44100);
    FLAC__stream_encoder_set_channels(enc, 2);
    FLAC__stream_encoder_set_bits_per_sample(enc, 16);
    FLAC__stream_encoder_set_total_samples_estimate(enc, frames);

    // Build a Vorbis comment block from `tags` (if any). libFLAC owns the
    // block once we hand it to set_metadata; we must delete it after
    // finish() returns. NULL-terminated `field=value` strings is the wire
    // format Vorbis expects.
    FLAC__StreamMetadata* commentBlock = nullptr;
    if (!tags.empty()) {
        commentBlock = FLAC__metadata_object_new(FLAC__METADATA_TYPE_VORBIS_COMMENT);
        if (!commentBlock) {
            FLAC__stream_encoder_delete(enc);
            return false;
        }
        for (const VorbisTag& tag : tags) {
            FLAC__StreamMetadata_VorbisComment_Entry entry;
            const std::string line = tag.first + "=" + tag.second;
            // entry_from_name_value_pair would do the join itself but it
            // also validates the field name against the Vorbis grammar
            // (no spaces, no '='), which we want — bad inputs would
            // otherwise corrupt the comment block silently.
            if (!FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(
                    &entry, tag.first.c_str(), tag.second.c_str())) {
                FLAC__metadata_object_delete(commentBlock);
                FLAC__stream_encoder_delete(enc);
                return false;
            }
            // append_comment takes ownership of entry.entry, so don't free.
            if (!FLAC__metadata_object_vorbiscomment_append_comment(
                    commentBlock, entry, /*copy=*/false)) {
                std::free(entry.entry);
                FLAC__metadata_object_delete(commentBlock);
                FLAC__stream_encoder_delete(enc);
                return false;
            }
        }
        FLAC__StreamMetadata* metadata[] = { commentBlock };
        if (!FLAC__stream_encoder_set_metadata(enc, metadata, 1)) {
            FLAC__metadata_object_delete(commentBlock);
            FLAC__stream_encoder_delete(enc);
            return false;
        }
    }

    if (FLAC__stream_encoder_init_file(enc, outPath.c_str(),
                                       nullptr, nullptr)
            != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
        if (commentBlock) FLAC__metadata_object_delete(commentBlock);
        FLAC__stream_encoder_delete(enc);
        return false;
    }

    std::vector<FLAC__int32> chunk(kChunkFrames * 2);
    bool ok = true;
    for (uint64_t off = 0; off < frames && ok; off += kChunkFrames) {
        const uint64_t n = (frames - off < kChunkFrames)
                            ? (frames - off) : kChunkFrames;
        // Widen int16 -> int32 in-place per chunk. Compiler vectorizes this
        // trivially; the buffer stays small enough to stay in L2.
        const int16_t* src = pcm + (off * 2);
        for (uint64_t i = 0; i < n * 2; ++i)
            chunk[i] = static_cast<FLAC__int32>(src[i]);

        ok = FLAC__stream_encoder_process_interleaved(
            enc, chunk.data(), static_cast<unsigned>(n));
    }

    // Always run finish() so libFLAC writes the MD5 + final block, even when
    // we failed mid-stream — otherwise the output file is junk *and* the
    // encoder leaks file descriptors.
    if (!FLAC__stream_encoder_finish(enc))
        ok = false;
    FLAC__stream_encoder_delete(enc);
    if (commentBlock)
        FLAC__metadata_object_delete(commentBlock);

    if (!ok)
        std::remove(outPath.c_str());
    return ok;
}

} // namespace flacencode
