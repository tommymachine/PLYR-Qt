#include "FlacTags.h"

#include <QDateTime>
#include <QFile>

namespace {

// Little-endian uint32 reader for Vorbis comments.
inline bool readU32LE(const QByteArray& b, int pos, quint32& out) {
    if (pos + 4 > b.size()) return false;
    const auto* u = reinterpret_cast<const unsigned char*>(b.constData());
    out = quint32(u[pos])
        | (quint32(u[pos + 1]) <<  8)
        | (quint32(u[pos + 2]) << 16)
        | (quint32(u[pos + 3]) << 24);
    return true;
}

} // namespace


std::optional<FlacTags> FlacTags::read(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return std::nullopt;

    // Magic: "fLaC"
    const auto magic = f.read(4);
    if (magic.size() != 4
        || static_cast<unsigned char>(magic[0]) != 0x66
        || static_cast<unsigned char>(magic[1]) != 0x4C
        || static_cast<unsigned char>(magic[2]) != 0x61
        || static_cast<unsigned char>(magic[3]) != 0x43) {
        return std::nullopt;
    }

    FlacTags out;

    // Walk metadata blocks until the last-flag is set.
    while (true) {
        const auto hdr = f.read(4);
        if (hdr.size() != 4) break;

        const auto bytes = reinterpret_cast<const unsigned char*>(hdr.constData());
        const bool isLast = (bytes[0] & 0x80) != 0;
        const int  type   = bytes[0] & 0x7F;
        const int  length = (int(bytes[1]) << 16) | (int(bytes[2]) << 8) | int(bytes[3]);

        if (type == 4) {            // VORBIS_COMMENT
            const auto block = f.read(length);
            if (block.size() == length) parseVorbisComment(block, out);
        } else if (type == 0) {     // STREAMINFO
            const auto block = f.read(length);
            if (block.size() == length) parseStreamInfo(block, out);
        } else {
            // Skip unknown blocks (PADDING, SEEKTABLE, PICTURE, etc.)
            f.seek(f.pos() + length);
        }

        if (isLast) break;
    }
    return out;
}


void FlacTags::parseStreamInfo(const QByteArray& data, FlacTags& out)
{
    if (data.size() < 18) return;
    const auto* b = reinterpret_cast<const unsigned char*>(data.constData());

    // 20 bits of sample rate starting at byte 10 (big-endian bitstream).
    const int sampleRate =
        (int(b[10]) << 12) | (int(b[11]) << 4) | (int(b[12]) >> 4);
    if (sampleRate <= 0) return;

    // 36 bits of total-sample count: low 4 bits of b[13] then b[14..17].
    const quint64 totalSamples =
          (quint64(b[13] & 0x0F) << 32)
        | (quint64(b[14])        << 24)
        | (quint64(b[15])        << 16)
        | (quint64(b[16])        <<  8)
        |  quint64(b[17]);

    out.duration = double(totalSamples) / double(sampleRate);
}


void FlacTags::parseVorbisComment(const QByteArray& data, FlacTags& out)
{
    int pos = 0;

    // Vendor string — skip.
    quint32 vendorLen = 0;
    if (!readU32LE(data, pos, vendorLen)) return;
    pos += 4;
    pos += int(vendorLen);

    // Number of user comments.
    quint32 count = 0;
    if (!readU32LE(data, pos, count)) return;
    pos += 4;

    for (quint32 i = 0; i < count; ++i) {
        quint32 len = 0;
        if (!readU32LE(data, pos, len)) return;
        pos += 4;
        const int end = pos + int(len);
        if (end > data.size()) return;

        const QString raw = QString::fromUtf8(data.constData() + pos, int(len));
        pos = end;

        const int eq = raw.indexOf(QLatin1Char('='));
        if (eq <= 0) continue;

        out.tags.insert(raw.left(eq).toUpper(), raw.mid(eq + 1));
    }
}


namespace concerto::metadata {

std::vector<flacencode::VorbisTag> buildVorbisTags(
    const AlbumMeta& a, int i)
{
    std::vector<flacencode::VorbisTag> out;
    auto add = [&](const char* field, const QString& value) {
        if (!value.isEmpty())
            out.emplace_back(field, value.toStdString());
    };
    auto addStd = [&](const char* field, const std::string& value) {
        if (!value.empty()) out.emplace_back(field, value);
    };

    // Album-level fields.
    add("ALBUM",            a.title);
    add("ALBUMARTIST",      a.albumArtist.isEmpty() ? a.artistCredit : a.albumArtist);
    add("ALBUMARTISTSORT",  QString());  // not yet plumbed from MB
    add("ARTIST",           a.artistCredit);
    add("DATE",             a.date);
    if (!a.originalDate.isEmpty() && a.originalDate != a.date)
        add("ORIGINALDATE", a.originalDate);
    add("LABEL",            a.label);
    add("CATALOGNUMBER",    a.catalogNumber);
    add("BARCODE",          a.barcode);
    add("ASIN",             a.asin);
    add("RELEASECOUNTRY",   a.country);
    add("DISCSUBTITLE",     a.discSubtitle);
    if (a.discTotalCount > 0) {
        addStd("DISCNUMBER",  std::to_string(a.discPosition));
        addStd("TOTALDISCS",  std::to_string(a.discTotalCount));
        addStd("DISCTOTAL",   std::to_string(a.discTotalCount));
    }
    addStd("TOTALTRACKS",  std::to_string(a.tracks.size()));
    addStd("TRACKTOTAL",   std::to_string(a.tracks.size()));
    add("MUSICBRAINZ_ALBUMID",        a.releaseId);
    add("MUSICBRAINZ_RELEASEGROUPID", a.releaseGroupId);
    add("MUSICBRAINZ_DISCID",         a.mbDiscId);
    add("MUSICBRAINZ_ALBUMARTISTID",  a.albumArtistId);
    for (const QString& g : a.genreNames) add("GENRE", g);

    // Concerto provenance marker. Future identification passes (the
    // folder-time library pipeline; see docs/LIBRARY_METADATA_PLAN.md
    // §A.0 "Stage Z trust") read these and skip web lookup when the
    // pipeline version is ≥ the consumer's expectation. Harmless to
    // any tool that doesn't recognise it. Always emitted whenever
    // Concerto wrote the tag bundle, regardless of which stage
    // produced the data.
    addStd("CONCERTO_PIPELINE_VERSION", std::string("1"));
    add("CONCERTO_SOURCE",
        a.sourceTag.isEmpty() ? QStringLiteral("unknown") : a.sourceTag);
    add("CONCERTO_ENRICHED_AT",
        QDateTime::currentDateTimeUtc().toString(Qt::ISODate));

    if (i < 0 || i >= a.tracks.size()) return out;
    const TrackMeta& t = a.tracks[i];

    // Per-track fields.
    add("TITLE",                  t.title);
    addStd("TRACKNUMBER",         std::to_string(t.position));
    add("WORK",                   t.workTitle);
    add("MOVEMENTNAME",           t.movementName);
    if (t.movementNumber > 0)
        addStd("MOVEMENT",        std::to_string(t.movementNumber));
    if (t.movementTotal > 0)
        addStd("MOVEMENTTOTAL",   std::to_string(t.movementTotal));
    if (!t.workTitle.isEmpty() || t.movementNumber > 0)
        out.emplace_back("SHOWMOVEMENT", "1");
    add("COMPOSER",               t.composerName);
    add("COMPOSERSORT",           t.composerSort);
    add("ISRC",                   t.isrc);
    add("MUSICBRAINZ_TRACKID",    t.recordingId);
    add("MUSICBRAINZ_WORKID",     t.workId);
    add("MUSICBRAINZ_ARTISTID",   t.composerId);

    for (const Performer& p : t.performers) {
        if (p.role == QLatin1String("conductor")) {
            add("CONDUCTOR", p.name);
            continue;
        }
        // Picard convention: "Artist (instrument/role)".
        QString label = p.name;
        if (!p.attrs.isEmpty()) {
            label += QStringLiteral(" (") + p.attrs.join(QStringLiteral(", "))
                  + QLatin1Char(')');
        } else if (p.role == QLatin1String("performing orchestra")) {
            label += QStringLiteral(" (performing orchestra)");
        } else if (p.role == QLatin1String("chorus master")) {
            label += QStringLiteral(" (chorus master)");
        } else if (p.role == QLatin1String("arranger")) {
            label += QStringLiteral(" (arranger)");
        }
        add("PERFORMER", label);
    }
    return out;
}

} // namespace concerto::metadata
