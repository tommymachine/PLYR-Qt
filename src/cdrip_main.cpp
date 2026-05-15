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

#include "ArVerify.h"
#include "CdDevice.h"
#include "CdShield.h"
#include "FlacEncode.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

// Full disc rip: read every sector into memory, slice on TOC boundaries,
// FLAC-encode each track with no tags and no drive-offset correction.
// The point is to prove the end-to-end pipeline; AR's offset scan does
// the bit-accuracy comparison against canonical pressings, so this rip
// only needs to be self-consistent — drive-offset alignment lands next.
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
    const uint64_t totalBytes   = uint64_t{totalSectors} * 2352;

    std::printf("\nallocating %.1f MiB for full-disc PCM buffer\n",
                static_cast<double>(totalBytes) / (1024.0 * 1024.0));
    std::vector<uint8_t> disc(totalBytes);

    std::printf("reading %u sectors  (LBA %d .. %d)\n",
                totalSectors, firstLba, firstLba + int(totalSectors));

    constexpr uint32_t kChunk = 27;
    uint32_t remaining  = totalSectors;
    int32_t  lba        = firstLba;
    uint64_t bytesRead  = 0;
    uint32_t readSoFar  = 0;
    uint32_t nextReport = std::max<uint32_t>(totalSectors / 10, 1);

    while (remaining > 0) {
        const uint32_t n = std::min(remaining, kChunk);
        std::span<uint8_t> view(disc.data() + bytesRead, uint64_t{n} * 2352);
        const auto r = dev->readSectors(lba, n, view, /*wantC2=*/false);
        if (r.status != plyr::cd::ReadStatus::Ok) {
            std::fprintf(stderr, "\nread failed at LBA %d: %s\n",
                         lba, dev->lastDeviceError().c_str());
            return EXIT_FAILURE;
        }
        if (r.sectorsRead != n) {
            std::fprintf(stderr, "\nshort read at LBA %d: asked %u got %u\n",
                         lba, n, r.sectorsRead);
            return EXIT_FAILURE;
        }
        bytesRead += uint64_t{r.sectorsRead} * 2352;
        remaining -= r.sectorsRead;
        lba       += static_cast<int32_t>(r.sectorsRead);
        readSoFar += r.sectorsRead;
        if (readSoFar >= nextReport) {
            std::printf("  %3u%%  LBA %d\n",
                        readSoFar * 100u / totalSectors, lba);
            std::fflush(stdout);
            nextReport += std::max<uint32_t>(totalSectors / 10, 1);
        }
    }

    std::printf("\nencoding %zu tracks -> %s\n", toc->tracks.size(), outDir);
    for (size_t i = 0; i < toc->tracks.size(); ++i) {
        const auto&    t        = toc->tracks[i];
        const uint32_t nextLba  = (i + 1 < toc->tracks.size())
                                    ? toc->tracks[i + 1].startLba
                                    : toc->leadOutLba;
        const uint32_t startSec = t.startLba - toc->tracks.front().startLba;
        const uint64_t frames   = uint64_t{nextLba - t.startLba} * 588;
        // CDDA raw bytes are interleaved 16-bit LE stereo — already the
        // exact layout flacencode wants on this little-endian host.
        const int16_t* pcm = reinterpret_cast<const int16_t*>(
            disc.data() + uint64_t{startSec} * 2352);

        char filename[64];
        std::snprintf(filename, sizeof(filename),
                      "track_%02u.flac", t.trackNumber);
        const std::string outPath = std::string(outDir) + "/" + filename;
        if (!flacencode::encodeCdAudioToFile(outPath, pcm, frames)) {
            std::fprintf(stderr, "encode failed for track %u\n", t.trackNumber);
            return EXIT_FAILURE;
        }
        std::printf("  track %2u  %5u sectors  %9llu frames  ->  %s\n",
                    t.trackNumber, nextLba - t.startLba,
                    static_cast<unsigned long long>(frames), filename);
    }

    std::printf("\nrip complete. verify with:\n  arverify_cli %s\n", outDir);
    return EXIT_SUCCESS;
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--shield") == 0) return runShield();
        if (std::strcmp(argv[i], "--toc")    == 0) return runReadToc();
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
