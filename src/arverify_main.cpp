// Standalone CLI: verify ripped FLAC discs against AccurateRip.
//
//   arverify_cli <disc-folder> [<disc-folder> ...]
//
// Each folder is treated as one CD: its *.flac files, sorted by name, are the
// tracks in order. The tool reconstructs the disc TOC from the track lengths,
// looks the disc up in AccurateRip, decodes the entire disc into one packed
// stereo-sample buffer, scans for the disc-wide pressing offset that best
// matches the database, and reports per-track accuracy at that offset.

#include "ArVerify.h"
#include "FlacDecode.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <QUrl>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace {

QTextStream out(stdout);
QTextStream err(stderr);

constexpr const char* kUserAgent = "plyr-arverify/0.1";
constexpr const char* kAccurateRipBase = "http://www.accuraterip.com/accuraterip/";
constexpr const char* kCtdbBase = "http://db.cuetools.net/";

// Synchronous HTTP GET. `status` receives the HTTP status code (0 on a
// transport-level failure) and `errorText` the reply's error string, if any.
QByteArray httpGet(QNetworkAccessManager& nam, const QUrl& url,
                   int& status, QString& errorText) {
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(kUserAgent));
    QNetworkReply* reply = nam.get(req);

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    errorText = (reply->error() != QNetworkReply::NoError) ? reply->errorString()
                                                           : QString();
    const QByteArray body = reply->readAll();
    reply->deleteLater();
    return body;
}

// A disc folder's *.flac files, as absolute paths in track order. The rip
// names tracks "01 - ...", "02 - ..." etc., so a plain name sort is correct.
QStringList flacFilesInOrder(const QString& folder) {
    QDir dir(folder);
    QStringList files = dir.entryList(QStringList{QStringLiteral("*.flac")},
                                      QDir::Files, QDir::Name);
    for (QString& f : files)
        f = dir.absoluteFilePath(f);
    return files;
}

QString hex8(uint32_t v) {
    return QStringLiteral("%1").arg(v, 8, 16, QChar('0'));
}

// Verify one disc folder. Returns 0 on success (whatever the rip's accuracy),
// 1 on a hard failure that prevented verification.
int verifyDisc(QNetworkAccessManager& nam, const QString& folder) {
    const QString discName = QDir(folder).dirName();
    const QStringList tracks = flacFilesInOrder(folder);
    if (tracks.isEmpty()) {
        err << discName << ": no .flac files found\n";
        return 1;
    }

    // --- Pass 1: STREAMINFO only -> per-track sample counts -> TOC -> ids ---
    std::vector<uint64_t> sampleCounts;
    sampleCounts.reserve(static_cast<size_t>(tracks.size()));
    for (const QString& path : tracks) {
        const auto si = flacdecode::readStreamInfo(path.toStdString());
        if (!si || !si->isCdFormat()) {
            err << discName << ": " << QDir(folder).relativeFilePath(path)
                << " is not Red Book CD audio (44100 / stereo / 16-bit)\n";
            return 1;
        }
        sampleCounts.push_back(si->totalSamples);
    }

    const auto toc = arverify::reconstructToc(sampleCounts);
    if (!toc) {
        err << discName << ": tracks are not sector-aligned; the disc TOC "
               "cannot be reconstructed\n";
        return 1;
    }
    const arverify::DiscIds ids = arverify::computeDiscIds(*toc);
    const int total = toc->trackCount();

    out << "\n=== " << discName << "  (" << total << " tracks) ===\n";
    out << "  disc id: " << hex8(ids.accurateRipId1) << "-"
        << hex8(ids.accurateRipId2) << "-" << hex8(ids.cddbId) << "\n";
    out.flush();

    // --- AccurateRip lookup ---
    std::vector<arverify::ArDiscEntry> entries;
    {
        const QUrl url(QString::fromLatin1(kAccurateRipBase)
                       + QString::fromStdString(arverify::accurateRipPath(*toc, ids)));
        int status = 0;
        QString netError;
        const QByteArray body = httpGet(nam, url, status, netError);
        if (status == 0) {
            out << "  AccurateRip lookup failed: " << netError << "\n";
        } else if (status != 200) {
            out << "  AccurateRip: not in the database (HTTP " << status << ")\n";
        } else {
            entries = arverify::parseAccurateRipResponse(
                reinterpret_cast<const uint8_t*>(body.constData()),
                static_cast<size_t>(body.size()));
            out << "  AccurateRip: " << entries.size() << " pressing(s) on file\n";
        }
        out.flush();
    }

    // --- CTDB (CueTools) lookup: independent consensus pool, CRC32-based ---
    std::vector<arverify::CtdbEntry> ctdbEntries;
    {
        const QUrl url(QString::fromLatin1(kCtdbBase)
                       + QString::fromStdString(arverify::ctdbLookupPath(*toc)));
        int status = 0;
        QString netError;
        const QByteArray body = httpGet(nam, url, status, netError);
        if (status == 0) {
            out << "  CTDB lookup failed: " << netError << "\n";
        } else if (status != 200) {
            out << "  CTDB: HTTP " << status << "\n";
        } else {
            ctdbEntries = arverify::parseCtdbResponse(
                reinterpret_cast<const uint8_t*>(body.constData()),
                static_cast<size_t>(body.size()));
            out << "  CTDB: " << ctdbEntries.size() << " entry(s) on file\n";
        }
        out.flush();
    }

    if (entries.empty() && ctdbEntries.empty()) {
        out << "  neither AR nor CTDB has this disc — nothing to verify against\n";
        out.flush();
        return 0;
    }

    // Distinct AR CRCs per track across all pressings — input to the offset
    // scan. Same data feeds the per-track match below, but iterated
    // differently there (we want confidence + which pressing matched).
    std::vector<std::vector<uint32_t>> dbCrcsPerTrack(static_cast<size_t>(total));
    for (const auto& entry : entries) {
        const int n = std::min(entry.trackCount, total);
        for (int t = 0; t < n; ++t) {
            const uint32_t crc = entry.tracks[static_cast<size_t>(t)].crc;
            auto& list = dbCrcsPerTrack[static_cast<size_t>(t)];
            if (std::find(list.begin(), list.end(), crc) == list.end())
                list.push_back(crc);
        }
    }

    // --- Pass 2: decode whole disc into one packed-stereo sample buffer ---
    // The offset scan needs random access across the entire disc, so we hold
    // every sample at once. Track-by-track int16 PCM is converted to AR's
    // packed-uint32 form and the int16 buffer is freed as we go.
    std::vector<arverify::ArSample> disc;
    std::vector<arverify::TrackSpan> spans(static_cast<size_t>(total));
    {
        uint64_t totalFrames = 0;
        for (uint64_t s : sampleCounts) totalFrames += s;
        disc.reserve(totalFrames);
    }
    for (int t = 0; t < total; ++t) {
        const auto audio = flacdecode::decodeFile(tracks[t].toStdString());
        if (!audio) {
            err << discName << ": track " << (t + 1) << " decode failed\n";
            return 1;
        }
        const uint64_t spanStart = disc.size();
        for (uint64_t i = 0; i < audio->frames; ++i) {
            disc.push_back(arverify::packStereo(audio->pcm[2 * i],
                                                audio->pcm[2 * i + 1]));
        }
        spans[static_cast<size_t>(t)] = { spanStart, disc.size() };
    }

    // --- AR offset scan: fast v1-only prefix-sum, with a v2-aware slow
    //     fallback for discs whose DB entries are stored as v2 CRCs.
    int arOffset = 0;
    if (entries.empty()) {
        out << "  AR offset: skipped (no AR entries)\n";
    } else {
        const arverify::OffsetMatch scan =
            arverify::scanForOffset(disc.data(), disc.size(), spans, dbCrcsPerTrack);

        if (scan.midTrackCount == 0) {
            out << "  AR offset: skipped (need >= 3 tracks)\n";
        } else if (scan.hits > 0) {
            arOffset = scan.offset;
            out << "  AR offset: " << (arOffset >= 0 ? "+" : "")
                << arOffset << "  (" << scan.hits << "/" << scan.midTrackCount
                << " middle tracks via fast v1)\n";
        } else {
            out << "  AR offset: v1 found no match — trying v1+v2 slow scan\n";
            out.flush();
            const auto slow = arverify::scanForOffsetSlow(
                disc.data(), disc.size(), spans, dbCrcsPerTrack);
            if (slow) {
                arOffset = *slow;
                out << "  AR offset: " << (arOffset >= 0 ? "+" : "")
                    << arOffset << "  (via v2 fallback)\n";
            } else {
                out << "  AR offset: no match within +/-3000 frames — verifying at 0\n";
            }
        }
    }
    out.flush();

    // --- CTDB offset scan: CTDB groups submissions independently of AR, so
    //     the offset for a given TOC routinely differs between the two
    //     pools. Scanned the same way as AR's slow fallback but with CRC32.
    int ctdbOffset = 0;
    if (ctdbEntries.empty()) {
        out << "  CTDB offset: skipped (no CTDB entries)\n";
    } else {
        const auto found = arverify::scanForOffsetCtdb(
            disc.data(), disc.size(), spans, ctdbEntries);
        if (found) {
            ctdbOffset = *found;
            out << "  CTDB offset: " << (ctdbOffset >= 0 ? "+" : "")
                << ctdbOffset << "  (CRC32 slow scan)\n";
        } else {
            out << "  CTDB offset: no match within +/-3000 frames\n";
        }
    }
    out.flush();

    // --- Per-track verification ---
    // AR uses arOffset; CTDB uses ctdbOffset (the two pools group
    // submissions into different per-pressing clusters, so the offset
    // routinely differs). Each pool's edge state is computed against its
    // own offset.
    int arAccurate = 0, arEvaluated = 0, arEdge = 0;
    int ctdbAccurate = 0, ctdbEvaluated = 0, ctdbEdge = 0;
    for (int t = 0; t < total; ++t) {
        const int trackNo = t + 1;
        const arverify::TrackSpan& span = spans[static_cast<size_t>(t)];

        // AR side.
        QString arVerdict;
        if (entries.empty()) {
            arVerdict = QStringLiteral("AR —");
        } else {
            const auto sums = arverify::checksumsAtOffset(
                disc.data(), disc.size(), span, arOffset, trackNo, total);
            if (!sums) {
                ++arEdge;
                arVerdict = QStringLiteral("AR edge");
            } else {
                ++arEvaluated;
                bool matched = false;
                int bestConf = 0;
                for (const auto& entry : entries) {
                    if (t >= static_cast<int>(entry.tracks.size()))
                        continue;
                    const auto& dbT = entry.tracks[static_cast<size_t>(t)];
                    if (dbT.crc == sums->v1 || dbT.crc == sums->v2) {
                        matched = true;
                        bestConf = std::max<int>(bestConf, dbT.confidence);
                    }
                }
                if (matched) {
                    ++arAccurate;
                    arVerdict = QStringLiteral("AR ACCURATE (%1)").arg(bestConf);
                } else {
                    arVerdict = QStringLiteral("AR NOT ACCURATE");
                }
            }
        }

        // CTDB side.
        QString ctdbVerdict;
        if (ctdbEntries.empty()) {
            ctdbVerdict = QStringLiteral("CTDB —");
        } else {
            const auto ctdbCrc = arverify::ctdbChecksumAtOffset(
                disc.data(), disc.size(), span, ctdbOffset);
            if (!ctdbCrc) {
                ++ctdbEdge;
                ctdbVerdict = QStringLiteral("CTDB edge");
            } else {
                ++ctdbEvaluated;
                bool matched = false;
                int bestConf = 0;
                for (const auto& entry : ctdbEntries) {
                    if (t >= static_cast<int>(entry.trackCrcs.size()))
                        continue;
                    if (entry.trackCrcs[static_cast<size_t>(t)] == *ctdbCrc) {
                        matched = true;
                        bestConf = std::max<int>(bestConf, entry.confidence);
                    }
                }
                if (matched) {
                    ++ctdbAccurate;
                    ctdbVerdict = QStringLiteral("CTDB ACCURATE (%1)").arg(bestConf);
                } else {
                    ctdbVerdict = QStringLiteral("CTDB NOT ACCURATE");
                }
            }
        }

        out << QStringLiteral("  track %1: %2   %3\n")
                   .arg(trackNo, 2).arg(arVerdict, -20).arg(ctdbVerdict);
        out.flush();
    }

    out << "  summary:";
    if (!entries.empty()) {
        out << " AR " << arAccurate << "/" << arEvaluated;
        if (arEdge > 0) out << " (" << arEdge << " edge)";
    }
    if (!ctdbEntries.empty()) {
        out << " CTDB " << ctdbAccurate << "/" << ctdbEvaluated;
        if (ctdbEdge > 0) out << " (" << ctdbEdge << " edge)";
    }
    out << "\n";
    out.flush();
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    const QStringList args = app.arguments();
    if (args.size() < 2) {
        err << "usage: arverify_cli <disc-folder> [<disc-folder> ...]\n";
        return 2;
    }

    QNetworkAccessManager nam;
    int rc = 0;
    for (int i = 1; i < args.size(); ++i)
        rc |= verifyDisc(nam, args[i]);
    return rc;
}
