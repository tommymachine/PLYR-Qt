// RipWorker — port of cdrip_main.cpp::runRip, shaped for the GUI thread.
// See RipWorker.h for the surface; this file is the implementation.

#include "RipWorker.h"

#include "ArVerify.h"
#include "CdDevice.h"
#include "DriveOffsetDb.h"
#include "FlacEncode.h"
#include "MusicBrainz.h"

#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>
#include <QUuid>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace plyr::cd {

namespace {

constexpr const char* kAccurateRipBase = "http://www.accuraterip.com/accuraterip/";
constexpr const char* kCtdbBase        = "http://db.cuetools.net/";
constexpr const char* kUserAgent       = "Concerto-Ripper/0.1";

// Same per-call chunk size cdparanoia uses and runRip used. ~62 KiB —
// big enough that ioctl round-trip is amortized, small enough that any
// SCSI / USB transfer cap is honored.
constexpr uint32_t kReadChunkSectors = 27;

// Synchronous HTTP GET on the worker thread. Spins a nested event loop
// on `nam` so the worker stays responsive to its own queued events
// (notably doCancel from the GUI thread).
QByteArray httpGet(QNetworkAccessManager& nam, const QUrl& url, int& status) {
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QString::fromLatin1(kUserAgent));
    QNetworkReply* reply = nam.get(req);

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished,
                     &loop, &QEventLoop::quit);
    loop.exec();

    status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QByteArray body;
    if (reply->error() == QNetworkReply::NoError && status == 200) {
        body = reply->readAll();
    }
    reply->deleteLater();
    return body;
}

// Bridge plyr::cd::Toc into the arverify-side TOC (absolute, 150-based).
arverify::DiscToc toArverifyToc(const Toc& src) {
    arverify::DiscToc out;
    out.trackOffsets.reserve(src.tracks.size());
    for (const auto& t : src.tracks) {
        out.trackOffsets.push_back(t.startLba + arverify::kLeadInFrames);
    }
    out.leadoutOffset = src.leadOutLba + arverify::kLeadInFrames;
    return out;
}

enum class DiscKind { PureAudio, MixedMode, PureData, Empty };

DiscKind classifyDisc(const Toc& toc) {
    if (toc.tracks.empty()) return DiscKind::Empty;
    bool hasAudio = false, hasData = false;
    for (const auto& t : toc.tracks) {
        if (t.isData) hasData = true; else hasAudio = true;
    }
    if (hasAudio && hasData) return DiscKind::MixedMode;
    if (hasData)             return DiscKind::PureData;
    return DiscKind::PureAudio;
}

// Build the initial per-track QVariantList the Ripper shows. Titles are
// filled in later, once MB resolves.
QVariantList buildInitialTracks(const Toc& toc) {
    QVariantList out;
    const uint32_t firstLba = toc.tracks.front().startLba;
    const uint32_t totalSectors = toc.leadOutLba - firstLba;
    for (size_t i = 0; i < toc.tracks.size(); ++i) {
        const auto&    t        = toc.tracks[i];
        const uint32_t nextLba  = (i + 1 < toc.tracks.size())
                                  ? toc.tracks[i + 1].startLba
                                  : toc.leadOutLba;
        const uint32_t sec      = nextLba - t.startLba;
        QVariantMap row;
        row["number"]        = int(t.trackNumber);
        row["durationSec"]   = int(sec / 75);
        row["startFraction"] = double(t.startLba - firstLba) / double(totalSectors);
        row["endFraction"]   = double(nextLba    - firstLba) / double(totalSectors);
        row["status"]        = QStringLiteral("pending");
        row["title"]         = QString();
        out.append(row);
    }
    return out;
}

// Replace track titles in a QVariantList with those from the MB match.
QVariantList tracksWithMbTitles(const QVariantList& src,
                                const musicbrainz::Release& rel) {
    QVariantList out = src;
    const auto& mbTracks = rel.disc.tracks;
    for (int i = 0; i < out.size(); ++i) {
        QVariantMap m = out[i].toMap();
        if (i < int(mbTracks.size()) && !mbTracks[i].title.empty()) {
            m["title"] = QString::fromStdString(mbTracks[i].title);
        }
        out[i] = m;
    }
    return out;
}

// Duplicated from cdrip_main.cpp::tagsForTrack. Will move to a shared
// header once the rip code stabilizes.
std::vector<flacencode::VorbisTag> tagsForTrack(
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
    if (!rel.releaseGroupId.empty())
        add("MUSICBRAINZ_RELEASEGROUPID", rel.releaseGroupId);
    return tags;
}

// CUE doesn't define an escape sequence for embedded double-quote. The
// widely-tolerated substitution is a single apostrophe.
std::string cueQuote(const std::string& s) {
    std::string out = s;
    std::replace(out.begin(), out.end(), '"', '\'');
    return out;
}

struct ZeroFilledRange {
    int32_t  lba;
    uint32_t sectors;
};

void writeCueSheet(const std::string& outDir,
                   const Toc& toc,
                   const musicbrainz::Release* mb)
{
    const std::string path = outDir + "/cd_rip.cue";
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return;
    if (mb) {
        if (!mb->artist.empty())
            std::fprintf(f, "PERFORMER \"%s\"\n", cueQuote(mb->artist).c_str());
        if (!mb->title.empty())
            std::fprintf(f, "TITLE \"%s\"\n", cueQuote(mb->title).c_str());
    }
    for (size_t i = 0; i < toc.tracks.size(); ++i) {
        const auto& t = toc.tracks[i];
        char filename[64];
        std::snprintf(filename, sizeof(filename),
                      "track_%02u.flac", t.trackNumber);
        std::fprintf(f, "FILE \"%s\" WAVE\n", filename);
        std::fprintf(f, "  TRACK %02u AUDIO\n", t.trackNumber);
        if (mb && i < mb->disc.tracks.size()) {
            if (!mb->disc.tracks[i].title.empty()) {
                std::fprintf(f, "    TITLE \"%s\"\n",
                             cueQuote(mb->disc.tracks[i].title).c_str());
            }
            if (!mb->artist.empty()) {
                std::fprintf(f, "    PERFORMER \"%s\"\n",
                             cueQuote(mb->artist).c_str());
            }
        }
        std::fprintf(f, "    INDEX 01 00:00:00\n");
    }
    std::fclose(f);
}

void writeRipLog(const std::string& outDir,
                 const DriveInfo& drive,
                 const Toc& toc,
                 const arverify::DiscIds& ids,
                 int offset, bool offsetFromDb,
                 const musicbrainz::Release* mb,
                 const std::vector<ZeroFilledRange>& zeroFilled,
                 double elapsedSec,
                 const std::string& verifySummary)
{
    const std::string path = outDir + "/cd_rip.log";
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return;

    std::fprintf(f, "Concerto rip log\n\n");

    std::fprintf(f, "Drive:        %s / %s  rev %s  (/dev/%s)\n",
                 drive.vendor.empty()   ? "?" : drive.vendor.c_str(),
                 drive.product.empty()  ? "?" : drive.product.c_str(),
                 drive.revision.empty() ? "?" : drive.revision.c_str(),
                 drive.id.c_str());
    std::fprintf(f, "Drive offset: %+d samples  (%s)\n\n",
                 offset, offsetFromDb
                     ? "from bundled AccurateRip DB"
                     : "fallback 0 — drive not in DB, verifier scanned");

    std::fprintf(f, "Disc IDs:\n");
    std::fprintf(f, "  AccurateRip   id1=%08x  id2=%08x\n",
                 ids.accurateRipId1, ids.accurateRipId2);
    std::fprintf(f, "  CDDB          %08x\n", ids.cddbId);
    std::fprintf(f, "  MusicBrainz   %s\n\n", ids.musicBrainzDiscId.c_str());

    if (mb) {
        std::fprintf(f, "MusicBrainz match:\n");
        std::fprintf(f, "  Artist:  %s\n", mb->artist.c_str());
        std::fprintf(f, "  Album:   %s\n", mb->title.c_str());
        if (!mb->date.empty())
            std::fprintf(f, "  Date:    %s\n", mb->date.c_str());
        if (!mb->country.empty())
            std::fprintf(f, "  Country: %s\n", mb->country.c_str());
        std::fprintf(f, "  Disc:    %d of %d\n",
                     mb->disc.position, mb->disc.totalCount);
        std::fprintf(f, "  Release: %s\n",  mb->id.c_str());
        if (!mb->releaseGroupId.empty())
            std::fprintf(f, "  Release-group: %s\n", mb->releaseGroupId.c_str());
        std::fprintf(f, "\n");
    } else {
        std::fprintf(f, "MusicBrainz: no match\n\n");
    }

    std::fprintf(f, "Table of contents:\n");
    for (const auto& t : toc.tracks) {
        const uint32_t abs = t.startLba + 150;
        std::fprintf(f, "  track %2u  LBA %7u  MSF %02u:%02u.%02u  %s%s\n",
                     t.trackNumber, t.startLba,
                     abs / (75 * 60), (abs / 75) % 60, abs % 75,
                     t.isData ? "DATA" : "AUDIO",
                     t.preEmphasis ? "  +preemphasis" : "");
    }
    {
        const uint32_t abs = toc.leadOutLba + 150;
        std::fprintf(f, "  lead-out  LBA %7u  MSF %02u:%02u.%02u\n\n",
                     toc.leadOutLba,
                     abs / (75 * 60), (abs / 75) % 60, abs % 75);
    }

    if (zeroFilled.empty()) {
        std::fprintf(f, "Read errors: none.\n");
    } else {
        std::fprintf(f, "Read errors (%zu range(s) zero-filled):\n",
                     zeroFilled.size());
        uint32_t total = 0;
        for (const auto& z : zeroFilled) {
            std::fprintf(f, "  LBA %d..%d  (%u sectors)\n",
                         z.lba, z.lba + int(z.sectors), z.sectors);
            total += z.sectors;
        }
    }

    if (!verifySummary.empty()) {
        std::fprintf(f, "\nVerify: %s\n", verifySummary.c_str());
    }
    std::fprintf(f, "\nElapsed: %.1f s\n", elapsedSec);
    std::fclose(f);
}

// Read `count` sectors at `lba` with up to two re-tries on transient
// errors. Non-retriable failures break out immediately; after the budget
// is exhausted the buffer is zero-filled and `outZeroFilled` is set so
// the caller can log + continue. Same strategy as cdrip_main::readWithRetry.
bool readWithRetry(CdDevice& dev, int32_t lba, uint32_t count,
                   std::span<uint8_t> buf,
                   bool& outZeroFilled, std::string& outLastError) {
    constexpr int kMaxRetries = 2;
    ReadResult r;
    for (int attempt = 0; attempt <= kMaxRetries; ++attempt) {
        r = dev.readSectors(lba, count, buf, /*wantC2=*/false);
        if (r.status == ReadStatus::Ok && r.sectorsRead == count) {
            outZeroFilled = false;
            return true;
        }
        if (r.status == ReadStatus::OutOfRange
            || r.status == ReadStatus::FatalDeviceError
            || r.status == ReadStatus::Aborted) {
            break;
        }
    }
    outLastError  = dev.lastDeviceError();
    outZeroFilled = true;
    std::memset(buf.data(), 0, buf.size());
    return false;
}

// Slug an album title for use as a folder name. Mirrors what the stub
// produced: collapse runs of non-alphanumerics to `_`, trim trailing.
QString slugForFolder(const QString& title) {
    QString s = title;
    s.replace(QRegularExpression{QStringLiteral("[^A-Za-z0-9]+")},
              QStringLiteral("_"));
    while (s.endsWith(QChar('_'))) s.chop(1);
    if (s.isEmpty()) s = QStringLiteral("Untitled");
    return s;
}

// Move a directory across the filesystem. Plain rename when source and
// destination share a volume; copy+delete otherwise.
bool moveDirectory(const std::filesystem::path& src,
                   const std::filesystem::path& dst,
                   std::string& err)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    if (ec) { err = "create_directories: " + ec.message(); return false; }

    fs::rename(src, dst, ec);
    if (!ec) return true;

    // Cross-volume? errno EXDEV / std::errc::cross_device_link.
    if (ec == std::errc::cross_device_link
        || ec == std::errc::operation_not_supported)
    {
        ec.clear();
        fs::copy(src, dst,
                 fs::copy_options::recursive | fs::copy_options::copy_symlinks,
                 ec);
        if (ec) { err = "copy: " + ec.message(); return false; }
        fs::remove_all(src, ec);
        // Failure to remove the original is non-fatal here — the data is
        // safely at dst. Surface as a warning.
        if (ec) { err = "copy ok, remove_all: " + ec.message(); }
        return true;
    }
    err = "rename: " + ec.message();
    return false;
}

} // namespace

RipWorker::RipWorker(QObject* parent) : QObject(parent) {}
RipWorker::~RipWorker() = default;

void RipWorker::doCancel() {
    m_cancel.store(true, std::memory_order_release);
}

void RipWorker::discardStagedRip() {
    if (m_currentTempDir.isEmpty()) return;
    std::error_code ec;
    std::filesystem::remove_all(m_currentTempDir.toStdString(), ec);
    m_currentTempDir.clear();
}

void RipWorker::doRip(const QString& bsdName,
                      const QString& preferredParentFolder) {
    // Reset the per-rip cancel flag — the GUI may have set it during the
    // previous disc; the new rip starts fresh.
    m_cancel.store(false, std::memory_order_release);
    m_currentTempDir.clear();

    const auto t0 = std::chrono::steady_clock::now();

    // --- Open the drive ---------------------------------------------
    const auto drives = CdDevice::enumerate();
    DriveInfo selected;
    for (const auto& d : drives) {
        if (!bsdName.isEmpty()
            && QString::fromStdString(d.id) == bsdName) {
            selected = d; break;
        }
    }
    if (selected.id.empty() && !drives.empty()) {
        selected = drives.front();
    }
    if (selected.id.empty()) {
        emit failed(QStringLiteral("No optical drive with media found."));
        return;
    }

    auto dev = CdDevice::open(selected.id);
    if (!dev) {
        emit failed(QStringLiteral("Couldn't open /dev/%1.")
                        .arg(QString::fromStdString(selected.id)));
        return;
    }

    // --- TOC + classification ---------------------------------------
    const auto tocOpt = dev->readToc();
    if (!tocOpt) {
        emit failed(QStringLiteral("Couldn't read the disc TOC: %1")
                        .arg(QString::fromStdString(dev->lastDeviceError())));
        return;
    }
    const Toc& toc = *tocOpt;
    switch (classifyDisc(toc)) {
        case DiscKind::PureAudio: break;
        case DiscKind::MixedMode:
            emit failed(QStringLiteral(
                "This is a mixed-mode CD (data + audio). Concerto rips "
                "audio-only CDs in v1."));
            return;
        case DiscKind::PureData:
            emit failed(QStringLiteral(
                "This is a data CD (no CDDA tracks). Open it in Finder."));
            return;
        case DiscKind::Empty:
            emit failed(QStringLiteral("The disc TOC came back empty."));
            return;
    }

    if (m_cancel.load(std::memory_order_acquire)) {
        emit ripCancelled();
        return;
    }

    // --- Disc IDs + drive offset ------------------------------------
    const arverify::DiscToc arToc = toArverifyToc(toc);
    const arverify::DiscIds ids   = arverify::computeDiscIds(arToc);

    const auto offsetOpt = lookupDriveOffset(selected.vendor, selected.product);
    const int  driveOffset = offsetOpt.value_or(0);

    const int32_t  firstLba     = static_cast<int32_t>(toc.tracks.front().startLba);
    const uint32_t totalSectors = toc.leadOutLba - toc.tracks.front().startLba;

    QVariantMap discInfo;
    discInfo["driveName"] = QStringLiteral("%1 %2 rev %3").arg(
        QString::fromStdString(selected.vendor),
        QString::fromStdString(selected.product),
        QString::fromStdString(selected.revision));
    discInfo["vendor"]              = QString::fromStdString(selected.vendor);
    discInfo["product"]             = QString::fromStdString(selected.product);
    discInfo["revision"]            = QString::fromStdString(selected.revision);
    discInfo["driveOffsetSamples"]  = driveOffset;
    discInfo["driveOffsetFromDb"]   = offsetOpt.has_value();
    discInfo["trackCount"]          = int(toc.tracks.size());
    discInfo["totalDurationSec"]    = int(totalSectors / 75);
    discInfo["tracks"]              = buildInitialTracks(toc);
    discInfo["mbDiscId"]            = QString::fromStdString(ids.musicBrainzDiscId);
    discInfo["accurateRipId"]       = QStringLiteral("%1-%2-%3")
                                          .arg(ids.accurateRipId1, 8, 16, QChar('0'))
                                          .arg(ids.accurateRipId2, 8, 16, QChar('0'))
                                          .arg(ids.cddbId,         8, 16, QChar('0'));
    emit discIdentified(discInfo);

    // --- MusicBrainz lookup -----------------------------------------
    // QNAM lives on the worker thread (this owns it; the slot ran on
    // the worker via queued connection).
    QNetworkAccessManager nam;
    const auto releases = musicbrainz::lookupByDiscId(
        nam, ids.musicBrainzDiscId, static_cast<int>(toc.tracks.size()),
        kUserAgent);
    const musicbrainz::Release* chosen = nullptr;
    if (!releases.empty()) {
        chosen = &releases.front();
        QVariantMap m;
        m["hasMatch"]        = true;
        m["albumTitle"]      = QString::fromStdString(chosen->title);
        m["artist"]          = QString::fromStdString(chosen->artist);
        m["date"]            = QString::fromStdString(chosen->date);
        m["country"]         = QString::fromStdString(chosen->country);
        m["discPosition"]    = chosen->disc.position;
        m["discTotalCount"]  = chosen->disc.totalCount;
        m["releaseId"]       = QString::fromStdString(chosen->id);
        m["releaseGroupId"]  = QString::fromStdString(chosen->releaseGroupId);
        m["tracks"]          = tracksWithMbTitles(
            discInfo["tracks"].toList(), *chosen);
        emit mbResolved(m);
    } else {
        emit mbUnavailable();
    }

    if (m_cancel.load(std::memory_order_acquire)) {
        emit ripCancelled();
        return;
    }

    // --- Allocate padded disc buffer + temp dir ---------------------
    const uint32_t padSectors    = static_cast<uint32_t>(
        (std::abs(driveOffset) + 587) / 588) + 1;
    const uint32_t paddedSectors = totalSectors + 2 * padSectors;
    const uint64_t paddedBytes   = uint64_t{paddedSectors} * 2352;
    const uint64_t mainStart     = uint64_t{padSectors} * 2352;
    // Local buffer — lives just for this rip. (Earlier versions held
    // this as a member so a post-save "splice" could replay offset-
    // corrected PCM into the audio pipe; that path got cut after the
    // QAudioDecoder::seek clamping issue made it unreliable.)
    std::vector<uint8_t> disc(paddedBytes, 0);

    // Temp dir is created BEFORE the read loop — inline encoding writes
    // FLACs here as soon as each track's sectors land, so the GUI can
    // pick them up via discTrackReady → playlist.appendTrack while the
    // rip is still going.
    //
    // Two cases:
    //   * Batch with known parent → write straight into
    //     <preferredParentFolder>/<suggestedName>/. The folder appears
    //     in the user's library from the start; doSave becomes a no-op
    //     rename to itself.
    //   * Standalone / first disc of a fresh batch (parent unknown) →
    //     ~/Library/Application Support/Concerto/rip_in_progress/<uuid>/,
    //     and doSave moves the directory into the user's chosen home
    //     when the save picker resolves.
    //
    // Pre-compute the suggested folder name here too (we already
    // resolved MB above) so we can use it for the in-place case.
    QString suggestedName;
    if (chosen) {
        const QString slug = slugForFolder(
            QString::fromStdString(chosen->title));
        if (chosen->disc.totalCount > 1) {
            suggestedName = QStringLiteral("%1__Disc_%2")
                                .arg(slug)
                                .arg(chosen->disc.position, 2, 10, QChar('0'));
        } else {
            suggestedName = slug;
        }
    } else {
        suggestedName = QStringLiteral("Untitled_CD_")
            + QDateTime::currentDateTime().toString("yyyyMMdd_HHmm");
    }

    QString tempDir;
    if (!preferredParentFolder.isEmpty() && !suggestedName.isEmpty()) {
        // Disambiguate if a folder with that name already exists in
        // the parent — append " (N)" suffix until we find a free spot.
        QString candidate = preferredParentFolder
            + QStringLiteral("/") + suggestedName;
        if (QDir(candidate).exists()) {
            for (int n = 2; n < 1000; ++n) {
                const QString alt = preferredParentFolder
                    + QStringLiteral("/")
                    + suggestedName
                    + QStringLiteral(" (%1)").arg(n);
                if (!QDir(alt).exists()) {
                    candidate = alt;
                    suggestedName = QString("%1 (%2)").arg(suggestedName).arg(n);
                    break;
                }
            }
        }
        tempDir = candidate;
    } else {
        const QString rootData = QStandardPaths::writableLocation(
            QStandardPaths::GenericDataLocation);
        const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces)
                                 .remove(QChar('-'));
        tempDir = rootData
            + QStringLiteral("/Concerto/rip_in_progress/") + uuid;
    }
    if (!QDir().mkpath(tempDir)) {
        emit failed(QStringLiteral("Couldn't create temp directory:\n%1").arg(tempDir));
        return;
    }
    m_currentTempDir = tempDir;

    emit readStarted();
    // The state machine stays in Reading for the whole read+inline-
    // encode window — each encoded track lights up live via
    // encodedTrack signals, no need to transition to Encoding for that.
    // Encoding state fires briefly after the read loop only if the
    // last track is still waiting on the lead-out probe (positive
    // drive offset). For APPLE SuperDrive (-6) it'll be a no-op flash.

    // Streaming-preview path: kick the audio engine into pull-from-pipe
    // mode so each freshly-read chunk goes straight to a QAudioSink.
    // The user hears audio ~2 s after rip start (drive spin-up + first
    // read), well before FLAC encoding of track 1 finishes.
    //
    // Pass the TOC-derived disc duration through so the audio engine
    // populates the synthetic segment's durationMs — that's what
    // drives the seek slider's range and the duration label.
    const qint64 discDurationMs =
        (qint64(totalSectors) * 1000) / 75;
    emit previewStreamStart(discDurationMs);

    uint32_t remaining   = totalSectors;
    int32_t  lba         = firstLba;
    uint64_t writeAt     = mainStart;
    uint32_t readSoFar   = 0;
    auto     milestoneT0 = std::chrono::steady_clock::now();
    uint32_t milestoneSectors = 0;
    int      currentTrackNumber = toc.tracks.front().trackNumber;
    size_t   currentTrackIndex  = 0;
    size_t   encodedThrough     = 0;  // count of tracks already FLAC'd

    std::vector<ZeroFilledRange> zeroFilledRanges;
    std::string lastReadError;

    // Heartbeat cadence: report every ~120 chunks (~25 MB read) or
    // ~500ms, whichever fires first. cdparanoia-style raw progress is
    // overwhelming; QML only needs a few updates per percent.
    constexpr uint32_t kChunkPerReport = 120;
    auto lastEmit = std::chrono::steady_clock::now();
    uint32_t sinceEmit = 0;

    // Encode every track whose offset-corrected byte range is now
    // fully resident in `disc`. `writtenBytes` is the highest byte
    // index that's been read into the buffer (exclusive).
    auto tryEncodeReadyTracks = [&](uint64_t writtenBytes) -> bool {
        while (encodedThrough < toc.tracks.size()) {
            const auto&    t        = toc.tracks[encodedThrough];
            const uint32_t nextLba  = (encodedThrough + 1 < toc.tracks.size())
                                        ? toc.tracks[encodedThrough + 1].startLba
                                        : toc.leadOutLba;
            const uint32_t startSec = t.startLba - toc.tracks.front().startLba;
            const uint64_t frames   = uint64_t{nextLba - t.startLba} * 588;
            const int64_t  byteIdx  = static_cast<int64_t>(mainStart)
                                      + int64_t{startSec} * 2352
                                      + int64_t{driveOffset} * 4;
            const uint64_t endByte  = uint64_t(byteIdx) + frames * 4;
            if (endByte > writtenBytes) return true;  // not yet — wait

            const int16_t* pcm = reinterpret_cast<const int16_t*>(
                disc.data() + byteIdx);

            char filename[64];
            std::snprintf(filename, sizeof(filename),
                          "track_%02u.flac", t.trackNumber);
            const std::string outPath =
                tempDir.toStdString() + "/" + filename;

            std::vector<flacencode::VorbisTag> tags;
            if (chosen) {
                tags = tagsForTrack(*chosen, static_cast<int>(encodedThrough),
                                    ids.musicBrainzDiscId);
            }
            if (!flacencode::encodeCdAudioToFile(
                    outPath, pcm, frames, flacencode::EncoderConfig{}, tags)) {
                emit previewStreamStop();
                discardStagedRip();
                emit failed(QStringLiteral("Encode failed for track %1")
                                .arg(t.trackNumber));
                return false;
            }
            emit encodedTrack(int(t.trackNumber),
                              QString::fromStdString(outPath));
            ++encodedThrough;
        }
        return true;
    };

    while (remaining > 0) {
        if (m_cancel.load(std::memory_order_acquire)) {
            emit previewStreamStop();
            discardStagedRip();
            emit ripCancelled();
            return;
        }
        const uint32_t n = std::min(remaining, kReadChunkSectors);
        std::span<uint8_t> view(disc.data() + writeAt, uint64_t{n} * 2352);
        bool wasZeroFilled = false;
        const bool ok = readWithRetry(*dev, lba, n, view,
                                      wasZeroFilled, lastReadError);
        if (!ok && !wasZeroFilled) {
            emit previewStreamStop();
            discardStagedRip();
            emit failed(QStringLiteral(
                "Unrecoverable read at LBA %1 (%2 sectors): %3")
                .arg(lba).arg(n)
                .arg(QString::fromStdString(lastReadError)));
            return;
        }
        if (wasZeroFilled) {
            zeroFilledRanges.push_back({lba, n});
            const double fraction =
                double(readSoFar) / double(totalSectors);
            QVariantMap zf;
            zf["fraction"] = fraction;
            zf["sectors"]  = int(n);
            zf["lba"]      = lba;
            emit zeroFilled(zf);
            emit warning(QStringLiteral(
                "Zero-filled LBA %1..%2 (%3 sectors) — verifier will flag any affected track")
                .arg(lba).arg(lba + int(n)).arg(n));
        }

        writeAt          += uint64_t{n} * 2352;
        remaining        -= n;
        lba              += static_cast<int32_t>(n);
        readSoFar        += n;
        milestoneSectors += n;
        sinceEmit        += n;

        // Streaming preview: push the just-read CDDA bytes to the
        // audio engine. We use the raw read region rather than the
        // offset-corrected slice — at typical drive offsets (±6 to
        // ±100 frames) the human ear can't hear the difference, and
        // sample-perfect alignment would mean staging chunks until
        // the offset-corrected window is fully resident.
        emit previewPcm(QByteArray(
            reinterpret_cast<const char*>(view.data()),
            qsizetype(view.size())));

        // Advance "current track" pointer as the read crosses a boundary.
        while (currentTrackIndex + 1 < toc.tracks.size()
               && static_cast<uint32_t>(lba)
                  >= toc.tracks[currentTrackIndex + 1].startLba)
        {
            ++currentTrackIndex;
            currentTrackNumber =
                toc.tracks[currentTrackIndex].trackNumber;
        }

        // Encode any track whose offset-corrected byte range is fully
        // resident now. For negative drive offset (APPLE SUPERDRIVE
        // here is −6) every track including the last meets the byte
        // check inside this loop; for positive offsets the last track
        // is finished off after the lead-out probe below.
        if (!tryEncodeReadyTracks(writeAt)) return;

        const auto now = std::chrono::steady_clock::now();
        const double sinceEmitDt = std::chrono::duration<double>(
            now - lastEmit).count();
        if (sinceEmit >= kChunkPerReport || sinceEmitDt >= 0.5) {
            const double dt = std::chrono::duration<double>(
                now - milestoneT0).count();
            const double rate = dt > 0
                ? double(milestoneSectors) / dt : 0.0;
            const double mult = rate / 75.0;
            const double frac = double(readSoFar) / double(totalSectors);
            const int eta = mult > 0.5
                ? int(double(remaining) / std::max(rate, 1.0))
                : 0;
            emit readProgress(lba, rate, mult, eta,
                              currentTrackNumber, frac);
            sinceEmit = 0;
            lastEmit  = now;
            if (sinceEmitDt > 0.5) {
                milestoneT0      = now;
                milestoneSectors = 0;
            }
        }
    }

    // Final progress emit at 100%, with cleared speed/multiplier.
    emit readProgress(lba, 0.0, 0.0, 0, 0, 1.0);

    // Lead-out probe — gives positive-offset drives the post-pad data
    // their last track needs. Most drives refuse and the post-pad
    // stays zero-initialized; AR's last-5-sector skip absorbs that.
    if (padSectors > 0) {
        std::span<uint8_t> tail(
            disc.data() + mainStart + uint64_t{totalSectors} * 2352,
            uint64_t{padSectors} * 2352);
        bool wasZeroFilled = false;
        std::string err;
        readWithRetry(*dev, static_cast<int32_t>(toc.leadOutLba),
                      padSectors, tail, wasZeroFilled, err);
        if (!wasZeroFilled) writeAt += uint64_t{padSectors} * 2352;
    }

    // Eject so the user can swap in the next disc while we finish.
    if (!dev->eject()) {
        emit warning(QStringLiteral("Eject didn't go through: %1")
                         .arg(QString::fromStdString(dev->lastDeviceError())));
    }
    dev.reset();

    // Finish encoding any track that was still waiting on the lead-out
    // probe (last track for positive drive offset). Pass the full buffer
    // size — the post-pad zone is zero-initialized so the byte range
    // is always satisfiable here.
    emit encodingStarted();
    if (!tryEncodeReadyTracks(paddedBytes)) return;

    if (m_cancel.load(std::memory_order_acquire)) {
        emit previewStreamStop();
        discardStagedRip();
        emit ripCancelled();
        return;
    }
    emit encodingComplete();

    if (m_cancel.load(std::memory_order_acquire)) {
        discardStagedRip();
        emit ripCancelled();
        return;
    }

    // --- Verify against AccurateRip + CTDB --------------------------
    emit verifyingStarted();

    // AR + CTDB HTTP lookups.
    std::vector<arverify::ArDiscEntry> arEntries;
    {
        const QUrl url(QString::fromLatin1(kAccurateRipBase)
            + QString::fromStdString(arverify::accurateRipPath(arToc, ids)));
        int status = 0;
        const QByteArray body = httpGet(nam, url, status);
        if (status == 200 && !body.isEmpty()) {
            arEntries = arverify::parseAccurateRipResponse(
                reinterpret_cast<const uint8_t*>(body.constData()),
                size_t(body.size()));
        }
    }
    std::vector<arverify::CtdbEntry> ctdbEntries;
    {
        const QUrl url(QString::fromLatin1(kCtdbBase)
            + QString::fromStdString(arverify::ctdbLookupPath(arToc)));
        int status = 0;
        const QByteArray body = httpGet(nam, url, status);
        if (status == 200 && !body.isEmpty()) {
            ctdbEntries = arverify::parseCtdbResponse(
                reinterpret_cast<const uint8_t*>(body.constData()),
                size_t(body.size()));
        }
    }

    // Canonical-aligned disc samples: the offset-corrected slice of the
    // padded buffer is already in AR's packed-stereo layout on this LE
    // host. No copy needed.
    const auto* canonical = reinterpret_cast<const arverify::ArSample*>(
        disc.data() + mainStart + ptrdiff_t{driveOffset} * 4);
    const uint64_t discFrames = uint64_t{totalSectors} * 588;

    std::vector<arverify::TrackSpan> spans(toc.tracks.size());
    for (size_t i = 0; i < toc.tracks.size(); ++i) {
        const uint32_t startLba = toc.tracks[i].startLba;
        const uint32_t nextLba  = (i + 1 < toc.tracks.size())
                                    ? toc.tracks[i + 1].startLba
                                    : toc.leadOutLba;
        spans[i].start = uint64_t(startLba - toc.tracks.front().startLba) * 588;
        spans[i].end   = uint64_t(nextLba  - toc.tracks.front().startLba) * 588;
    }

    // Mark tracks that overlap any zero-filled LBA range as "fail" up
    // front. AR/CTDB checks won't redeem them.
    std::vector<bool> tainted(toc.tracks.size(), false);
    for (const auto& z : zeroFilledRanges) {
        const uint32_t zStart = uint32_t(z.lba);
        const uint32_t zEnd   = uint32_t(z.lba) + z.sectors;
        for (size_t i = 0; i < toc.tracks.size(); ++i) {
            const uint32_t tStart = toc.tracks[i].startLba;
            const uint32_t tEnd   = (i + 1 < toc.tracks.size())
                                    ? toc.tracks[i + 1].startLba
                                    : toc.leadOutLba;
            if (zStart < tEnd && zEnd > tStart) {
                tainted[i] = true;
            }
        }
    }

    int okCount = 0, warnCount = 0, failCount = 0, unknownCount = 0;
    int bestArConf = 0, bestCtdbConf = 0;
    const int total = int(toc.tracks.size());
    const bool haveAr   = !arEntries.empty();
    const bool haveCtdb = !ctdbEntries.empty();

    for (int t = 0; t < total; ++t) {
        QString status;
        if (tainted[t]) {
            status = QStringLiteral("fail");
            ++failCount;
        } else if (!haveAr && !haveCtdb) {
            status = QStringLiteral("unknown");
            ++unknownCount;
        } else {
            bool arMatched = false;
            int  arConf    = 0;
            if (haveAr) {
                const auto sums = arverify::checksumsAtOffset(
                    canonical, discFrames, spans[t], 0, t + 1, total);
                if (sums) {
                    for (const auto& e : arEntries) {
                        if (t >= int(e.tracks.size())) continue;
                        const auto& dbT = e.tracks[size_t(t)];
                        if (dbT.crc == sums->v1 || dbT.crc == sums->v2) {
                            arMatched = true;
                            arConf = std::max<int>(arConf, dbT.confidence);
                        }
                    }
                }
            }

            bool ctdbMatched = false;
            int  ctdbConf    = 0;
            if (haveCtdb) {
                const auto crc = arverify::ctdbChecksumAtOffset(
                    canonical, discFrames, spans[t], 0);
                if (crc) {
                    for (const auto& e : ctdbEntries) {
                        if (t >= int(e.trackCrcs.size())) continue;
                        if (e.trackCrcs[size_t(t)] == *crc) {
                            ctdbMatched = true;
                            ctdbConf = std::max<int>(ctdbConf, e.confidence);
                        }
                    }
                }
            }

            if (arMatched || ctdbMatched) {
                status = QStringLiteral("ok");
                ++okCount;
                bestArConf   = std::max(bestArConf, arConf);
                bestCtdbConf = std::max(bestCtdbConf, ctdbConf);
            } else {
                status = QStringLiteral("warn");
                ++warnCount;
            }
        }
        emit verifyTrackResult(t + 1, status);
    }

    // Summary line.
    QString summary;
    if (haveAr || haveCtdb) {
        summary = QStringLiteral("%1/%2 ACCURATE").arg(okCount).arg(total);
        if (warnCount > 0)
            summary += QStringLiteral("  ·  %1 with warning%2")
                        .arg(warnCount)
                        .arg(warnCount == 1 ? "" : "s");
        if (failCount > 0)
            summary += QStringLiteral("  ·  %1 zero-filled").arg(failCount);
        summary += offsetOpt
            ? QStringLiteral("  ·  AR offset 0")
            : QStringLiteral("  ·  drive offset unknown");
        if (haveCtdb && bestCtdbConf > 0)
            summary += QStringLiteral("  ·  CTDB conf %1").arg(bestCtdbConf);
    } else {
        summary = QStringLiteral("Not in AR or CTDB — accuracy unknown");
    }
    emit verifyComplete(summary);

    // --- Sidecars + ready-to-save -----------------------------------
    const double elapsedSec =
        std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count();
    writeRipLog(tempDir.toStdString(), selected, toc, ids,
                driveOffset, offsetOpt.has_value(),
                chosen, zeroFilledRanges, elapsedSec, summary.toStdString());
    writeCueSheet(tempDir.toStdString(), toc, chosen);

    // suggestedName was pre-computed above (before tempDir creation)
    // so we could write directly into the parent folder for batch
    // resumes. Use the same name now for the save handoff.
    emit readyToSave(tempDir, suggestedName);
    // doSave / discardStagedRip take it from here.
}

void RipWorker::doSave(const QString& parentFolder, const QString& folderName) {
    if (m_currentTempDir.isEmpty()) {
        emit failed(QStringLiteral("Internal error: no staged rip to save."));
        return;
    }
    namespace fs = std::filesystem;
    const fs::path src(m_currentTempDir.toStdString());

    QString name = folderName;
    name.replace(QChar('/'), QChar('_'));
    if (name.isEmpty()) name = QStringLiteral("Untitled");

    fs::path dst = fs::path(parentFolder.toStdString()) / name.toStdString();

    // In-place fast path: when a batch rip wrote directly into
    // <parent>/<finalName>/ the dst is the src — nothing to move. Just
    // announce the save and let the playlist pick it up where it
    // already lives.
    std::error_code ec;
    if (fs::exists(src, ec) && fs::exists(dst, ec)
        && fs::equivalent(src, dst, ec))
    {
        const QString fromTemp = m_currentTempDir;
        m_currentTempDir.clear();
        emit discSaved(fromTemp, QString::fromStdString(dst.string()));
        return;
    }

    // Out-of-place: disambiguate if a folder with that name already
    // exists at the destination.
    if (fs::exists(dst)) {
        for (int n = 2; n < 1000; ++n) {
            fs::path alt = fs::path(parentFolder.toStdString())
                / (name.toStdString() + " (" + std::to_string(n) + ")");
            if (!fs::exists(alt)) { dst = std::move(alt); break; }
        }
    }

    const QString fromTemp = m_currentTempDir;
    std::string err;
    if (!moveDirectory(src, dst, err)) {
        emit failed(QStringLiteral("Couldn't move rip into place:\n%1")
                        .arg(QString::fromStdString(err)));
        return;
    }
    m_currentTempDir.clear();
    emit discSaved(fromTemp, QString::fromStdString(dst.string()));
}

} // namespace plyr::cd
