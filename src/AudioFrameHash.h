// Content-stable hash over the *decoded audio frames* — not the file
// bytes. Tag edits and writeback change the file bytes but never the
// decoded PCM, which is exactly what we want for the library DB's
// file-keying scheme (plan §6.2).
//
// Implementation: Qt's QAudioDecoder dispatches every format the player
// already handles (FLAC / MP3 / M4A / OGG / WAV / AIFF on macOS via
// AudioToolbox), so we get format-agnostic coverage for free. Hash is
// SHA-256 (QCryptographicHash) over the interleaved 16-bit-PCM byte
// stream of the first N seconds. Truncating at N=30s bounds the cost
// (sub-second on lossy / FLAC) while keeping the hash discriminative —
// 30s at 44.1 kHz / stereo / 16-bit = ~5 MB of PCM, well past the
// birthday bound on a 256-bit output.
//
// Why not QCryptographicHash over the file bytes directly? Because
// FlacTags::write (Step 7) rewrites the file's tag block — the
// audio-frame hash survives that round-trip. See plan §6.2 for the
// long-form justification.

#pragma once

#include <QString>

namespace concerto::library {

class AudioFrameHash {
public:
    // Returns a hex SHA-256 of the first `firstSeconds` of decoded
    // PCM. Format dispatch is via QAudioDecoder. Returns empty on
    // failure (file unreadable, codec unsupported, decode error).
    //
    // The returned string is lowercase hex, 64 characters.
    static QString compute(const QString& path, int firstSeconds = 30);
};

} // namespace concerto::library
