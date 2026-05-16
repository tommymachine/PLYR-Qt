// MusicBrainz lookup + tag-encoding demo. Three modes:
//
//   mbquery_cli <disc-folder> [--tag-track-1 <out.flac>]
//       Reconstruct the TOC from a folder of FLACs, run the original
//       single-hop MB lookup, print the candidate releases.
//
//   mbquery_cli --disc-id <id> [--track-count <N>]
//       End-to-end Stage 1 resolution (disc-ID lookup → score-rank-pick
//       → release second-hop → flatten). Prints the per-track tag bundle
//       the rip pipeline would write.
//
//   mbquery_cli --self-test
//       Run the multi-candidate scoring smoke test (synthetic JSON).
//
// The --disc-id mode is the v1 metadata pipeline's end-to-end validation
// harness. Used to confirm the Ravel-test-case tags (METADATA_PLAN.md).

#include "ArVerify.h"
#include "FlacDecode.h"
#include "FlacEncode.h"
#include "FlacTags.h"
#include "MetadataModel.h"
#include "MetadataResolver.h"
#include "MetadataScoring.h"
#include "MusicBrainz.h"
#include "SystemPaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <QTimer>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

QTextStream out(stdout);
QTextStream err(stderr);

QStringList flacFilesInOrder(const QString& folder) {
    QDir dir(folder);
    QStringList files = dir.entryList(QStringList{QStringLiteral("*.flac")},
                                      QDir::Files, QDir::Name);
    for (QString& f : files)
        f = dir.absoluteFilePath(f);
    return files;
}

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
    return tags;
}

int runFolderMode(const QStringList& args)
{
    const QString folder = args.at(1);
    QString tagOutPath;
    for (int i = 2; i < args.size(); ++i) {
        if (args[i] == QStringLiteral("--tag-track-1") && i + 1 < args.size())
            tagOutPath = args[++i];
    }

    const QStringList tracks = flacFilesInOrder(folder);
    if (tracks.isEmpty()) {
        err << "no .flac files in " << folder << "\n";
        return 1;
    }

    std::vector<uint64_t> sampleCounts;
    sampleCounts.reserve(static_cast<size_t>(tracks.size()));
    for (const QString& path : tracks) {
        const auto si = flacdecode::readStreamInfo(path.toStdString());
        if (!si || !si->isCdFormat()) {
            err << path << ": not Red Book CD audio\n";
            return 1;
        }
        sampleCounts.push_back(si->totalSamples);
    }

    const auto toc = arverify::reconstructToc(sampleCounts);
    if (!toc) {
        err << "tracks aren't sector-aligned; can't reconstruct TOC\n";
        return 1;
    }
    const arverify::DiscIds ids = arverify::computeDiscIds(*toc);
    const int total = toc->trackCount();

    out << "=== " << QDir(folder).dirName() << "  (" << total << " tracks) ===\n";
    out << "  MB disc ID: " << QString::fromStdString(ids.musicBrainzDiscId) << "\n";
    out.flush();

    QNetworkAccessManager nam;
    const auto releases = musicbrainz::lookupByDiscId(
        nam, ids.musicBrainzDiscId, total);
    if (releases.empty()) {
        out << "  no MusicBrainz match\n";
        return 0;
    }
    out << "  " << releases.size() << " release(s) matched\n\n";

    for (size_t i = 0; i < releases.size(); ++i) {
        const musicbrainz::Release& r = releases[i];
        out << "[" << (i + 1) << "] " << QString::fromStdString(r.title) << "\n";
        out << "    artist:  " << QString::fromStdString(r.artist) << "\n";
        if (!r.date.empty())
            out << "    date:    " << QString::fromStdString(r.date) << "\n";
        if (!r.country.empty())
            out << "    country: " << QString::fromStdString(r.country) << "\n";
        out << "    release: " << QString::fromStdString(r.id) << "\n";
        out << "    disc:    " << r.disc.position << " of " << r.disc.totalCount
            << " (" << r.disc.tracks.size() << " tracks)\n";
        for (const auto& t : r.disc.tracks) {
            out << QStringLiteral("      %1. %2\n")
                       .arg(t.position, 2)
                       .arg(QString::fromStdString(t.title));
        }
        out << "\n";
        out.flush();
    }

    if (!tagOutPath.isEmpty()) {
        const musicbrainz::Release& rel = releases.front();
        out << "Re-encoding track 1 to " << tagOutPath
            << " with tags derived from release [1]...\n";
        const auto audio = flacdecode::decodeFile(tracks[0].toStdString());
        if (!audio) {
            err << "  decode of source track 1 failed\n";
            return 1;
        }
        const auto tags = tagsForTrack(rel, 0, ids.musicBrainzDiscId);
        if (!flacencode::encodeCdAudioToFile(
                tagOutPath.toStdString(),
                audio->pcm.data(), audio->frames,
                flacencode::EncoderConfig{}, tags)) {
            err << "  encode failed\n";
            return 1;
        }
        const auto roundTrip = flacdecode::readVorbisComments(
            tagOutPath.toStdString());
        out << "  wrote " << roundTrip.size() << " Vorbis comment(s); read back:\n";
        for (const auto& [field, value] : roundTrip) {
            out << "    " << QString::fromStdString(field) << " = "
                << QString::fromStdString(value) << "\n";
        }
    }
    return 0;
}

int runDiscIdMode(const QString& discId, int trackCount,
                  const std::vector<int>& lengthsSec)
{
    out << "=== Stage 1 resolution for disc-ID " << discId << " ===\n";
    out.flush();

    concerto::metadata::MetadataResolver resolver;
    resolver.setUserAgent(QStringLiteral(
        "concerto-metadata/0.1 ( https://github.com/tfletcher/plyr-qt )"));

    QObject::connect(&resolver, &concerto::metadata::MetadataResolver::stageChanged,
                     [](const QString& s) {
                         out << "[stage] " << s << "\n";
                         out.flush();
                     });

    concerto::metadata::MetadataResolver::Request req;
    req.discId     = discId;
    req.trackCount = trackCount;
    req.tocSummary.discId          = discId;
    req.tocSummary.trackCount      = trackCount;
    req.tocSummary.trackLengthsSec = lengthsSec;

    QEventLoop loop;
    concerto::metadata::AlbumMeta album;
    QString srcTag;
    QObject::connect(&resolver, &concerto::metadata::MetadataResolver::resolved,
                     &loop, [&](const concerto::metadata::AlbumMeta& a,
                                const QString& s) {
                         album = a;
                         srcTag = s;
                         loop.quit();
                     });
    QTimer::singleShot(0, &resolver, [&] { resolver.resolve(req); });
    loop.exec();

    out << "\n=== Resolved ===\n";
    out << "  source:        " << srcTag << "\n";
    out << "  album:         " << album.title << "\n";
    out << "  artistCredit:  " << album.artistCredit << "\n";
    out << "  date:          " << album.date << "\n";
    out << "  country:       " << album.country << "\n";
    out << "  barcode:       " << album.barcode << "\n";
    out << "  label:         " << album.label << "\n";
    out << "  catalog:       " << album.catalogNumber << "\n";
    out << "  releaseId:     " << album.releaseId << "\n";
    out << "  rgId:          " << album.releaseGroupId << "\n";
    out << "  disc:          " << album.discPosition << " of "
        << album.discTotalCount << "  (" << album.discSubtitle << ")\n";
    out << "  pick reason:   " << album.pickReason << "\n";
    out << "  confidence:    " << album.confidence << "\n";
    out << "  track count:   " << album.tracks.size() << "\n\n";

    for (int i = 0; i < album.tracks.size(); ++i) {
        out << QStringLiteral("--- Track %1 ---\n").arg(i + 1, 2, 10, QLatin1Char('0'));
        out << concerto::metadata::debugDumpTrack(album, i);
        out << "\n";
        out.flush();
    }

    out << "=== scoringLog ===\n";
    for (const QVariant& v : album.scoringLog) {
        const QVariantMap m = v.toMap();
        out << QStringLiteral("  %1  score=%2  %3\n")
                .arg(m.value(QStringLiteral("releaseId")).toString())
                .arg(m.value(QStringLiteral("score")).toInt())
                .arg(QString::fromUtf8(QJsonDocument::fromVariant(
                        m.value(QStringLiteral("components"))).toJson(
                            QJsonDocument::Compact)));
    }
    out << "\n=== pipelineLog ===\n";
    for (const QVariant& v : album.pipelineLog) {
        const QVariantMap m = v.toMap();
        out << QStringLiteral("  %1  %2  (%3 ms)\n")
                .arg(m.value(QStringLiteral("stage")).toString())
                .arg(m.value(QStringLiteral("outcome")).toString())
                .arg(m.value(QStringLiteral("durationMs")).toLongLong());
    }
    return 0;
}

// Synthetic three-candidate scoring smoke test. Builds a contrived
// disc-ID response with one "clean" release (curated country, barcode,
// classical-style credit) and two diluted siblings; asserts the cleaner
// one wins by a wide margin.
int runSelfTest()
{
    auto makeArtist = [](const QString& name, const QString& type, const QString& id) {
        QJsonObject a;
        a.insert(QStringLiteral("name"), name);
        if (!type.isEmpty()) a.insert(QStringLiteral("type"), type);
        if (!id.isEmpty())   a.insert(QStringLiteral("id"),   id);
        QJsonObject credit;
        credit.insert(QStringLiteral("name"), name);
        credit.insert(QStringLiteral("joinphrase"), QStringLiteral("; "));
        credit.insert(QStringLiteral("artist"), a);
        return credit;
    };
    auto release = [&](const QString& id, const QString& country,
                       bool barcode, bool classical,
                       const QString& firstDate, const QString& status) {
        QJsonObject o;
        o.insert(QStringLiteral("id"),     id);
        o.insert(QStringLiteral("status"), status);
        o.insert(QStringLiteral("title"),  QStringLiteral("Test ") + id);
        o.insert(QStringLiteral("country"), country);
        if (barcode) o.insert(QStringLiteral("barcode"), QStringLiteral("0123456789012"));
        QJsonArray credit;
        if (classical) {
            credit.append(makeArtist(QStringLiteral("Ravel"), QStringLiteral("Person"),
                                     QStringLiteral("ravel-id")));
            credit.append(makeArtist(QStringLiteral("Orchestra"), QStringLiteral("Orchestra"),
                                     QStringLiteral("orch-id")));
        } else {
            credit.append(makeArtist(QStringLiteral("Pop Artist"), QStringLiteral("Person"),
                                     QStringLiteral("pop-id")));
        }
        o.insert(QStringLiteral("artist-credit"), credit);
        QJsonObject rg;
        rg.insert(QStringLiteral("primary-type"), QStringLiteral("Album"));
        rg.insert(QStringLiteral("first-release-date"), firstDate);
        o.insert(QStringLiteral("release-group"), rg);
        QJsonObject medium;
        medium.insert(QStringLiteral("position"), 1);
        medium.insert(QStringLiteral("track-count"), 13);
        o.insert(QStringLiteral("media"), QJsonArray{medium});
        return o;
    };

    QJsonArray candidates;
    candidates.append(release(QStringLiteral("aaa-clean"),
        QStringLiteral("GB"), true, true,
        QStringLiteral("2012-08-20"), QStringLiteral("Official")));
    candidates.append(release(QStringLiteral("bbb-bootleg"),
        QStringLiteral("XW"), false, true,
        QStringLiteral("2012-08-20"), QStringLiteral("Bootleg")));
    candidates.append(release(QStringLiteral("ccc-thin"),
        QStringLiteral("XX"), false, false,
        QStringLiteral("2020-01-01"), QStringLiteral("Official")));

    concerto::metadata::scoring::TocSummary toc;
    toc.discId      = QStringLiteral("synthetic");
    toc.trackCount  = 13;

    const auto pick = concerto::metadata::scoring::pick(candidates, toc);
    out << "self-test scoring rows:\n";
    for (const QVariant& v : pick.scoringLog) {
        const QVariantMap m = v.toMap();
        out << QStringLiteral("  %1  score=%2  %3\n")
                .arg(m.value(QStringLiteral("releaseId")).toString())
                .arg(m.value(QStringLiteral("score")).toInt())
                .arg(QString::fromUtf8(QJsonDocument::fromVariant(
                        m.value(QStringLiteral("components"))).toJson(
                            QJsonDocument::Compact)));
    }
    out << "winner: " << pick.releaseId << "\n";
    return pick.releaseId == QLatin1String("aaa-clean") ? 0 : 1;
}

int run(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    // Cache modules key their on-disk path off applicationName() (via
    // concerto::paths::appDataDir). The GUI app sets this in main.cpp; the
    // CLIs need their own copy so they hit the same cache.
    QCoreApplication::setApplicationName("Concerto");
    QCoreApplication::setOrganizationName("Thompson");

    // Idempotent rename of any legacy PLYR-Qt app-data dir to the
    // current name. Same call site the GUI app uses.
    concerto::paths::migrateAppData();

    const QStringList args = app.arguments();
    if (args.size() < 2) {
        err << "usage:\n"
            << "  mbquery_cli <disc-folder> [--tag-track-1 <out.flac>]\n"
            << "  mbquery_cli --disc-id <mb-disc-id> [--track-count <N>]\n"
            << "  mbquery_cli --self-test\n";
        return 2;
    }

    if (args[1] == QStringLiteral("--self-test")) {
        return runSelfTest();
    }

    if (args[1] == QStringLiteral("--disc-id")) {
        if (args.size() < 3) {
            err << "  --disc-id requires a disc-id argument\n";
            return 2;
        }
        const QString discId = args[2];
        int trackCount = 0;
        std::vector<int> lengthsSec;
        for (int i = 3; i < args.size(); ++i) {
            if (args[i] == QStringLiteral("--track-count") && i + 1 < args.size()) {
                trackCount = args[++i].toInt();
            } else if (args[i] == QStringLiteral("--lengths") && i + 1 < args.size()) {
                for (const QString& s : args[++i].split(QLatin1Char(','),
                                                       Qt::SkipEmptyParts)) {
                    lengthsSec.push_back(s.toInt());
                }
            }
        }
        return runDiscIdMode(discId, trackCount, lengthsSec);
    }

    return runFolderMode(args);
}

} // namespace

int main(int argc, char* argv[]) {
    return run(argc, argv);
}
