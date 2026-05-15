#include "ArVerify.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QString>
#include <QStringList>
#include <QXmlStreamReader>

#include <algorithm>
#include <cstdio>
#include <utility>
#include <vector>

#include <zlib.h>  // crc32_z — hardware-accelerated on common targets

namespace arverify {

namespace {

// Sum of the decimal digits of n — the per-track term of the CDDB disc id.
uint32_t cddbDigitSum(uint32_t n) {
    uint32_t sum = 0;
    while (n > 0) {
        sum += n % 10;
        n /= 10;
    }
    return sum;
}

// Read a little-endian uint32 from a byte cursor.
uint32_t readLE32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0])
         | static_cast<uint32_t>(p[1]) << 8
         | static_cast<uint32_t>(p[2]) << 16
         | static_cast<uint32_t>(p[3]) << 24;
}

// CRC32 (IEEE polynomial, init/final XOR with 0xFFFFFFFF) — the algorithm
// CTDB stores. Delegate to zlib's crc32_z, which is hardware-accelerated
// (PCLMULQDQ on x86, dedicated CRC32 instructions on ARM64) — the slow
// offset-scan path is CRC32-bound and the speedup is ~10x over a
// byte-at-a-time table.
uint32_t crc32(const uint8_t* data, size_t length) {
    return static_cast<uint32_t>(
        ::crc32_z(0, data, length));
}

// MusicBrainz disc id: SHA-1 over a fixed-width hex blob of [first track]
// [last track][lead-out][offsets 1..99], base64'd with the MusicBrainz
// alphabet (+ / =  ->  . _ -).
std::string musicBrainzDiscId(const DiscToc& toc) {
    char field[9];
    std::string blob;
    blob.reserve(2 + 2 + 100 * 8);

    std::snprintf(field, sizeof(field), "%02X%02X", 1, toc.trackCount());
    blob += field;

    std::snprintf(field, sizeof(field), "%08X", toc.leadoutOffset);
    blob += field;  // slot 0 is the lead-out

    for (int i = 0; i < 99; ++i) {
        const uint32_t offset = (i < toc.trackCount()) ? toc.trackOffsets[i] : 0u;
        std::snprintf(field, sizeof(field), "%08X", offset);
        blob += field;
    }

    QByteArray digest = QCryptographicHash::hash(
        QByteArray::fromRawData(blob.data(), static_cast<int>(blob.size())),
        QCryptographicHash::Sha1);
    QByteArray b64 = digest.toBase64();
    b64.replace('+', '.').replace('/', '_').replace('=', '-');
    return std::string(b64.constData(), static_cast<size_t>(b64.size()));
}

} // namespace

std::optional<DiscToc> reconstructToc(const std::vector<uint64_t>& trackSampleCounts) {
    if (trackSampleCounts.empty())
        return std::nullopt;

    DiscToc toc;
    toc.trackOffsets.reserve(trackSampleCounts.size());

    uint32_t cursor = kLeadInFrames;
    for (uint64_t samples : trackSampleCounts) {
        if (samples == 0 || samples % kSamplesPerFrame != 0)
            return std::nullopt;  // not sector-aligned: TOC cannot be trusted
        toc.trackOffsets.push_back(cursor);
        cursor += static_cast<uint32_t>(samples / kSamplesPerFrame);
    }
    toc.leadoutOffset = cursor;
    return toc;
}

DiscIds computeDiscIds(const DiscToc& toc) {
    DiscIds ids;
    const int n = toc.trackCount();

    // AccurateRip id1/id2 — over LBA offsets (track 1 = 0).
    uint32_t id1 = 0;
    uint32_t id2 = 0;
    for (int i = 0; i < n; ++i) {
        const uint32_t lba = toc.trackOffsets[i] - kLeadInFrames;
        id1 += lba;
        id2 += (lba != 0 ? lba : 1u) * static_cast<uint32_t>(i + 1);
    }
    const uint32_t lbaLeadout = toc.leadoutOffset - kLeadInFrames;
    id1 += lbaLeadout;
    id2 += lbaLeadout * static_cast<uint32_t>(n + 1);
    ids.accurateRipId1 = id1;
    ids.accurateRipId2 = id2;

    // CDDB / FreeDB id — over absolute offsets (track 1 = 150).
    uint32_t checksum = 0;
    for (int i = 0; i < n; ++i)
        checksum += cddbDigitSum(toc.trackOffsets[i] / kFramesPerSecond);
    const uint32_t seconds = toc.leadoutOffset / kFramesPerSecond
                           - toc.trackOffsets[0] / kFramesPerSecond;
    ids.cddbId = ((checksum % 255) << 24) | (seconds << 8) | static_cast<uint32_t>(n);

    ids.musicBrainzDiscId = musicBrainzDiscId(toc);
    return ids;
}

std::string accurateRipPath(const DiscToc& toc, const DiscIds& ids) {
    char id1[9];
    std::snprintf(id1, sizeof(id1), "%08x", static_cast<unsigned>(ids.accurateRipId1));

    char path[64];
    std::snprintf(path, sizeof(path), "%c/%c/%c/dBAR-%03d-%08x-%08x-%08x.bin",
                  id1[7], id1[6], id1[5],
                  toc.trackCount(),
                  static_cast<unsigned>(ids.accurateRipId1),
                  static_cast<unsigned>(ids.accurateRipId2),
                  static_cast<unsigned>(ids.cddbId));
    return path;
}

ArChecksums computeArChecksums(const ArSample* samples, uint64_t frames,
                               int trackNumber, int totalTracks) {
    // The lead-in / lead-out transition regions at the very start of track 1
    // and end of the last track are excluded from the checksum.
    constexpr uint64_t kSkipFrames = 5 * kSamplesPerFrame;  // 5 CD sectors

    uint64_t includeFrom = 0;
    uint64_t includeTo = frames;
    if (trackNumber == 1)
        includeFrom += kSkipFrames;
    if (trackNumber == totalTracks)
        includeTo -= kSkipFrames;

    uint32_t csumLo = 0;
    uint32_t csumHi = 0;
    for (uint64_t i = 0; i < frames; ++i) {
        const uint64_t position = i + 1;  // 1-based position within the track
        if (position < includeFrom || position > includeTo)
            continue;
        const uint64_t product = static_cast<uint64_t>(samples[i]) * position;
        csumLo += static_cast<uint32_t>(product);
        csumHi += static_cast<uint32_t>(product >> 32);
    }

    return ArChecksums{ csumLo, csumLo + csumHi };
}

std::optional<ArChecksums> checksumsAtOffset(
    const ArSample* discSamples, uint64_t discFrames,
    TrackSpan track, int offset,
    int trackNumber, int totalTracks)
{
    // The "offset" shifts where in the disc we *read* the track's samples;
    // the AR weighting (1-based position within the track) is unchanged. If
    // the shifted window pokes past either disc edge, the missing samples
    // would have come from the disc's lead-in/lead-out, which a FLAC rip
    // doesn't contain — caller decides what to do.
    const int64_t shifted0 = static_cast<int64_t>(track.start) + offset;
    const int64_t shifted1 = static_cast<int64_t>(track.end)   + offset;
    if (shifted0 < 0 || shifted1 > static_cast<int64_t>(discFrames))
        return std::nullopt;

    const uint64_t frames = track.end - track.start;
    return computeArChecksums(discSamples + static_cast<uint64_t>(shifted0),
                              frames, trackNumber, totalTracks);
}

OffsetMatch scanForOffset(const ArSample* discSamples, uint64_t discFrames,
                          const std::vector<TrackSpan>& trackSpans,
                          const std::vector<std::vector<uint32_t>>& dbCrcsPerTrack,
                          int searchRangeFrames)
{
    OffsetMatch best;
    const int totalTracks = static_cast<int>(trackSpans.size());
    if (totalTracks < 3 || dbCrcsPerTrack.size() != static_cast<size_t>(totalTracks))
        return best;  // nothing to scan: need at least one middle track and
                      // a CRC list aligned with the TOC

    // Two prefix arrays over the disc samples, both kept mod 2^32 via the
    // natural wraparound of uint32 arithmetic:
    //   A[k] = sum_{i<k} sample[i]
    //   B[k] = sum_{i<k} sample[i] * i        (i is the disc-absolute index)
    // With these, the AR v1 of a track at disc range [a0, a1) is
    //   v1 = (B[a1] - B[a0]) + (1 - t0 - D) * (A[a1] - A[a0])
    // where t0 is the track's *natural* start and D is the offset shift —
    // an O(1) evaluation per (track, offset). v2 is not prefix-summable
    // because its high half depends on every product's carry; the scan
    // therefore returns the best v1 candidate, which the caller should
    // confirm with `checksumsAtOffset` (which computes v1+v2 properly and
    // applies the first/last-track skip regions).
    const uint64_t N = discFrames;
    std::vector<uint32_t> A(N + 1, 0);
    std::vector<uint32_t> B(N + 1, 0);
    for (uint64_t i = 0; i < N; ++i) {
        const uint32_t s = discSamples[i];
        A[i + 1] = A[i] + s;
        B[i + 1] = B[i] + static_cast<uint32_t>(static_cast<uint64_t>(s) * i);
    }

    best.midTrackCount = totalTracks - 2;

    for (int D = -searchRangeFrames; D <= searchRangeFrames; ++D) {
        int hits = 0;
        // Tracks 1 and N (the ones with AR skip regions) are excluded — a
        // fast prefix-sum v1 evaluation can't faithfully exclude them.
        for (int ti = 1; ti < totalTracks - 1; ++ti) {
            const TrackSpan& span = trackSpans[static_cast<size_t>(ti)];
            const int64_t a0 = static_cast<int64_t>(span.start) + D;
            const int64_t a1 = static_cast<int64_t>(span.end)   + D;
            if (a0 < 0 || a1 > static_cast<int64_t>(N))
                continue;  // shifted window crosses a disc edge — track is
                           // unverifiable at this offset, don't count it

            const uint32_t rA = A[a1] - A[a0];
            const uint32_t rB = B[a1] - B[a0];
            // (1 - t0 - D) reduced mod 2^32. Signed-to-unsigned narrowing in
            // C++ is defined as mod 2^N, so the cast does the right thing
            // for negative values.
            const uint32_t coeff = static_cast<uint32_t>(
                int64_t{1} - static_cast<int64_t>(span.start) - D);
            const uint32_t v1 = rB + coeff * rA;

            const auto& crcs = dbCrcsPerTrack[static_cast<size_t>(ti)];
            if (std::find(crcs.begin(), crcs.end(), v1) != crcs.end())
                ++hits;
        }
        if (hits > best.hits) {
            best.hits = hits;
            best.offset = D;
        }
    }

    return best;
}

std::optional<int> scanForOffsetSlow(
    const ArSample* discSamples, uint64_t discFrames,
    const std::vector<TrackSpan>& trackSpans,
    const std::vector<std::vector<uint32_t>>& dbCrcsPerTrack,
    int searchRangeFrames)
{
    const int totalTracks = static_cast<int>(trackSpans.size());
    if (totalTracks < 3 || dbCrcsPerTrack.size() != static_cast<size_t>(totalTracks))
        return std::nullopt;

    // Use the shortest middle tracks: cheapest to checksum at each offset.
    // Two probes makes a false-positive collision vanishingly unlikely
    // (two unrelated 32-bit CRCs lining up at the same offset is ~2^-32
    // per offset, and we only consider offsets that pass the first probe).
    std::vector<int> mids;
    mids.reserve(static_cast<size_t>(totalTracks - 2));
    for (int ti = 1; ti < totalTracks - 1; ++ti) mids.push_back(ti);
    std::sort(mids.begin(), mids.end(), [&trackSpans](int a, int b) {
        const auto& sa = trackSpans[static_cast<size_t>(a)];
        const auto& sb = trackSpans[static_cast<size_t>(b)];
        return (sa.end - sa.start) < (sb.end - sb.start);
    });
    if (mids.empty())
        return std::nullopt;
    const int probeA = mids[0];
    const int probeB = mids.size() >= 2 ? mids[1] : probeA;

    auto matchesAt = [&](int track, int offset) -> bool {
        const auto sums = checksumsAtOffset(
            discSamples, discFrames,
            trackSpans[static_cast<size_t>(track)], offset,
            track + 1, totalTracks);
        if (!sums)
            return false;
        const auto& crcs = dbCrcsPerTrack[static_cast<size_t>(track)];
        return std::find(crcs.begin(), crcs.end(), sums->v1) != crcs.end()
            || std::find(crcs.begin(), crcs.end(), sums->v2) != crcs.end();
    };

    // First-match wins. Most offsets fail at the cheap probeA check, so
    // the second probe runs only on the small set of candidates that pass.
    for (int D = -searchRangeFrames; D <= searchRangeFrames; ++D) {
        if (matchesAt(probeA, D) && (probeB == probeA || matchesAt(probeB, D)))
            return D;
    }
    return std::nullopt;
}

std::vector<ArDiscEntry> parseAccurateRipResponse(const uint8_t* data, size_t length) {
    std::vector<ArDiscEntry> entries;

    // Each entry: 1-byte track count, three 4-byte ids, then per track a
    // 1-byte confidence followed by a 4-byte crc.
    size_t pos = 0;
    while (pos + 13 <= length) {
        const int trackCount = data[pos];
        const size_t entrySize = 1 + 12 + static_cast<size_t>(trackCount) * 9;
        if (trackCount <= 0 || pos + entrySize > length)
            break;  // malformed or truncated — keep whatever parsed cleanly

        ArDiscEntry entry;
        entry.trackCount = trackCount;
        entry.discId1 = readLE32(data + pos + 1);
        entry.discId2 = readLE32(data + pos + 5);
        entry.cddbId  = readLE32(data + pos + 9);

        entry.tracks.reserve(static_cast<size_t>(trackCount));
        size_t cursor = pos + 13;
        for (int t = 0; t < trackCount; ++t) {
            ArTrackEntry track;
            track.confidence = data[cursor];
            track.crc = readLE32(data + cursor + 1);
            entry.tracks.push_back(track);
            cursor += 9;
        }

        entries.push_back(std::move(entry));
        pos += entrySize;
    }

    return entries;
}

std::string ctdbLookupPath(const DiscToc& toc) {
    std::string path = "lookup2.php?version=3&ctdb=1&fuzzy=0&toc=";
    char buf[16];
    for (uint32_t off : toc.trackOffsets) {
        std::snprintf(buf, sizeof(buf), "%u:", off - kLeadInFrames);
        path += buf;
    }
    std::snprintf(buf, sizeof(buf), "%u", toc.leadoutOffset - kLeadInFrames);
    path += buf;
    return path;
}

std::optional<uint32_t> ctdbChecksumAtOffset(
    const ArSample* discSamples, uint64_t discFrames,
    TrackSpan track, int offset)
{
    const int64_t shifted0 = static_cast<int64_t>(track.start) + offset;
    const int64_t shifted1 = static_cast<int64_t>(track.end)   + offset;
    if (shifted0 < 0 || shifted1 > static_cast<int64_t>(discFrames))
        return std::nullopt;

    // CTDB CRCs are computed over the raw little-endian byte layout of the
    // packed stereo samples (L_lo, L_hi, R_lo, R_hi, ...), which matches
    // a CD-DA WAV's PCM region byte-for-byte on little-endian hosts.
    const uint64_t frames = track.end - track.start;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(
        discSamples + static_cast<uint64_t>(shifted0));
    return crc32(bytes, static_cast<size_t>(frames * sizeof(ArSample)));
}

std::optional<int> scanForOffsetCtdb(
    const ArSample* discSamples, uint64_t discFrames,
    const std::vector<TrackSpan>& trackSpans,
    const std::vector<CtdbEntry>& ctdbEntries,
    int searchRangeFrames)
{
    const int totalTracks = static_cast<int>(trackSpans.size());
    if (totalTracks < 3 || ctdbEntries.empty())
        return std::nullopt;

    // Same two-shortest-middle-tracks probe strategy as scanForOffsetSlow.
    std::vector<int> mids;
    mids.reserve(static_cast<size_t>(totalTracks - 2));
    for (int ti = 1; ti < totalTracks - 1; ++ti) mids.push_back(ti);
    std::sort(mids.begin(), mids.end(), [&trackSpans](int a, int b) {
        const auto& sa = trackSpans[static_cast<size_t>(a)];
        const auto& sb = trackSpans[static_cast<size_t>(b)];
        return (sa.end - sa.start) < (sb.end - sb.start);
    });
    if (mids.empty())
        return std::nullopt;
    const int probeA = mids[0];
    const int probeB = mids.size() >= 2 ? mids[1] : probeA;

    // For probe A at this offset, the max confidence among entries whose
    // track-A CRC matches our computed value. 0 means no entry matched.
    auto probeAConfidence = [&](int offset) -> int {
        const auto crc = ctdbChecksumAtOffset(
            discSamples, discFrames,
            trackSpans[static_cast<size_t>(probeA)], offset);
        if (!crc) return 0;
        int best = 0;
        for (const auto& entry : ctdbEntries) {
            if (probeA < static_cast<int>(entry.trackCrcs.size())
                && entry.trackCrcs[static_cast<size_t>(probeA)] == *crc)
                best = std::max(best, entry.confidence);
        }
        return best;
    };
    auto probeBMatches = [&](int offset) -> bool {
        const auto crc = ctdbChecksumAtOffset(
            discSamples, discFrames,
            trackSpans[static_cast<size_t>(probeB)], offset);
        if (!crc) return false;
        for (const auto& entry : ctdbEntries) {
            if (probeB < static_cast<int>(entry.trackCrcs.size())
                && entry.trackCrcs[static_cast<size_t>(probeB)] == *crc)
                return true;
        }
        return false;
    };

    int bestConfidence = 0;
    int bestOffset = 0;
    for (int D = -searchRangeFrames; D <= searchRangeFrames; ++D) {
        const int conf = probeAConfidence(D);
        if (conf <= bestConfidence)
            continue;  // not better than what we have — no point checking probe B
        if (probeB != probeA && !probeBMatches(D))
            continue;
        bestConfidence = conf;
        bestOffset = D;
    }
    return bestConfidence > 0 ? std::optional<int>{bestOffset} : std::nullopt;
}

std::vector<CtdbEntry> parseCtdbResponse(const uint8_t* data, size_t length) {
    std::vector<CtdbEntry> entries;

    QXmlStreamReader reader(QByteArray::fromRawData(
        reinterpret_cast<const char*>(data), static_cast<int>(length)));
    while (!reader.atEnd()) {
        reader.readNext();
        if (!reader.isStartElement() || reader.name() != QLatin1String("entry"))
            continue;

        const auto attrs = reader.attributes();
        CtdbEntry entry;
        entry.confidence = attrs.value(QLatin1String("confidence")).toInt();

        // `trackcrcs` is a space-separated list of 8-hex-digit CRC32 values
        // in track order. Any malformed token discards the whole entry —
        // a partial CRC list would silently misalign track comparisons.
        const QString crcsStr = attrs.value(QLatin1String("trackcrcs")).toString();
        const QStringList parts = crcsStr.split(QChar(' '), Qt::SkipEmptyParts);
        entry.trackCrcs.reserve(static_cast<size_t>(parts.size()));
        bool malformed = false;
        for (const QString& p : parts) {
            bool ok = false;
            const uint32_t v = p.toUInt(&ok, 16);
            if (!ok) { malformed = true; break; }
            entry.trackCrcs.push_back(v);
        }
        if (malformed || entry.trackCrcs.empty())
            continue;
        entries.push_back(std::move(entry));
    }
    return entries;
}

} // namespace arverify
