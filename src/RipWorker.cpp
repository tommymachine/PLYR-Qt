// RipWorker — port of cdrip_main.cpp::runRip, shaped for the GUI thread.
// See RipWorker.h for the surface; this file is the implementation.

#include "RipWorker.h"

#include "ArVerify.h"
#include "CdDevice.h"
#include "DriveOffsetDb.h"
#include "FlacDecode.h"
#include "FlacEncode.h"
#include "FlacTags.h"
#include "MetadataResolver.h"
#include "MetadataScoring.h"
#include "MusicBrainz.h"
#include "PendingSubmissions.h"
#include "SystemPaths.h"

#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QStringList>
#include <QUuid>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace concerto::cd {

namespace {

constexpr const char* kAccurateRipBase = "http://www.accuraterip.com/accuraterip/";
constexpr const char* kCtdbBase        = "http://db.cuetools.net/";
constexpr const char* kUserAgent       = "Concerto-Ripper/0.1";

// Same per-call chunk size cdparanoia uses and runRip used. ~62 KiB —
// big enough that ioctl round-trip is amortized, small enough that any
// SCSI / USB transfer cap is honored.
constexpr uint32_t kReadChunkSectors = 27;

// Raw-sectors sidecar written incrementally during the read loop. Each
// 2352-byte sector goes in at the position it was read, with no offset
// correction. Lets a force-quit mid-rip resume at LBA granularity instead
// of having to re-read tracks that hadn't finished encoding yet. The
// design brief lives in the project memory under "Concerto CD ripper
// direction" — search for the "LBA-granular resume" section.
constexpr const char* kRawSidecarName = "cd_rip.raw";

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

// Bridge concerto::cd::Toc into the arverify-side TOC (absolute, 150-based).
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

// Legacy tag builder — used as a fallback when the resolver hasn't
// produced an AlbumMeta yet (e.g. MB unreachable on Stage 1). When the
// resolver supplies an album, concerto::metadata::buildVorbisTags() (in
// FlacTags.h) produces the rich classical tag set instead.
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
                      const QString& preferredParentFolder,
                      const QString& resumeTempDir,
                      const QString& resumeMbDiscId) {
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

    // --- Resolve rich metadata via the v1 pipeline ------------------
    // Single async resolver call; we block on `resolved` via QEventLoop
    // at this stage boundary (mirrors the existing pattern used elsewhere
    // in this file). The new async client is the foundation for moving
    // the resolver fully off the nested-event-loop pattern; the eventual
    // shape is one resolver kicked off in parallel with the read loop,
    // pumped via QCoreApplication::processEvents at well-defined yield
    // points. v1 keeps it synchronous to avoid touching the read loop.
    concerto::metadata::AlbumMeta resolvedAlbum;
    {
        concerto::metadata::MetadataResolver resolver;
        resolver.setUserAgent(QString::fromLatin1(kUserAgent));

        concerto::metadata::MetadataResolver::Request mreq;
        mreq.discId      = QString::fromStdString(ids.musicBrainzDiscId);
        mreq.trackCount  = static_cast<int>(toc.tracks.size());
        mreq.tocSummary.discId      = mreq.discId;
        mreq.tocSummary.trackCount  = mreq.trackCount;
        mreq.tocSummary.trackLengthsSec.reserve(toc.tracks.size());
        for (size_t i = 0; i < toc.tracks.size(); ++i) {
            const uint32_t nextLba = (i + 1 < toc.tracks.size())
                ? toc.tracks[i + 1].startLba : toc.leadOutLba;
            mreq.tocSummary.trackLengthsSec.push_back(
                static_cast<int>((nextLba - toc.tracks[i].startLba) / 75));
        }
        mreq.tocForSubmissions.firstTrack = toc.tracks.front().trackNumber;
        mreq.tocForSubmissions.lastTrack  = toc.tracks.back().trackNumber;
        mreq.tocForSubmissions.leadoutLba = toc.leadOutLba;
        for (const auto& t : toc.tracks)
            mreq.tocForSubmissions.offsets.push_back(t.startLba + 150);

        QEventLoop loop;
        QObject::connect(&resolver, &concerto::metadata::MetadataResolver::resolved,
                         &loop, [&](const concerto::metadata::AlbumMeta& a,
                                    const QString& /*src*/) {
                             resolvedAlbum = a;
                             loop.quit();
                         });
        resolver.resolve(mreq);
        loop.exec();
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
    bool reusingTempDir = false;
    // Honor the resume only if the inserted disc's mbDiscId matches the
    // saved batch's expectation. A mismatch means the user inserted a
    // different disc under the same batch (likely the next pending one)
    // — start fresh and let the Ripper observe in_progress -> done on
    // save like any normal rip.
    const QString computedMbDiscId =
        QString::fromStdString(ids.musicBrainzDiscId);
    const bool discIdMatches =
        !resumeMbDiscId.isEmpty() && computedMbDiscId == resumeMbDiscId;
    if (!resumeTempDir.isEmpty() && !discIdMatches) {
        qWarning("RipWorker: ignoring resume tempDir, disc id mismatch "
                 "(saved=%s inserted=%s)",
                 resumeMbDiscId.toUtf8().constData(),
                 computedMbDiscId.toUtf8().constData());
    } else if (!resumeTempDir.isEmpty() && QDir(resumeTempDir).exists()) {
        // Resume path: reuse the previous attempt's dir so the existing
        // track_NN.flac files are preserved. The original suggestedName
        // is not honored here — whatever the prior dir is called wins,
        // and doSave will move/rename it at the end if needed. See the
        // brief: "preferredParentFolder conflict" is intentional.
        tempDir = resumeTempDir;
        reusingTempDir = true;
    }
    if (tempDir.isEmpty()) {
        if (!resumeTempDir.isEmpty() && discIdMatches) {
            // The persisted path is gone (user emptied trash, moved the
            // dir, etc.). Fall back to a fresh rip and tell the user.
            emit warning(QStringLiteral(
                "Resume directory missing — starting over:\n%1").arg(resumeTempDir));
        }
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
            const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces)
                                     .remove(QChar('-'));
            tempDir = concerto::paths::appDataDir()
                + QStringLiteral("/rip_in_progress/") + uuid;
        }
    }
    if (!QDir().mkpath(tempDir)) {
        emit failed(QStringLiteral("Couldn't create temp directory:\n%1").arg(tempDir));
        return;
    }
    m_currentTempDir = tempDir;
    emit ripStarting(tempDir);

    // LBA-granular resume: load any existing cd_rip.raw into the disc
    // buffer BEFORE looking at the FLACs. The raw file is appended
    // sector-by-sector during the read loop; on resume its size tells
    // us exactly which LBAs were already read. See the design brief
    // (memory: "LBA-granular resume").
    //
    // Math walk: if existingBytes = 1000 * 2352, then 1000 sectors were
    // read. Those bytes were written sequentially to the file as they
    // landed in disc[mainStart .. mainStart + 1000*2352). On resume we
    // load them back into the same region; lba = firstLba + 1000,
    // writeAt = mainStart + 1000*2352, readSoFar = 1000, remaining =
    // totalSectors - 1000. Symmetric with the write path.
    uint32_t rawSectorsRead = 0;
    uint64_t rawBytesLoaded = 0;
    const QString rawPath = tempDir + QStringLiteral("/")
                            + QString::fromLatin1(kRawSidecarName);
    if (reusingTempDir && QFile::exists(rawPath)) {
        const qint64 fsize = QFileInfo(rawPath).size();
        if (fsize > 0) {
            // Round down: a force-quit mid-chunk-write can leave a
            // partial trailing sector. We re-read those bytes rather
            // than trust them.
            uint64_t bytes = static_cast<uint64_t>(fsize);
            bytes = (bytes / 2352) * 2352;
            const uint64_t mainCapacity = paddedBytes - mainStart;
            if (bytes > mainCapacity) {
                // Should never happen — a raw file bigger than the main
                // region means the TOC changed under us. Treat as
                // corrupt and start fresh.
                qWarning("RipWorker: cd_rip.raw oversized (%llu > %llu); "
                         "ignoring and starting over.",
                         static_cast<unsigned long long>(bytes),
                         static_cast<unsigned long long>(mainCapacity));
            } else {
                std::ifstream raw(rawPath.toStdString(),
                                  std::ios::binary);
                if (raw) {
                    raw.read(reinterpret_cast<char*>(disc.data() + mainStart),
                             static_cast<std::streamsize>(bytes));
                    const std::streamsize got = raw.gcount();
                    if (got > 0) {
                        rawBytesLoaded = static_cast<uint64_t>(got);
                        rawSectorsRead = static_cast<uint32_t>(
                            rawBytesLoaded / 2352);
                        // Trim to whole sectors again in case read was short.
                        rawBytesLoaded = uint64_t{rawSectorsRead} * 2352;
                    }
                }
                // Trim any torn trailing partial sector on disk so the
                // append-mode writes below resume at a clean boundary.
                // If we left a fractional sector behind, the next
                // force-quit would compute the wrong resume LBA from
                // file size.
                if (rawBytesLoaded < static_cast<uint64_t>(fsize)) {
                    std::error_code ec;
                    std::filesystem::resize_file(
                        rawPath.toStdString(), rawBytesLoaded, ec);
                }
            }
        }
    }

    // Open the raw sidecar for appending. Same handle is reused for
    // every chunk write in the read loop — no allocations in the hot
    // path. We flush after every chunk: drive throughput tops out around
    // 1–2 MB/s, so the disk-write rate is well below what an SSD can
    // absorb, and a per-chunk flush keeps the force-quit data loss
    // window to the last chunk (~360 ms of audio) instead of whatever
    // the OS's writeback cadence happens to be.
    std::ofstream rawFile(rawPath.toStdString(),
                          std::ios::binary | std::ios::out | std::ios::app);
    if (!rawFile) {
        qWarning("RipWorker: couldn't open cd_rip.raw for append at %s",
                 rawPath.toUtf8().constData());
        // Non-fatal — the rip still works without LBA-granular resume.
    }

    // Resume scan: enumerate existing track_NN.flac files in tempDir,
    // decode each one back into the disc buffer at its offset-corrected
    // position so the end-of-rip AR/CTDB verification can run against
    // the full buffer. Tracks that decode successfully are skipped by
    // the read loop. Decode failures are silently re-ripped (the FLAC
    // is treated as not-present) — never fail the resume because a
    // saved track is corrupt.
    //
    // When cd_rip.raw covers a track's offset-corrected byte range we
    // skip the FLAC decode (the raw bytes are already in the buffer)
    // but still emit `encodedTrack` so the playlist sees it.
    std::set<int> alreadyEncoded;
    if (reusingTempDir) {
        QDir dir(tempDir);
        const auto entries = dir.entryList(
            QStringList{QStringLiteral("track_*.flac")}, QDir::Files);
        const QRegularExpression rx(QStringLiteral("^track_(\\d+)\\.flac$"));
        for (const auto& name : entries) {
            const auto m = rx.match(name);
            if (!m.hasMatch()) continue;
            const int trackNumber = m.captured(1).toInt();
            // Find this trackNumber in the TOC. If it doesn't match
            // (different disc inserted under the same temp dir somehow),
            // skip and let the regular read handle it.
            size_t trackIdx = toc.tracks.size();
            for (size_t i = 0; i < toc.tracks.size(); ++i) {
                if (int(toc.tracks[i].trackNumber) == trackNumber) {
                    trackIdx = i; break;
                }
            }
            if (trackIdx >= toc.tracks.size()) continue;

            const QString flacPath = dir.absoluteFilePath(name);

            // Offset-corrected destination — same math tryEncodeReadyTracks
            // uses to decide where each track's PCM is located:
            //   byteIdx = mainStart                  (skip the head pad)
            //           + (startLba - firstLba) * 2352   (LBA offset)
            //           + driveOffset * 4                (per-sample 4-byte
            //                                            stereo-16 offset)
            const auto& t = toc.tracks[trackIdx];
            const uint32_t nextLba  = (trackIdx + 1 < toc.tracks.size())
                                        ? toc.tracks[trackIdx + 1].startLba
                                        : toc.leadOutLba;
            const uint32_t startSec = t.startLba - toc.tracks.front().startLba;
            const uint64_t trackFrames = uint64_t{nextLba - t.startLba} * 588;
            const int64_t  byteIdx  = static_cast<int64_t>(mainStart)
                                      + int64_t{startSec} * 2352
                                      + int64_t{driveOffset} * 4;
            const uint64_t trackEndByte = uint64_t(byteIdx) + trackFrames * 4;

            // If the raw sidecar already covers this track's full byte
            // range, the buffer is already populated from raw — skip the
            // FLAC decode entirely. We still emit the encodedTrack signal
            // so the playlist surfaces the saved file.
            if (rawBytesLoaded > 0
                && byteIdx >= static_cast<int64_t>(mainStart)
                && trackEndByte <= mainStart + rawBytesLoaded)
            {
                alreadyEncoded.insert(trackNumber);
                emit encodedTrack(trackNumber, flacPath);
                continue;
            }

            const auto decoded = flacdecode::decodeFile(flacPath.toStdString());
            if (!decoded || !decoded->info.isCdFormat()) {
                // Treat as not-done; the read loop will re-rip the
                // sectors covering this track and overwrite the FLAC.
                continue;
            }

            const uint64_t framesBytes = decoded->frames * 4;
            if (byteIdx < 0
                || uint64_t(byteIdx) + framesBytes > disc.size())
            {
                // Out-of-bounds (TOC mismatch with the saved FLAC) —
                // treat as not-done.
                continue;
            }
            std::memcpy(disc.data() + byteIdx,
                        decoded->pcm.data(),
                        framesBytes);
            alreadyEncoded.insert(trackNumber);
            // Populate the playlist so the user sees the already-encoded
            // tracks as soon as the resume starts.
            emit encodedTrack(trackNumber, flacPath);
        }
    }

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
    //
    // The actual emit is deferred until after the resume LBA is
    // computed below — on resume we also need to pass the disc-
    // relative startOffsetMs so the position counter starts at the
    // right spot rather than at zero.
    const qint64 discDurationMs =
        (qint64(totalSectors) * 1000) / 75;

    // Resume hop: pick the LBA where the read loop starts.
    //
    // Two inputs:
    //   * rawSectorsRead — bytes already in cd_rip.raw, sector-aligned.
    //     Source of truth when non-zero.
    //   * alreadyEncoded — tracks whose FLACs landed last time. Used
    //     only when raw is absent (pre-raw-support resumes, or a fresh
    //     attempt that never wrote any sectors).
    //
    // Buffer-byte-offset note: writeAt is the head-of-pad-aware byte
    // position. (lba - firstLba) * 2352 advances writeAt one sector
    // (2352 bytes) per LBA past firstLba, matching what one read-chunk
    // iteration would advance had we started from the front.
    size_t resumeTrackIdx = 0;
    while (resumeTrackIdx < toc.tracks.size()
           && alreadyEncoded.count(int(toc.tracks[resumeTrackIdx].trackNumber)))
    {
        ++resumeTrackIdx;
    }

    uint32_t resumeReadSoFar;
    int32_t  resumeLba;
    size_t   resumeCurrentTrackIdx;
    if (rawSectorsRead > 0) {
        // Raw is canonical — pick up exactly where the file ends.
        resumeReadSoFar = rawSectorsRead;
        resumeLba       = firstLba + static_cast<int32_t>(rawSectorsRead);
        // Walk the TOC to find which track contains the resume LBA.
        resumeCurrentTrackIdx = 0;
        for (size_t i = 0; i < toc.tracks.size(); ++i) {
            const uint32_t nextLba = (i + 1 < toc.tracks.size())
                                       ? toc.tracks[i + 1].startLba
                                       : toc.leadOutLba;
            if (static_cast<uint32_t>(resumeLba) < nextLba) {
                resumeCurrentTrackIdx = i;
                break;
            }
            resumeCurrentTrackIdx = i;
        }
    } else {
        // No raw → fall back to track-aligned resume.
        resumeLba = (resumeTrackIdx < toc.tracks.size())
            ? static_cast<int32_t>(toc.tracks[resumeTrackIdx].startLba)
            : static_cast<int32_t>(toc.leadOutLba);
        resumeReadSoFar       = uint32_t(resumeLba - firstLba);
        resumeCurrentTrackIdx = std::min(resumeTrackIdx,
                                         toc.tracks.size() - 1);
    }

    // Now we know how far in we're starting — kick off the streaming
    // preview with both the disc total and the start offset. CDDA is
    // 75 sectors/sec, so resumeReadSoFar sectors = N*1000/75 ms into
    // the disc.
    const qint64 startOffsetMs =
        (qint64(resumeReadSoFar) * 1000) / 75;
    emit previewStreamStart(discDurationMs, startOffsetMs);

    uint32_t remaining   = totalSectors - resumeReadSoFar;
    int32_t  lba         = resumeLba;
    uint64_t writeAt     = mainStart + uint64_t{resumeReadSoFar} * 2352;
    uint32_t readSoFar   = resumeReadSoFar;
    auto     milestoneT0 = std::chrono::steady_clock::now();
    uint32_t milestoneSectors = 0;
    int      currentTrackNumber =
        int(toc.tracks[resumeCurrentTrackIdx].trackNumber);
    size_t   currentTrackIndex  = resumeCurrentTrackIdx;
    size_t   encodedThrough     = resumeTrackIdx;  // count of tracks already FLAC'd

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
    // index that's been read into the buffer (exclusive). Tracks
    // already present in `alreadyEncoded` (resumed-from-disk) are
    // counted as done without re-encoding.
    auto tryEncodeReadyTracks = [&](uint64_t writtenBytes) -> bool {
        while (encodedThrough < toc.tracks.size()) {
            const auto&    t        = toc.tracks[encodedThrough];
            if (alreadyEncoded.count(int(t.trackNumber))) {
                ++encodedThrough;
                continue;
            }
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
            if (!resolvedAlbum.isEmpty()
                && resolvedAlbum.sourceTag != QLatin1String("stub")) {
                tags = concerto::metadata::buildVorbisTags(
                    resolvedAlbum, static_cast<int>(encodedThrough));
            } else if (chosen) {
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
            // Preserve the temp dir + cd_rip.raw + already-encoded
            // FLACs so the user can resume from this point on next
            // launch. Ripper::stopRip(deleteBatch=true) handles the
            // explicit-delete path by invoking discardStagedRip
            // itself after the worker comes to rest.
            emit previewStreamStop();
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

        // Persist this chunk to cd_rip.raw so a force-quit can resume
        // here. We write the zero-filled bytes too — the file's sector
        // count must mirror the in-memory buffer's coverage so the load
        // path on resume can use file size as ground truth. flush()
        // forces the userland buffer to the OS; the kernel's writeback
        // handles the rest.
        if (rawFile) {
            rawFile.write(reinterpret_cast<const char*>(view.data()),
                          static_cast<std::streamsize>(view.size()));
            rawFile.flush();
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
        if (!wasZeroFilled) {
            writeAt += uint64_t{padSectors} * 2352;
            // Persist the probe too so resume picks up the post-pad
            // bytes for positive-offset drives. Skip when zero-filled
            // — the in-memory zero-init already represents that on
            // resume; no point persisting a zero block.
            if (rawFile) {
                rawFile.write(reinterpret_cast<const char*>(tail.data()),
                              static_cast<std::streamsize>(tail.size()));
                rawFile.flush();
            }
        }
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
        // Preserve temp dir for resume — see comment in the read loop.
        emit previewStreamStop();
        emit ripCancelled();
        return;
    }
    emit encodingComplete();

    if (m_cancel.load(std::memory_order_acquire)) {
        // Preserve temp dir for resume — see comment in the read loop.
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

    // The FLACs are now canonical (encoded and verified) so cd_rip.raw
    // has no further job. Close the handle and unlink the file. If the
    // user force-quits between here and doSave the worst case is that
    // they re-run AR/CTDB on resume — which is cheap and avoids
    // shipping a 700 MB raw blob into the saved album folder.
    if (rawFile.is_open()) rawFile.close();
    {
        std::error_code ec;
        std::filesystem::remove(rawPath.toStdString(), ec);
        // Non-fatal if the unlink fails; the doSave belt-and-braces
        // path will retry before the move.
    }

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

    // Belt-and-braces unlink of the raw sidecar. doRip already deletes
    // it after verifyComplete, but if the user force-quit between the
    // verify and now there could still be one on disk. Don't ship a
    // hundreds-of-MB raw blob into the user's library.
    {
        std::error_code ec;
        fs::remove(src / kRawSidecarName, ec);
    }

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

} // namespace concerto::cd
