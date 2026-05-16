#include "MetadataModel.h"

#include <QDateTime>
#include <QStringBuilder>

namespace concerto::metadata {

QString debugDumpTrack(const AlbumMeta& a, int i) {
    if (i < 0 || i >= a.tracks.size())
        return QStringLiteral("(track %1 out of range)").arg(i);
    const TrackMeta& t = a.tracks[i];

    QString out;
    auto add = [&](const QString& key, const QString& val) {
        if (!val.isEmpty())
            out += key + QLatin1Char('=') + val + QLatin1Char('\n');
    };
    add(QStringLiteral("ALBUM"),          a.title);
    add(QStringLiteral("ALBUMARTIST"),    a.albumArtist);
    add(QStringLiteral("ARTIST"),         a.artistCredit);
    add(QStringLiteral("TITLE"),          t.title);
    add(QStringLiteral("WORK"),           t.workTitle);
    add(QStringLiteral("MOVEMENTNAME"),   t.movementName);
    if (t.movementNumber > 0)
        add(QStringLiteral("MOVEMENT"),       QString::number(t.movementNumber));
    if (t.movementTotal > 0)
        add(QStringLiteral("MOVEMENTTOTAL"),  QString::number(t.movementTotal));
    if (!t.workTitle.isEmpty() || t.movementNumber > 0)
        add(QStringLiteral("SHOWMOVEMENT"), QStringLiteral("1"));
    add(QStringLiteral("COMPOSER"),       t.composerName);
    add(QStringLiteral("COMPOSERSORT"),   t.composerSort);
    for (const Performer& p : t.performers) {
        if (p.role == QLatin1String("conductor")) {
            add(QStringLiteral("CONDUCTOR"), p.name);
        } else {
            QString label = p.name;
            if (!p.attrs.isEmpty())
                label += QStringLiteral(" (") + p.attrs.join(QStringLiteral(", "))
                       + QLatin1Char(')');
            else if (p.role == QLatin1String("performing orchestra"))
                label += QStringLiteral(" (orchestra)");
            add(QStringLiteral("PERFORMER"), label);
        }
    }
    add(QStringLiteral("DATE"),           a.date);
    if (!a.originalDate.isEmpty() && a.originalDate != a.date)
        add(QStringLiteral("ORIGINALDATE"), a.originalDate);
    add(QStringLiteral("LABEL"),          a.label);
    add(QStringLiteral("CATALOGNUMBER"),  a.catalogNumber);
    add(QStringLiteral("BARCODE"),        a.barcode);
    add(QStringLiteral("ASIN"),           a.asin);
    if (a.discTotalCount > 0) {
        add(QStringLiteral("DISCNUMBER"),  QString::number(a.discPosition));
        add(QStringLiteral("TOTALDISCS"),  QString::number(a.discTotalCount));
    }
    add(QStringLiteral("TRACKNUMBER"),    QString::number(t.position));
    add(QStringLiteral("TOTALTRACKS"),    QString::number(a.tracks.size()));
    add(QStringLiteral("MUSICBRAINZ_ALBUMID"),         a.releaseId);
    add(QStringLiteral("MUSICBRAINZ_RELEASEGROUPID"),  a.releaseGroupId);
    add(QStringLiteral("MUSICBRAINZ_DISCID"),          a.mbDiscId);
    add(QStringLiteral("MUSICBRAINZ_TRACKID"),         t.recordingId);
    add(QStringLiteral("MUSICBRAINZ_WORKID"),          t.workId);
    add(QStringLiteral("MUSICBRAINZ_ARTISTID"),        t.composerId);
    add(QStringLiteral("MUSICBRAINZ_ALBUMARTISTID"),   a.albumArtistId);
    add(QStringLiteral("ISRC"),                        t.isrc);
    if (!a.genreNames.isEmpty())
        add(QStringLiteral("GENRE"),  a.genreNames.join(QStringLiteral("; ")));

    // Concerto provenance marker (see FlacTags.cpp buildVorbisTags
    // for the canonical emission point at rip-time). Mirrored here
    // so diagnostic dumps from mbquery_cli surface the same trust
    // tags the rip flow will embed in the FLAC.
    add(QStringLiteral("CONCERTO_PIPELINE_VERSION"), QStringLiteral("1"));
    add(QStringLiteral("CONCERTO_SOURCE"),
        a.sourceTag.isEmpty() ? QStringLiteral("unknown") : a.sourceTag);
    add(QStringLiteral("CONCERTO_ENRICHED_AT"),
        QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    return out;
}

} // namespace concerto::metadata
