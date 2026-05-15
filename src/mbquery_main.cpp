// MusicBrainz lookup + tag-encoding demo for a folder of CD-rip FLACs.
//
//   mbquery_cli <disc-folder> [--tag-track-1 <out.flac>]
//
// Reconstructs the disc TOC from each track's STREAMINFO, computes the
// MusicBrainz disc ID via the arverify core, hits MB's web service, and
// prints the resulting release(s) with their per-track titles.
//
// With `--tag-track-1 <path>`: re-encodes track 1 to <path> using tags
// derived from the first matched release, then reads those tags back
// from the encoded file to confirm round-trip correctness — proves the
// rip-time encoder + tagger pipeline works end-to-end before we have a
// real CD-reader to feed it.

#include "ArVerify.h"
#include "FlacDecode.h"
#include "FlacEncode.h"
#include "MusicBrainz.h"

#include <QCoreApplication>
#include <QDir>
#include <QNetworkAccessManager>
#include <QString>
#include <QStringList>
#include <QTextStream>

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

// Derive Vorbis tags for one track from a chosen MusicBrainz release and
// the disc-level identifiers we already computed. Same fields rip_cd.sh
// would write: TITLE, ARTIST, ALBUM, ALBUMARTIST, DATE, TRACKNUMBER,
// TRACKTOTAL, DISCNUMBER/DISCTOTAL (multi-disc only), MUSICBRAINZ_*.
std::vector<flacencode::VorbisTag> tagsForTrack(
    const musicbrainz::Release& rel,
    int trackIndex0Based,
    const std::string& mbDiscId)
{
    std::vector<flacencode::VorbisTag> tags;
    auto add = [&](const char* field, const std::string& value) {
        if (!value.empty())
            tags.emplace_back(field, value);
    };

    const auto& tracks = rel.disc.tracks;
    if (trackIndex0Based >= 0
        && trackIndex0Based < static_cast<int>(tracks.size())) {
        add("TITLE", tracks[trackIndex0Based].title);
        if (!tracks[trackIndex0Based].recordingId.empty())
            add("MUSICBRAINZ_TRACKID", tracks[trackIndex0Based].recordingId);
    }
    add("ARTIST", rel.artist);          // per-track artist isn't fetched yet
    add("ALBUMARTIST", rel.artist);
    add("ALBUM", rel.title);
    add("DATE", rel.date);
    tags.emplace_back("TRACKNUMBER",
                      std::to_string(trackIndex0Based + 1));
    tags.emplace_back("TRACKTOTAL", std::to_string(tracks.size()));
    if (rel.disc.totalCount > 1) {
        tags.emplace_back("DISCNUMBER", std::to_string(rel.disc.position));
        tags.emplace_back("DISCTOTAL", std::to_string(rel.disc.totalCount));
    }
    add("MUSICBRAINZ_DISCID", mbDiscId);
    add("MUSICBRAINZ_ALBUMID", rel.id);
    return tags;
}

int run(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() < 2) {
        err << "usage: mbquery_cli <disc-folder> [--tag-track-1 <out.flac>]\n";
        return 2;
    }
    const QString folder = args[1];

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

} // namespace

int main(int argc, char* argv[]) {
    return run(argc, argv);
}
