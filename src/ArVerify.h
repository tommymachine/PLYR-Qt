// AccurateRip verification core: reconstruct a CD table of contents from
// sector-aligned PCM track lengths, derive the disc fingerprints the lookup
// services key on, and compute AccurateRip track checksums.
//
// Pure computation — no file I/O, no networking. Feed it sample counts and
// decoded PCM; the FLAC-decode and HTTP layers live elsewhere.
//
// Algorithm references (used to verify correctness, not transcribed):
//   - AccurateRip disc ids / CDDB id : whipper, whipper/image/table.py
//   - AccurateRip track checksum     : hydrogenaudio forum thread 97603
//   - MusicBrainz disc id            : musicbrainz.org/doc/Disc_ID_Calculation
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace arverify {

// CD geometry. A "frame" (sector) is the disc's addressable unit: 588 stereo
// 16-bit samples, 1/75 second of audio.
inline constexpr uint32_t kSamplesPerFrame = 588;
inline constexpr uint32_t kFramesPerSecond = 75;
inline constexpr uint32_t kLeadInFrames    = 150;  // standard 2-second lead-in

// One stereo frame packed for AccurateRip math: left channel in the low 16
// bits, right channel in the high 16 bits — little-endian WAV order. All AR
// checksums are defined over this sample shape.
using ArSample = uint32_t;
inline ArSample packStereo(int16_t left, int16_t right) {
    return static_cast<uint32_t>(static_cast<uint16_t>(left))
         | (static_cast<uint32_t>(static_cast<uint16_t>(right)) << 16);
}

// A reconstructed table of contents. Offsets are absolute frame addresses:
// track 1 sits at kLeadInFrames and leadoutOffset is one past the last track.
struct DiscToc {
    std::vector<uint32_t> trackOffsets;
    uint32_t leadoutOffset = 0;
    int trackCount() const { return static_cast<int>(trackOffsets.size()); }
};

// Rebuild a TOC from per-track sample counts in track order. Every count must
// be a whole number of CD sectors; a non-aligned track means the rip didn't
// come cleanly off a real CD and the TOC would be wrong, so nullopt.
std::optional<DiscToc> reconstructToc(const std::vector<uint64_t>& trackSampleCounts);

// The three disc fingerprints the lookup services key on. The offset
// convention differs per id: AccurateRip uses LBA offsets (track 1 = 0) while
// CDDB and MusicBrainz use absolute offsets (track 1 = 150). Mixing them up
// yields a plausible-looking id that simply never matches.
struct DiscIds {
    uint32_t accurateRipId1 = 0;
    uint32_t accurateRipId2 = 0;
    uint32_t cddbId = 0;
    std::string musicBrainzDiscId;  // 28-char, MusicBrainz base64 variant
};
DiscIds computeDiscIds(const DiscToc& toc);

// Path component of the AccurateRip lookup URL, e.g.
// "c/f/2/dBAR-006-000fa2fc-0053e2cc-4b0f6506.bin". Prefix with
// "http://www.accuraterip.com/accuraterip/" for the full URL.
std::string accurateRipPath(const DiscToc& toc, const DiscIds& ids);

// AccurateRip per-track checksums. A database entry stores one value per
// track; submitter tooling decides whether it is v1 or v2, so a match
// against either counts as accurate.
struct ArChecksums {
    uint32_t v1 = 0;
    uint32_t v2 = 0;
};

// Compute the AccurateRip v1/v2 checksums for a single track. `samples`
// points at `frames` packed stereo samples (see `ArSample`). `trackNumber`
// is 1-based. Per the AR definition the first track skips its first 5
// sectors (2940 samples) and the last track its last 5.
ArChecksums computeArChecksums(const ArSample* samples, uint64_t frames,
                               int trackNumber, int totalTracks);

// One track's absolute position in the concatenated disc-wide sample stream,
// in stereo frames: [start, end), with `end` one past the last frame.
struct TrackSpan {
    uint64_t start = 0;
    uint64_t end = 0;
};

// Compute v1/v2 for a track that is *read* with a sample-domain offset. At
// offset D, the track's frame range [t0, t1) inside the disc samples instead
// reads from [t0 + D, t1 + D). Returns nullopt if that shifted window
// crosses either disc edge (the track is unverifiable at this offset — the
// rip lacks lead-in / lead-out samples to fill the gap).
//
// The 1-based position used by the AR checksum is the track-internal
// position (1..frames), not the disc-absolute one — i.e. the offset shifts
// *which* samples are read but not *how* they are weighted. This matches
// dBpoweramp, EAC and CTDB.
std::optional<ArChecksums> checksumsAtOffset(
    const ArSample* discSamples, uint64_t discFrames,
    TrackSpan track, int offset,
    int trackNumber, int totalTracks);

// Outcome of a disc-wide pressing-offset scan.
struct OffsetMatch {
    int offset = 0;         // sample offset (in stereo frames) with the most hits
    int hits = 0;           // middle tracks whose v1 matched at that offset
    int midTrackCount = 0;  // total middle tracks the scan considered
};

// Find the disc-wide sample offset under which middle tracks (2..N-1) best
// match the AR database. Uses an O(disc) prefix-sum prepass so each
// (track, offset) v1 evaluation is O(1). v2 is *not* prefix-summable (the
// high-half carry depends on every product), so the scan is v1-only; the
// returned offset should then be confirmed track-by-track with
// `checksumsAtOffset`, which computes both v1 and v2 and applies the
// first/last-track skip regions.
//
// Tracks 1 and N are excluded from the scan because their AR skip regions
// would make a fast v1 evaluation imprecise.
//
// `dbCrcsPerTrack` is indexed by 0-based track number and is the union of
// CRCs seen in any pressing of that track in the AR response.
//
// `searchRangeFrames` is the +/- search width. AR drive offsets cluster in
// [-1500, +1500]; 3000 is a comfortable default covering observed pressing
// variation too.
OffsetMatch scanForOffset(const ArSample* discSamples, uint64_t discFrames,
                          const std::vector<TrackSpan>& trackSpans,
                          const std::vector<std::vector<uint32_t>>& dbCrcsPerTrack,
                          int searchRangeFrames = 3000);

// Fallback scan for discs whose DB entries store v2 CRCs that the fast v1
// prefix-sum scan can't reach. Probes the two shortest middle tracks at
// each offset with a full v1+v2 computation; returns the first offset where
// both tracks match (v1 or v2) in the DB. O(probedSamples * offsets) — much
// slower than `scanForOffset`, so call only when that returns hits == 0.
//
// Returns nullopt if no offset within the search range satisfies both
// probes — typical for discs whose pressing isn't in the AR database at all,
// or rips with real errors.
std::optional<int> scanForOffsetSlow(
    const ArSample* discSamples, uint64_t discFrames,
    const std::vector<TrackSpan>& trackSpans,
    const std::vector<std::vector<uint32_t>>& dbCrcsPerTrack,
    int searchRangeFrames = 3000);

// One track's row inside an AccurateRip response.
struct ArTrackEntry {
    uint8_t  confidence = 0;  // how many submitters agreed on this crc
    uint32_t crc = 0;
};

// One pressing's worth of data in an AccurateRip response. A single lookup
// can return several — distinct pressings that happen to share a TOC.
struct ArDiscEntry {
    int trackCount = 0;
    uint32_t discId1 = 0;
    uint32_t discId2 = 0;
    uint32_t cddbId = 0;
    std::vector<ArTrackEntry> tracks;
};

// Parse the packed binary body of an AccurateRip ".bin" response. Returns an
// empty vector on a malformed or truncated body.
std::vector<ArDiscEntry> parseAccurateRipResponse(const uint8_t* data, size_t length);

// --- CTDB (CueTools Database) -----------------------------------------------
// Second consensus pool, independent of AccurateRip. CTDB stores plain
// CRC32 (IEEE polynomial 0xEDB88320, zlib-compatible) over the raw
// little-endian PCM bytes of each track at its drive-offset-shifted
// position. No AR-style skip regions, no track-relative weighting — the
// offset shift is the only thing common with AR.

// CTDB lookup path/query against `db.cuetools.net`. Build the full URL by
// prepending "http://db.cuetools.net/". The TOC encoding is LBA track
// starts (track 1 = 0) followed by the LBA leadout, colon-separated —
// distinct from the 150-based form used by CDDB and MusicBrainz.
std::string ctdbLookupPath(const DiscToc& toc);

// CRC32 of the track's PCM bytes at sample offset `offset` within the
// disc. Returns nullopt if the shifted window crosses a disc edge.
std::optional<uint32_t> ctdbChecksumAtOffset(
    const ArSample* discSamples, uint64_t discFrames,
    TrackSpan track, int offset);

// One pressing's worth of CTDB data. `trackCrcs` is in track order.
struct CtdbEntry {
    int confidence = 0;
    std::vector<uint32_t> trackCrcs;
};

// Parse the XML body of a CTDB lookup2.php response. Returns an empty
// vector on malformed XML or zero entries.
std::vector<CtdbEntry> parseCtdbResponse(const uint8_t* data, size_t length);

// Find the disc-wide sample offset where this rip's CRC32 matches CTDB's
// stored CRCs. CTDB and AccurateRip group submissions by independent
// pressing populations, so the CTDB pressing offset routinely differs
// from AR's for the same TOC — verifying both at the AR offset alone
// misses CTDB hits in the common case. Linear in samples * offsets over
// the two shortest middle tracks (CRC32 is not prefix-summable). Among
// offsets where both probes match, prefers the one whose probe-A entry
// has the highest confidence — a high-confidence canonical pressing is
// nearly always what the user wants, even when low-confidence one-off
// pressings happen to checksum-collide at unrelated offsets.
std::optional<int> scanForOffsetCtdb(
    const ArSample* discSamples, uint64_t discFrames,
    const std::vector<TrackSpan>& trackSpans,
    const std::vector<CtdbEntry>& ctdbEntries,
    int searchRangeFrames = 3000);

} // namespace arverify
