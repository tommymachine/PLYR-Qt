#include "FlacDecode.h"

#include <FLAC/metadata.h>
#include <FLAC/stream_decoder.h>

#include <cstring>

namespace flacdecode {

namespace {

// libFLAC write callback: append one decoded frame's samples, interleaved.
FLAC__StreamDecoderWriteStatus writeCallback(
    const FLAC__StreamDecoder*, const FLAC__Frame* frame,
    const FLAC__int32* const buffer[], void* client)
{
    auto* out = static_cast<DecodedAudio*>(client);

    // Guard the dual-channel access below: a non-stereo stream would make
    // buffer[1] invalid. The result is discarded later by isCdFormat(),
    // but we must not read past the channel array first.
    if (frame->header.channels != 2)
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;

    const unsigned n = frame->header.blocksize;
    out->pcm.reserve(out->pcm.size() + static_cast<size_t>(n) * 2);
    for (unsigned i = 0; i < n; ++i) {
        out->pcm.push_back(static_cast<int16_t>(buffer[0][i]));
        out->pcm.push_back(static_cast<int16_t>(buffer[1][i]));
    }
    out->frames += n;
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

// libFLAC metadata callback: capture STREAMINFO and pre-size the buffer.
void metadataCallback(const FLAC__StreamDecoder*,
                      const FLAC__StreamMetadata* meta, void* client)
{
    if (meta->type != FLAC__METADATA_TYPE_STREAMINFO)
        return;
    auto* out = static_cast<DecodedAudio*>(client);
    const FLAC__StreamMetadata_StreamInfo& si = meta->data.stream_info;
    out->info.totalSamples  = si.total_samples;
    out->info.sampleRate    = si.sample_rate;
    out->info.channels      = si.channels;
    out->info.bitsPerSample = si.bits_per_sample;
    if (si.total_samples > 0 && si.channels == 2)
        out->pcm.reserve(static_cast<size_t>(si.total_samples) * 2);
}

void errorCallback(const FLAC__StreamDecoder*,
                   FLAC__StreamDecoderErrorStatus, void*)
{
    // Per-frame decode errors are reflected in the decoder state, which the
    // caller checks after the run; there is nothing useful to do mid-stream.
}

} // namespace

std::optional<StreamInfo> readStreamInfo(const std::string& path) {
    FLAC__StreamMetadata meta;
    if (!FLAC__metadata_get_streaminfo(path.c_str(), &meta))
        return std::nullopt;
    const FLAC__StreamMetadata_StreamInfo& si = meta.data.stream_info;
    StreamInfo info;
    info.totalSamples  = si.total_samples;
    info.sampleRate    = si.sample_rate;
    info.channels      = si.channels;
    info.bitsPerSample = si.bits_per_sample;
    return info;
}

std::vector<std::pair<std::string, std::string>> readVorbisComments(
    const std::string& path)
{
    std::vector<std::pair<std::string, std::string>> out;
    FLAC__StreamMetadata* block = nullptr;
    // get_tags walks the file header until it finds a VORBIS_COMMENT block,
    // copies it, and returns false if none exists or the file isn't a FLAC.
    if (!FLAC__metadata_get_tags(path.c_str(), &block) || !block)
        return out;

    const FLAC__StreamMetadata_VorbisComment& vc = block->data.vorbis_comment;
    out.reserve(vc.num_comments);
    for (FLAC__uint32 i = 0; i < vc.num_comments; ++i) {
        const FLAC__StreamMetadata_VorbisComment_Entry& e = vc.comments[i];
        const char* bytes = reinterpret_cast<const char*>(e.entry);
        // Entries are "FIELD=VALUE", with FIELD case-insensitive. Splitting
        // on the first '=' is what the Vorbis spec mandates; values
        // themselves may contain '='.
        const char* eq = static_cast<const char*>(memchr(bytes, '=', e.length));
        if (!eq) continue;
        out.emplace_back(std::string(bytes, eq - bytes),
                         std::string(eq + 1, bytes + e.length - (eq + 1)));
    }
    FLAC__metadata_object_delete(block);
    return out;
}

std::optional<DecodedAudio> decodeFile(const std::string& path) {
    FLAC__StreamDecoder* decoder = FLAC__stream_decoder_new();
    if (!decoder)
        return std::nullopt;

    DecodedAudio out;
    const FLAC__StreamDecoderInitStatus init = FLAC__stream_decoder_init_file(
        decoder, path.c_str(), writeCallback, metadataCallback,
        errorCallback, &out);
    if (init != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        FLAC__stream_decoder_delete(decoder);
        return std::nullopt;
    }

    const bool ok = FLAC__stream_decoder_process_until_end_of_stream(decoder);
    const FLAC__StreamDecoderState state = FLAC__stream_decoder_get_state(decoder);
    FLAC__stream_decoder_finish(decoder);
    FLAC__stream_decoder_delete(decoder);

    if (!ok || state == FLAC__STREAM_DECODER_ABORTED)
        return std::nullopt;
    if (!out.info.isCdFormat())
        return std::nullopt;
    return out;
}

} // namespace flacdecode
