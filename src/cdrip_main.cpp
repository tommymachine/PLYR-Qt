// cdrip_cli — incremental CLI harness for plyr::cd::CdDevice / CdShield.
//
// Modes:
//   (default)    Enumerate visible optical drives.
//   --toc        Enumerate, open drive 0, read the TOC, print per-track
//                start LBAs, and compute the four disc IDs (AccurateRip
//                id1/id2, CDDB, MusicBrainz) so they can be cross-checked
//                against `arverify_cli` on an existing rip of the same disc.
//   --read [N]   Open drive 0, read the TOC, then stream the disc end-to-
//                end via readSectors() in ~27-sector chunks. With an
//                explicit N reads only the first N sectors (useful for
//                smoke-testing). Verifies that the cumulative sample
//                count equals (leadout - firstTrack.startLba) * 588.
//   --rip DIR    Full no-offset rip: read the whole disc into memory,
//                slice by TOC track boundaries, FLAC-encode each track
//                (no tags, no drive-offset correction) into DIR/. Run
//                `arverify_cli DIR` afterwards to confirm the rip is
//                bit-accurate at *some* offset — the verifier's offset
//                scan compensates for whatever the SuperDrive's true
//                offset turns out to be. Drive-offset correction is
//                landed in the step that follows.
//   --shield     Activate CdShield (Disk Arbitration claim on audio CDs)
//                and idle on the main run loop until Ctrl-C. Used to
//                manually verify that inserting a CD does NOT pop up
//                Music.app while the shield is active.
//   --db-info    Diagnostic: print the bundled AccurateRip drive-offset
//                table stats and a small sample of entries. For "did the
//                resource embed?" / "is my drive recognized?" debugging.

#include "ArVerify.h"
#include "CdDevice.h"
#include "CdShield.h"
#include "DriveOffsetDb.h"
#include "FlacEncode.h"
#include "MusicBrainz.h"

#include <QCoreApplication>
#include <QNetworkAccessManager>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdlib>     // std::abs
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#ifdef CONCERTO_HAS_CD_SHIELD
  #include <CoreFoundation/CoreFoundation.h>
  #include <csignal>

  // CFRunLoopStop is not strictly async-signal-safe, but it's the
  // standard idiom for cleanly exiting CFRunLoopRun on SIGINT and
  // works fine on macOS in practice.
  static void onSigint(int) {
      CFRunLoopStop(CFRunLoopGetMain());
  }
#endif

static int runEnumerate() {
    const auto drives = plyr::cd::CdDevice::enumerate();
    std::printf("found %zu optical drive%s with media\n",
                drives.size(), drives.size() == 1 ? "" : "s");
    for (size_t i = 0; i < drives.size(); ++i) {
        const auto& d = drives[i];
        std::printf("  [%zu] /dev/%s  vendor=%-12s  product=%-24s  rev=%s%s\n",
                    i, d.id.c_str(),
                    d.vendor.empty()   ? "?"  : d.vendor.c_str(),
                    d.product.empty()  ? "?"  : d.product.c_str(),
                    d.revision.empty() ? "?"  : d.revision.c_str(),
                    d.hasMedia ? "" : "  [no media]");
    }
    return drives.empty() ? EXIT_FAILURE : EXIT_SUCCESS;
}

static int runShield() {
#ifdef CONCERTO_HAS_CD_SHIELD
    plyr::cd::CdShield shield;
    shield.start();
    std::fprintf(stderr,
        "[shield] active. insert a CD — Music.app should NOT launch.\n"
        "[shield] press Ctrl-C to release.\n");

    std::signal(SIGINT, onSigint);
    CFRunLoopRun();

    shield.stop();
    std::fprintf(stderr, "[shield] released.\n");
    return EXIT_SUCCESS;
#else
    std::fprintf(stderr, "cdrip_cli: --shield not supported on this platform\n");
    return EXIT_FAILURE;
#endif
}

// Bridge our LBA-based TOC into the arverify form, which uses absolute
// frame addresses (track 1 sits at kLeadInFrames = 150, lead-out is one
// past the last track). The arverify disc-ID math only needs offsets.
static arverify::DiscToc toArverifyToc(const plyr::cd::Toc& src) {
    arverify::DiscToc out;
    out.trackOffsets.reserve(src.tracks.size());
    for (const auto& t : src.tracks) {
        out.trackOffsets.push_back(t.startLba + arverify::kLeadInFrames);
    }
    out.leadoutOffset = src.leadOutLba + arverify::kLeadInFrames;
    return out;
}

static int runReadToc() {
    const auto drives = plyr::cd::CdDevice::enumerate();
    if (drives.empty()) {
        std::fprintf(stderr, "cdrip_cli: no optical drives with media\n");
        return EXIT_FAILURE;
    }
    const auto& d = drives[0];
    std::printf("opening /dev/%s  (%s / %s rev %s)\n",
                d.id.c_str(),
                d.vendor.empty() ? "?" : d.vendor.c_str(),
                d.product.empty() ? "?" : d.product.c_str(),
                d.revision.empty() ? "?" : d.revision.c_str());

    auto dev = plyr::cd::CdDevice::open(d.id);
    if (!dev) {
        std::fprintf(stderr, "open(%s) failed (errno=%d)\n",
                     d.id.c_str(), errno);
        return EXIT_FAILURE;
    }

    const auto toc = dev->readToc();
    if (!toc) {
        std::fprintf(stderr, "readToc: %s\n", dev->lastDeviceError().c_str());
        return EXIT_FAILURE;
    }

    std::printf("\nTOC: %zu track%s, lead-out at LBA %u\n",
                toc->tracks.size(), toc->tracks.size() == 1 ? "" : "s",
                toc->leadOutLba);
    for (const auto& t : toc->tracks) {
        // Display MSF using the standard 2-second lead-in convention
        // (track 1 LBA 0 == MSF 00:02:00).
        const uint32_t abs = t.startLba + 150;
        const uint32_t mm  = abs / (75 * 60);
        const uint32_t ss  = (abs / 75) % 60;
        const uint32_t ff  = abs % 75;
        std::printf("  track %2u  LBA %7u  (%02u:%02u.%02u)  %s%s\n",
                    t.trackNumber, t.startLba, mm, ss, ff,
                    t.isData ? "DATA " : "AUDIO",
                    t.preEmphasis ? "  +preemphasis" : "");
    }
    {
        const uint32_t abs = toc->leadOutLba + 150;
        const uint32_t mm  = abs / (75 * 60);
        const uint32_t ss  = (abs / 75) % 60;
        const uint32_t ff  = abs % 75;
        std::printf("  lead-out  LBA %7u  (%02u:%02u.%02u)\n",
                    toc->leadOutLba, mm, ss, ff);
    }

    const arverify::DiscIds ids = arverify::computeDiscIds(toArverifyToc(*toc));
    std::printf("\ndisc IDs:\n");
    std::printf("  AccurateRip:   id1=%08x  id2=%08x\n",
                ids.accurateRipId1, ids.accurateRipId2);
    std::printf("  CDDB:          %08x\n", ids.cddbId);
    std::printf("  MusicBrainz:   %s\n", ids.musicBrainzDiscId.c_str());

    const auto offsetOpt = plyr::cd::lookupDriveOffset(d.vendor, d.product);
    if (offsetOpt) {
        std::printf("\ndrive offset:  %+d samples (would be applied by --rip)\n",
                    *offsetOpt);
    } else {
        std::printf("\ndrive offset:  unknown for %s / %s — --rip would land at 0,\n"
                    "               arverify_cli's offset scan will compensate.\n",
                    d.vendor.c_str(), d.product.c_str());
    }

    return EXIT_SUCCESS;
}

// Stream sectors from the inserted disc and confirm the count matches
// what the TOC implies. If `cap` is non-zero, stop after that many
// sectors (smoke-testing); otherwise read to the lead-out.
static int runReadAll(uint32_t cap) {
    const auto drives = plyr::cd::CdDevice::enumerate();
    if (drives.empty()) {
        std::fprintf(stderr, "cdrip_cli: no optical drives with media\n");
        return EXIT_FAILURE;
    }
    const auto& d = drives[0];
    std::printf("opening /dev/%s  (%s / %s rev %s)\n",
                d.id.c_str(),
                d.vendor.empty()   ? "?" : d.vendor.c_str(),
                d.product.empty()  ? "?" : d.product.c_str(),
                d.revision.empty() ? "?" : d.revision.c_str());

    auto dev = plyr::cd::CdDevice::open(d.id);
    if (!dev) {
        std::fprintf(stderr, "open(%s) failed (errno=%d)\n", d.id.c_str(), errno);
        return EXIT_FAILURE;
    }

    const auto toc = dev->readToc();
    if (!toc || toc->tracks.empty()) {
        std::fprintf(stderr, "readToc: %s\n", dev->lastDeviceError().c_str());
        return EXIT_FAILURE;
    }

    const int32_t  firstLba    = static_cast<int32_t>(toc->tracks.front().startLba);
    const uint32_t totalToRead = (cap > 0)
        ? std::min<uint32_t>(cap, toc->leadOutLba - toc->tracks.front().startLba)
        : (toc->leadOutLba - toc->tracks.front().startLba);
    const uint64_t expectedSamples = uint64_t{totalToRead} * 588;

    std::printf("\nreading %u sectors  (LBA %d .. %d)  ~%.1f MiB raw audio\n",
                totalToRead, firstLba, firstLba + int(totalToRead),
                static_cast<double>(uint64_t{totalToRead} * 2352) / (1024.0 * 1024.0));

    // Chunk size: 27 sectors (~62 KiB) is the cdparanoia default and
    // sits comfortably under any USB / SCSI transfer cap. Each ioctl
    // round-trip dominates short reads, so don't go much smaller.
    constexpr uint32_t kChunk = 27;
    std::vector<uint8_t> buf(kChunk * 2352);

    uint32_t remaining   = totalToRead;
    int32_t  lba         = firstLba;
    uint64_t samplesRead = 0;
    uint32_t nextReport  = std::max<uint32_t>(totalToRead / 10, 1);  // ~10 lines
    uint32_t readSoFar   = 0;

    while (remaining > 0) {
        const uint32_t n = std::min(remaining, kChunk);
        std::span<uint8_t> view(buf.data(), uint64_t{n} * 2352);

        const auto r = dev->readSectors(lba, n, view, /*wantC2=*/false);
        if (r.status != plyr::cd::ReadStatus::Ok) {
            std::fprintf(stderr,
                "\nread failed at LBA %d (%u sectors requested, %u returned): %s\n",
                lba, n, r.sectorsRead, dev->lastDeviceError().c_str());
            return EXIT_FAILURE;
        }
        if (r.sectorsRead != n) {
            std::fprintf(stderr,
                "\nshort read at LBA %d: asked %u got %u\n",
                lba, n, r.sectorsRead);
            return EXIT_FAILURE;
        }

        samplesRead += uint64_t{r.sectorsRead} * 588;
        remaining   -= r.sectorsRead;
        lba         += static_cast<int32_t>(r.sectorsRead);
        readSoFar   += r.sectorsRead;
        if (readSoFar >= nextReport) {
            std::printf("  %3u%%  LBA %d  (%llu samples)\n",
                        readSoFar * 100u / totalToRead, lba,
                        static_cast<unsigned long long>(samplesRead));
            std::fflush(stdout);
            nextReport += std::max<uint32_t>(totalToRead / 10, 1);
        }
    }

    std::printf("\ntotal samples read: %llu  (expected %llu)  %s\n",
                static_cast<unsigned long long>(samplesRead),
                static_cast<unsigned long long>(expectedSamples),
                samplesRead == expectedSamples ? "OK" : "MISMATCH");
    return samplesRead == expectedSamples ? EXIT_SUCCESS : EXIT_FAILURE;
}

// One row in the rip log's "had to zero-fill" summary.
struct ZeroFilledRange {
    int32_t  lba;
    uint32_t sectors;
};

// Read `count` sectors at `lba` with up to `kMaxRetries` re-attempts on
// transient errors (EIO / EBUSY). Non-retriable failures (out-of-range,
// abort, fatal device) return immediately. After the retry budget is
// exhausted the caller's buffer is zero-filled and `outZeroFilled` is
// written so the caller can record the range and keep going — same
// strategy cdparanoia / EAC use for "unrecoverable" sectors. AR's first/
// last 5-sector skip windows + the disc-wide AR/CTDB cross-check are
// the actual safety nets.
static bool readWithRetry(plyr::cd::CdDevice& dev,
                          int32_t lba, uint32_t count,
                          std::span<uint8_t> buf,
                          bool& outZeroFilled,
                          std::string& outLastError) {
    constexpr int kMaxRetries = 2;
    plyr::cd::ReadResult r;
    for (int attempt = 0; attempt <= kMaxRetries; ++attempt) {
        r = dev.readSectors(lba, count, buf, /*wantC2=*/false);
        if (r.status == plyr::cd::ReadStatus::Ok && r.sectorsRead == count) {
            outZeroFilled = false;
            if (attempt > 0) {
                std::printf("  recovered LBA %d (+%u) on retry %d\n",
                            lba, count, attempt);
                std::fflush(stdout);
            }
            return true;
        }
        // Non-transient — no point hammering.
        if (r.status == plyr::cd::ReadStatus::OutOfRange
            || r.status == plyr::cd::ReadStatus::FatalDeviceError
            || r.status == plyr::cd::ReadStatus::Aborted) {
            break;
        }
    }
    outLastError    = dev.lastDeviceError();
    outZeroFilled   = true;
    std::memset(buf.data(), 0, buf.size());
    return false;
}

// Derive a Vorbis-comment set for one track from a chosen MusicBrainz
// release + the disc's MB ID. Same fields rip_cd.sh wrote: TITLE,
// ARTIST, ALBUM, ALBUMARTIST, DATE, TRACKNUMBER, TRACKTOTAL,
// DISCNUMBER/DISCTOTAL (multi-disc only), MUSICBRAINZ_*.
//
// Duplicated from mbquery_main.cpp's tagsForTrack — should land in a
// shared header once the rip code stabilizes.
static std::vector<flacencode::VorbisTag> tagsForTrack(
    const musicbrainz::Release& rel,
    int trackIndex0Based,
    const std::string& mbDiscId)
{
    std::vector<flacencode::VorbisTag> tags;
    auto add = [&](const char* field, const std::string& value) {
        if (!value.empty()) tags.emplace_back(field, value);
    };

    const auto& tracks = rel.disc.tracks;
    if (trackIndex0Based >= 0
        && trackIndex0Based < static_cast<int>(tracks.size())) {
        add("TITLE", tracks[trackIndex0Based].title);
        if (!tracks[trackIndex0Based].recordingId.empty())
            add("MUSICBRAINZ_TRACKID", tracks[trackIndex0Based].recordingId);
    }
    add("ARTIST", rel.artist);
    add("ALBUMARTIST", rel.artist);
    add("ALBUM", rel.title);
    add("DATE", rel.date);
    tags.emplace_back("TRACKNUMBER", std::to_string(trackIndex0Based + 1));
    tags.emplace_back("TRACKTOTAL", std::to_string(tracks.size()));
    if (rel.disc.totalCount > 1) {
        tags.emplace_back("DISCNUMBER", std::to_string(rel.disc.position));
        tags.emplace_back("DISCTOTAL", std::to_string(rel.disc.totalCount));
    }
    add("MUSICBRAINZ_DISCID", mbDiscId);
    add("MUSICBRAINZ_ALBUMID", rel.id);
    return tags;
}

// Full disc rip with drive-offset correction. Read range is widened by
// `padSectors` at each disc edge so the slicing window can shift up to
// |offset| sample frames in either direction; failed pad reads (lead-in
// is unsupported on most slim USB drives, lead-out beyond the TOC's
// reported leadOut is iffy too) are left zero-padded. Both AR and CTDB
// skip 5 sectors at the disc edges, so a few frames of zero-padding
// inside that region are invisible to the AR check.
static int runRip(const char* outDir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(outDir, ec);
    if (ec) {
        std::fprintf(stderr, "cdrip_cli: cannot create %s: %s\n",
                     outDir, ec.message().c_str());
        return EXIT_FAILURE;
    }

    const auto drives = plyr::cd::CdDevice::enumerate();
    if (drives.empty()) {
        std::fprintf(stderr, "cdrip_cli: no optical drives with media\n");
        return EXIT_FAILURE;
    }
    const auto& d = drives[0];
    std::printf("opening /dev/%s  (%s / %s rev %s)\n",
                d.id.c_str(),
                d.vendor.empty()   ? "?" : d.vendor.c_str(),
                d.product.empty()  ? "?" : d.product.c_str(),
                d.revision.empty() ? "?" : d.revision.c_str());

    auto dev = plyr::cd::CdDevice::open(d.id);
    if (!dev) {
        std::fprintf(stderr, "open(%s) failed (errno=%d)\n", d.id.c_str(), errno);
        return EXIT_FAILURE;
    }

    const auto toc = dev->readToc();
    if (!toc || toc->tracks.empty()) {
        std::fprintf(stderr, "readToc: %s\n", dev->lastDeviceError().c_str());
        return EXIT_FAILURE;
    }
    for (const auto& t : toc->tracks) {
        if (t.isData) {
            std::fprintf(stderr,
                "track %u is a data track — v1 rips audio-only CDs.\n",
                t.trackNumber);
            return EXIT_FAILURE;
        }
    }

    const int32_t  firstLba     = static_cast<int32_t>(toc->tracks.front().startLba);
    const uint32_t totalSectors = toc->leadOutLba - toc->tracks.front().startLba;

    // MusicBrainz lookup — done up front so a missing/slow network shows
    // up before the ~10-min disc read, not after. The rip itself stays
    // bit-accurate either way; missing MB just means tagless output.
    arverify::DiscToc arToc;
    arToc.trackOffsets.reserve(toc->tracks.size());
    for (const auto& t : toc->tracks) {
        arToc.trackOffsets.push_back(t.startLba + arverify::kLeadInFrames);
    }
    arToc.leadoutOffset = toc->leadOutLba + arverify::kLeadInFrames;
    const arverify::DiscIds ids = arverify::computeDiscIds(arToc);

    std::printf("\nMB disc id: %s\n", ids.musicBrainzDiscId.c_str());
    QNetworkAccessManager nam;
    const auto releases = musicbrainz::lookupByDiscId(
        nam, ids.musicBrainzDiscId, static_cast<int>(toc->tracks.size()));
    const musicbrainz::Release* chosen = nullptr;
    if (!releases.empty()) {
        chosen = &releases.front();
        std::printf("MB release: %s — %s%s%s  (disc %d of %d)\n",
                    chosen->artist.c_str(), chosen->title.c_str(),
                    chosen->date.empty() ? "" : "  ",
                    chosen->date.c_str(),
                    chosen->disc.position, chosen->disc.totalCount);
    } else {
        std::printf("MB: no match — encoding without tags\n");
    }

    // Drive-offset lookup. nullopt -> rip at offset 0 and let the verifier
    // scan it out post-hoc, same as the no-offset case.
    const auto offsetOpt = plyr::cd::lookupDriveOffset(d.vendor, d.product);
    const int  offset    = offsetOpt.value_or(0);
    if (offsetOpt) {
        std::printf("drive offset: %+d samples (from drive DB)\n", offset);
    } else {
        std::printf("drive offset: unknown for %s / %s — ripping at 0\n",
                    d.vendor.c_str(), d.product.c_str());
    }

    // Pad sectors at each edge so the slicing window can shift up to
    // |offset| frames either direction. ceil(|offset|/588) covers the
    // shift itself; +1 leaves one extra sector of headroom (cheap; the
    // largest offsets in the AR DB are ~1700 samples, ~3 sectors).
    const uint32_t padSectors    = static_cast<uint32_t>(
        (std::abs(offset) + 587) / 588) + 1;
    const uint32_t paddedSectors = totalSectors + 2 * padSectors;
    const uint64_t paddedBytes   = uint64_t{paddedSectors} * 2352;
    const uint64_t mainStart     = uint64_t{padSectors} * 2352;

    std::printf("\nallocating %.1f MiB  (natural %u sectors + %u pad at each edge)\n",
                static_cast<double>(paddedBytes) / (1024.0 * 1024.0),
                totalSectors, padSectors);
    std::vector<uint8_t> disc(paddedBytes);  // zero-initialized = pre/post pad default

    std::printf("reading %u sectors  (LBA %d .. %d)\n",
                totalSectors, firstLba, firstLba + int(totalSectors));

    constexpr uint32_t kChunk = 27;
    uint32_t remaining  = totalSectors;
    int32_t  lba        = firstLba;
    uint64_t writeAt    = mainStart;
    uint32_t readSoFar  = 0;
    uint32_t nextReport = std::max<uint32_t>(totalSectors / 10, 1);

    std::vector<ZeroFilledRange> zeroFilled;
    std::string lastReadError;

    while (remaining > 0) {
        const uint32_t n = std::min(remaining, kChunk);
        std::span<uint8_t> view(disc.data() + writeAt, uint64_t{n} * 2352);
        bool wasZeroFilled = false;
        const bool ok = readWithRetry(*dev, lba, n, view,
                                      wasZeroFilled, lastReadError);
        if (!ok && !wasZeroFilled) {
            std::fprintf(stderr,
                "\nunrecoverable read at LBA %d (%u sectors): %s\n",
                lba, n, lastReadError.c_str());
            return EXIT_FAILURE;
        }
        if (wasZeroFilled) {
            zeroFilled.push_back({lba, n});
            std::printf("  WARN: zero-filled LBA %d..%d (%s)\n",
                        lba, lba + int(n), lastReadError.c_str());
            std::fflush(stdout);
        }
        writeAt   += uint64_t{n} * 2352;
        remaining -= n;
        lba       += static_cast<int32_t>(n);
        readSoFar += n;
        if (readSoFar >= nextReport) {
            std::printf("  %3u%%  LBA %d\n",
                        readSoFar * 100u / totalSectors, lba);
            std::fflush(stdout);
            nextReport += std::max<uint32_t>(totalSectors / 10, 1);
        }
    }

    // Lead-out probe: try to read `padSectors` past the TOC's leadOutLba
    // into the post-pad region. Most drives let you read a few sectors of
    // lead-out runout (silence on commercial audio CDs); some return
    // EIO / OutOfRange. Failure is benign — the pre-pad / post-pad
    // regions fall inside AR's 5-sector skip windows at typical offsets.
    // Same retry+zero-fill path as the main loop, so the rip log records
    // the lead-out as a known bad range too.
    if (padSectors > 0) {
        std::span<uint8_t> tail(
            disc.data() + mainStart + uint64_t{totalSectors} * 2352,
            uint64_t{padSectors} * 2352);
        bool wasZeroFilled = false;
        std::string err;
        readWithRetry(*dev, static_cast<int32_t>(toc->leadOutLba),
                      padSectors, tail, wasZeroFilled, err);
        if (wasZeroFilled) {
            std::printf("(lead-out probe: %u pad sectors zero-filled: %s)\n",
                        padSectors, err.c_str());
        }
        // Lead-in (negative LBA) reads aren't supported yet — pre-pad
        // stays zero-filled. Within AR's first-5-sector skip region for
        // typical drive offsets, so the verifier doesn't see it.
    }

    // Slice each track at canonical-aligned byte boundaries. Canonical
    // sample 0 of the disc lives at byte `mainStart + offset*4` in the
    // padded buffer (negative offset shifts back into pre-pad zone,
    // positive offset shifts forward into main / post-pad).
    std::printf("\nencoding %zu tracks -> %s  (offset %+d applied)\n",
                toc->tracks.size(), outDir, offset);
    for (size_t i = 0; i < toc->tracks.size(); ++i) {
        const auto&    t        = toc->tracks[i];
        const uint32_t nextLba  = (i + 1 < toc->tracks.size())
                                    ? toc->tracks[i + 1].startLba
                                    : toc->leadOutLba;
        const uint32_t startSec = t.startLba - toc->tracks.front().startLba;
        const uint64_t frames   = uint64_t{nextLba - t.startLba} * 588;
        const int64_t  byteIdx  = static_cast<int64_t>(mainStart)
                                  + int64_t{startSec} * 2352
                                  + int64_t{offset} * 4;
        // CDDA raw bytes are interleaved 16-bit LE stereo — already the
        // exact layout flacencode wants on this little-endian host.
        const int16_t* pcm = reinterpret_cast<const int16_t*>(
            disc.data() + byteIdx);

        char filename[64];
        std::snprintf(filename, sizeof(filename),
                      "track_%02u.flac", t.trackNumber);
        const std::string outPath = std::string(outDir) + "/" + filename;

        std::vector<flacencode::VorbisTag> tags;
        if (chosen) {
            tags = tagsForTrack(*chosen, static_cast<int>(i),
                                ids.musicBrainzDiscId);
        }
        if (!flacencode::encodeCdAudioToFile(
                outPath, pcm, frames, flacencode::EncoderConfig{}, tags)) {
            std::fprintf(stderr, "encode failed for track %u\n", t.trackNumber);
            return EXIT_FAILURE;
        }
        std::printf("  track %2u  %5u sectors  %9llu frames  ->  %s\n",
                    t.trackNumber, nextLba - t.startLba,
                    static_cast<unsigned long long>(frames), filename);
    }

    if (!zeroFilled.empty()) {
        std::printf("\nWARNING: %zu read range(s) zero-filled inside the audio body:\n",
                    zeroFilled.size());
        uint32_t totalZero = 0;
        for (const auto& z : zeroFilled) {
            std::printf("  LBA %d .. %d  (%u sectors)\n",
                        z.lba, z.lba + int(z.sectors), z.sectors);
            totalZero += z.sectors;
        }
        std::printf("  %u sectors total (%.4f%% of disc) — AR/CTDB will surface any track that lands on these\n",
                    totalZero,
                    100.0 * static_cast<double>(totalZero)
                          / static_cast<double>(totalSectors));
    }

    std::printf("\nrip complete. verify with:\n  arverify_cli %s\n", outDir);
    return EXIT_SUCCESS;
}

static int runDbInfo() {
    const int n = plyr::cd::driveOffsetTableSize();
    std::printf("AccurateRip drive-offset table: %d entries bundled.\n", n);
    if (n == 0) {
        std::fprintf(stderr,
            "  (resource didn't load — Qt qrc :/drive_offsets.json missing?)\n");
        return EXIT_FAILURE;
    }
    const auto sample = plyr::cd::sampleDriveNames(8);
    std::printf("sample entries:\n");
    for (const auto& s : sample) std::printf("  %s\n", s.c_str());
    std::printf("\n");
    std::printf("local overrides (always tried first):\n");
    std::printf("  APPLE SUPERDRIVE -> %+d\n",
                plyr::cd::lookupDriveOffset("APPLE", "SUPERDRIVE").value_or(0));
    return EXIT_SUCCESS;
}

int main(int argc, char** argv) {
    // QCoreApplication is needed for the MusicBrainz lookup's nested
    // event loop (--rip). Cheap for the other modes; CFRunLoopRun in
    // --shield is compatible — Qt and CoreFoundation share the same
    // main run loop on macOS.
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("cdrip_cli");
    QCoreApplication::setOrganizationName("Concerto");

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--shield")  == 0) return runShield();
        if (std::strcmp(argv[i], "--toc")     == 0) return runReadToc();
        if (std::strcmp(argv[i], "--db-info") == 0) return runDbInfo();
        if (std::strcmp(argv[i], "--read")   == 0) {
            uint32_t cap = 0;
            if (i + 1 < argc) {
                char* end = nullptr;
                const unsigned long v = std::strtoul(argv[i + 1], &end, 10);
                if (end && *end == '\0') cap = static_cast<uint32_t>(v);
            }
            return runReadAll(cap);
        }
        if (std::strcmp(argv[i], "--rip") == 0 && i + 1 < argc) {
            return runRip(argv[i + 1]);
        }
    }
    return runEnumerate();
}
