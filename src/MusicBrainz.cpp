#include "MusicBrainz.h"

#include "MetadataScoring.h"

#include <QByteArray>
#include <QHash>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QString>
#include <QUrl>

#include <algorithm>

namespace musicbrainz {

namespace {

// One-shot HTTP GET on the caller's QNetworkAccessManager. Synchronous —
// spins a nested event loop on the QNAM. Retained for the legacy
// `lookupByDiscId` shim; new code uses Client (async).
QByteArray httpGet(QNetworkAccessManager& nam, const QUrl& url,
                   const QString& userAgent)
{
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, userAgent);
    QNetworkReply* reply = nam.get(req);

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished,
                     &loop, &QEventLoop::quit);
    loop.exec();

    QByteArray body;
    if (reply->error() == QNetworkReply::NoError
        && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200) {
        body = reply->readAll();
    }
    reply->deleteLater();
    return body;
}

std::string joinArtistCreditStd(const QJsonArray& credits) {
    std::string out;
    for (const QJsonValue& v : credits) {
        const QJsonObject co = v.toObject();
        out += co.value(QStringLiteral("name")).toString().toStdString();
        out += co.value(QStringLiteral("joinphrase")).toString().toStdString();
    }
    return out;
}

QString joinArtistCredit(const QJsonArray& credits) {
    QString out;
    for (const QJsonValue& v : credits) {
        const QJsonObject co = v.toObject();
        out += co.value(QStringLiteral("name")).toString();
        out += co.value(QStringLiteral("joinphrase")).toString();
    }
    return out;
}

std::vector<Track> extractTracks(const QJsonArray& tracksJson) {
    std::vector<Track> tracks;
    tracks.reserve(static_cast<size_t>(tracksJson.size()));
    for (const QJsonValue& tv : tracksJson) {
        const QJsonObject to = tv.toObject();
        Track t;
        t.position = to.value(QStringLiteral("position")).toInt();
        const QJsonObject recording = to.value(QStringLiteral("recording")).toObject();
        t.title = recording.value(QStringLiteral("title")).toString().toStdString();
        if (t.title.empty())
            t.title = to.value(QStringLiteral("title")).toString().toStdString();
        t.durationMs = recording.value(QStringLiteral("length")).toInt(
            to.value(QStringLiteral("length")).toInt(0));
        t.recordingId = recording.value(QStringLiteral("id")).toString().toStdString();
        tracks.push_back(std::move(t));
    }
    return tracks;
}

QJsonObject pickMediumStd(const QJsonArray& mediaJson,
                          const std::string& discId, int trackCount,
                          int* outPosition)
{
    for (const QJsonValue& mv : mediaJson) {
        const QJsonObject mo = mv.toObject();
        const QJsonArray discs = mo.value(QStringLiteral("discs")).toArray();
        for (const QJsonValue& dv : discs) {
            if (dv.toObject().value(QStringLiteral("id")).toString().toStdString() == discId) {
                *outPosition = mo.value(QStringLiteral("position")).toInt(1);
                return mo;
            }
        }
    }
    if (trackCount > 0) {
        for (const QJsonValue& mv : mediaJson) {
            const QJsonObject mo = mv.toObject();
            const int count = mo.value(QStringLiteral("track-count")).toInt(
                mo.value(QStringLiteral("tracks")).toArray().size());
            if (count == trackCount) {
                *outPosition = mo.value(QStringLiteral("position")).toInt(1);
                return mo;
            }
        }
    }
    if (!mediaJson.isEmpty()) {
        const QJsonObject mo = mediaJson.first().toObject();
        *outPosition = mo.value(QStringLiteral("position")).toInt(1);
        return mo;
    }
    *outPosition = 1;
    return QJsonObject{};
}

} // namespace

std::vector<Release> lookupByDiscId(QNetworkAccessManager& nam,
                                    const std::string& discId,
                                    int trackCount,
                                    const std::string& userAgent)
{
    const QUrl url(QStringLiteral(
            "https://musicbrainz.org/ws/2/discid/%1?fmt=json&inc=artist-credits+recordings+release-groups")
        .arg(QString::fromStdString(discId)));
    const QByteArray body = httpGet(nam, url, QString::fromStdString(userAgent));
    if (body.isEmpty()) return {};

    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (!doc.isObject()) return {};

    const QJsonObject root = doc.object();
    const QJsonArray releasesJson = root.value(QStringLiteral("releases")).toArray();
    if (releasesJson.isEmpty()) return {};

    std::vector<Release> result;
    result.reserve(static_cast<size_t>(releasesJson.size()));
    for (const QJsonValue& rv : releasesJson) {
        const QJsonObject ro = rv.toObject();
        Release rel;
        rel.id      = ro.value(QStringLiteral("id")).toString().toStdString();
        rel.releaseGroupId =
            ro.value(QStringLiteral("release-group"))
              .toObject().value(QStringLiteral("id")).toString().toStdString();
        rel.title   = ro.value(QStringLiteral("title")).toString().toStdString();
        rel.date    = ro.value(QStringLiteral("date")).toString().toStdString();
        rel.country = ro.value(QStringLiteral("country")).toString().toStdString();
        rel.artist  = joinArtistCreditStd(ro.value(QStringLiteral("artist-credit")).toArray());

        const QJsonArray mediaJson = ro.value(QStringLiteral("media")).toArray();
        int pos = 1;
        const QJsonObject medium = pickMediumStd(mediaJson, discId, trackCount, &pos);
        rel.disc.position = pos;
        rel.disc.totalCount = mediaJson.size() > 0 ? mediaJson.size() : 1;
        rel.disc.tracks = extractTracks(medium.value(QStringLiteral("tracks")).toArray());

        result.push_back(std::move(rel));
    }
    return result;
}

// ---------------------------------------------------------------------
// Async Client

Client::Client(QNetworkAccessManager* nam, QObject* parent)
    : QObject(parent), m_nam(nam) {}

void Client::fetchDiscId(const QString& discId)
{
    if (!m_nam) {
        emit discIdFailed(discId, 0, QStringLiteral("no QNetworkAccessManager"));
        return;
    }
    const QUrl url(QStringLiteral(
            "https://musicbrainz.org/ws/2/discid/%1?fmt=json&inc=artist-credits+recordings+release-groups")
        .arg(discId));
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, m_userAgent);

    QNetworkReply* reply = m_nam->get(req);
    QPointer<Client> self(this);
    connect(reply, &QNetworkReply::finished, this, [reply, self, discId]() {
        reply->deleteLater();
        if (!self) return;
        const int status = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || status != 200) {
            emit self->discIdFailed(discId, status, reply->errorString());
            return;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isObject()) {
            emit self->discIdFailed(discId, status, QStringLiteral("invalid JSON"));
            return;
        }
        const QJsonArray releases =
            doc.object().value(QStringLiteral("releases")).toArray();
        emit self->discIdResolved(discId, releases);
    });
}

void Client::fetchRelease(const QString& releaseMbid)
{
    if (!m_nam) {
        emit releaseFailed(releaseMbid, 0, QStringLiteral("no QNetworkAccessManager"));
        return;
    }
    const QUrl url(QStringLiteral(
            "https://musicbrainz.org/ws/2/release/%1"
            "?fmt=json&inc=artist-credits+recordings+labels+release-groups+media"
            "+work-rels+recording-level-rels+work-level-rels+artist-rels+isrcs")
        .arg(releaseMbid));
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, m_userAgent);

    QNetworkReply* reply = m_nam->get(req);
    QPointer<Client> self(this);
    connect(reply, &QNetworkReply::finished, this, [reply, self, releaseMbid]() {
        reply->deleteLater();
        if (!self) return;
        const int status = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || status != 200) {
            emit self->releaseFailed(releaseMbid, status, reply->errorString());
            return;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isObject()) {
            emit self->releaseFailed(releaseMbid, status, QStringLiteral("invalid JSON"));
            return;
        }
        emit self->releaseResolved(releaseMbid, doc.object());
    });
}

// ---------------------------------------------------------------------
// Flattening

namespace {

using concerto::metadata::AlbumMeta;
using concerto::metadata::Performer;
using concerto::metadata::TrackMeta;

QString creditedName(const QJsonObject& relation) {
    const QString credited = relation.value(QStringLiteral("target-credit")).toString();
    if (!credited.isEmpty()) return credited;
    return relation.value(QStringLiteral("artist"))
                   .toObject()
                   .value(QStringLiteral("name")).toString();
}

// Heuristic movement extractor (METADATA_PIPELINE_AUTOMATED.md "Movement
// extraction: split track titles on `": "`"). Two passes:
//   1. For each track, find the first `": "` (or ` : ` if that's
//      what MB used). Right-hand side is the movement label; left-hand
//      side is the work-group key used to count siblings.
//   2. After all tracks are seen, stamp MOVEMENTTOTAL onto every track
//      in a group with that group's final count.
//
// `WORK` retains whatever parseRecordingRels stored (the full
// movement-level work.title from MB) — the heuristic only feeds the
// MOVEMENTNAME / MOVEMENT / MOVEMENTTOTAL trio. Single-movement works
// (no colon in the title) get no MOVEMENT/MOVEMENTTOTAL.
void applyMovementHeuristic(QVector<TrackMeta>& tracks) {
    struct Slot { int firstIndex; int count; };
    QHash<QString, Slot> groups;
    std::vector<int> ordered;
    ordered.reserve(tracks.size());

    auto splitOnColon = [](const QString& title) -> std::pair<QString, QString> {
        // Accept either `: ` or ` : ` (MB's style guides drift between
        // them). Whichever appears first wins.
        const int a = title.indexOf(QStringLiteral(": "));
        const int b = title.indexOf(QStringLiteral(" : "));
        if (a < 0 && b < 0) return {QString(), QString()};
        if (b < 0 || (a >= 0 && a < b))
            return {title.left(a).trimmed(), title.mid(a + 2).trimmed()};
        return {title.left(b).trimmed(), title.mid(b + 3).trimmed()};
    };

    for (int i = 0; i < tracks.size(); ++i) {
        const auto [group, movement] = splitOnColon(tracks[i].title);
        if (group.isEmpty() || movement.isEmpty()) continue;

        if (tracks[i].movementName.isEmpty())
            tracks[i].movementName = movement;
        auto it = groups.find(group);
        if (it == groups.end()) {
            it = groups.insert(group, Slot{i, 1});
        } else {
            it->count += 1;
        }
        tracks[i].movementNumber = it->count;
        ordered.push_back(i);
    }
    for (int i : ordered) {
        const QString group = splitOnColon(tracks[i].title).first;
        const auto it = groups.find(group);
        if (it != groups.end()) tracks[i].movementTotal = it->count;
    }
}

QString labelAndCatalog(const QJsonObject& release,
                       QString* outLabel, QString* outCatalog)
{
    const QJsonArray labels = release.value(QStringLiteral("label-info")).toArray();
    if (labels.isEmpty()) return {};
    // Prefer the first entry that has both a label name AND a catalog
    // number; else fall back to whichever fields are present.
    QString fallbackLabel, fallbackCatalog;
    for (const QJsonValue& v : labels) {
        const QJsonObject li = v.toObject();
        const QString cat = li.value(QStringLiteral("catalog-number")).toString();
        const QString lbl = li.value(QStringLiteral("label")).toObject()
                              .value(QStringLiteral("name")).toString();
        if (!cat.isEmpty() && !lbl.isEmpty()) {
            *outLabel = lbl;
            *outCatalog = cat;
            return lbl + QStringLiteral(" / ") + cat;
        }
        if (fallbackLabel.isEmpty() && !lbl.isEmpty()) fallbackLabel = lbl;
        if (fallbackCatalog.isEmpty() && !cat.isEmpty()) fallbackCatalog = cat;
    }
    if (outLabel)   *outLabel   = fallbackLabel;
    if (outCatalog) *outCatalog = fallbackCatalog;
    return fallbackLabel;
}

void parseRecordingRels(const QJsonObject& recording, TrackMeta& out)
{
    const QJsonArray rels = recording.value(QStringLiteral("relations")).toArray();
    for (const QJsonValue& rv : rels) {
        const QJsonObject r = rv.toObject();
        const QString type = r.value(QStringLiteral("type")).toString();
        const QString targetType = r.value(QStringLiteral("target-type")).toString();

        if (type == QLatin1String("performance")
         && targetType == QLatin1String("work")) {
            const QJsonObject work = r.value(QStringLiteral("work")).toObject();
            if (out.workId.isEmpty())
                out.workId = work.value(QStringLiteral("id")).toString();
            if (out.workTitle.isEmpty()) {
                // Store the full work.title — METADATA_PLAN.md §2.4
                // wants `WORK` to be the canonical work label, which for
                // a single-movement is just the work name and for a
                // movement-level entity is "Parent: I. Movement".
                out.workTitle = work.value(QStringLiteral("title")).toString();
            }
            // Walk the work's relations for the composer (and only the
            // composer — lyricist/arranger live on the work too but
            // aren't load-bearing in v1).
            const QJsonArray wrels = work.value(QStringLiteral("relations")).toArray();
            for (const QJsonValue& wrv : wrels) {
                const QJsonObject wr = wrv.toObject();
                if (wr.value(QStringLiteral("type")).toString() != QLatin1String("composer"))
                    continue;
                const QJsonObject artist = wr.value(QStringLiteral("artist")).toObject();
                if (out.composerId.isEmpty())
                    out.composerId   = artist.value(QStringLiteral("id")).toString();
                if (out.composerName.isEmpty())
                    out.composerName = creditedName(wr);
                if (out.composerSort.isEmpty())
                    out.composerSort = artist.value(QStringLiteral("sort-name")).toString();
            }
            continue;
        }

        // Typed performer rels live on the recording itself.
        Performer p;
        p.role = type;
        const QJsonObject artist = r.value(QStringLiteral("artist")).toObject();
        p.name = creditedName(r);
        if (p.name.isEmpty()) continue;
        p.sortName    = artist.value(QStringLiteral("sort-name")).toString();
        p.mbArtistId  = artist.value(QStringLiteral("id")).toString();
        for (const QJsonValue& av : r.value(QStringLiteral("attributes")).toArray())
            p.attrs << av.toString();

        if (type == QLatin1String("conductor")
         || type == QLatin1String("performing orchestra")
         || type == QLatin1String("instrument")
         || type == QLatin1String("vocal")
         || type == QLatin1String("performer")
         || type == QLatin1String("chorus master")
         || type == QLatin1String("arranger"))
        {
            out.performers.append(p);
        }
    }
}

} // namespace

QString primaryArtistName(const QJsonObject& release)
{
    const QJsonArray ac = release.value(QStringLiteral("artist-credit")).toArray();
    if (ac.isEmpty()) return {};
    return ac.first().toObject().value(QStringLiteral("artist"))
                                .toObject().value(QStringLiteral("name")).toString();
}

AlbumMeta flattenRelease(const QJsonObject& release,
                         const QString& discId,
                         int trackCount)
{
    AlbumMeta out;
    out.releaseId       = release.value(QStringLiteral("id")).toString();
    out.releaseGroupId  = release.value(QStringLiteral("release-group"))
                                  .toObject()
                                  .value(QStringLiteral("id")).toString();
    out.title           = release.value(QStringLiteral("title")).toString();
    out.date            = release.value(QStringLiteral("date")).toString();
    out.country         = release.value(QStringLiteral("country")).toString();
    out.barcode         = release.value(QStringLiteral("barcode")).toString();
    out.asin            = release.value(QStringLiteral("asin")).toString();
    out.mbDiscId        = discId;

    out.artistCredit    = joinArtistCredit(release.value(QStringLiteral("artist-credit")).toArray());
    out.albumArtist     = primaryArtistName(release);
    out.albumArtistId   = release.value(QStringLiteral("artist-credit")).toArray().first()
                              .toObject().value(QStringLiteral("artist"))
                              .toObject().value(QStringLiteral("id")).toString();
    QString label, catalog;
    labelAndCatalog(release, &label, &catalog);
    out.label         = label;
    out.catalogNumber = catalog;

    int mediumPosition = 1;
    const QJsonObject medium = concerto::metadata::scoring::pickMedium(
        release, discId, trackCount, &mediumPosition);
    out.discPosition   = mediumPosition;
    const QJsonArray media = release.value(QStringLiteral("media")).toArray();
    out.discTotalCount = media.size() > 0 ? media.size() : 1;
    out.discSubtitle   = medium.value(QStringLiteral("title")).toString();

    const QJsonArray tracksJson = medium.value(QStringLiteral("tracks")).toArray();
    out.tracks.reserve(tracksJson.size());
    for (const QJsonValue& tv : tracksJson) {
        const QJsonObject to = tv.toObject();
        TrackMeta tm;
        tm.position = to.value(QStringLiteral("position")).toInt();
        const QJsonObject recording = to.value(QStringLiteral("recording")).toObject();
        tm.title = recording.value(QStringLiteral("title")).toString();
        if (tm.title.isEmpty())
            tm.title = to.value(QStringLiteral("title")).toString();
        tm.recordingId = recording.value(QStringLiteral("id")).toString();
        const qint64 ms = recording.value(QStringLiteral("length")).toVariant().toLongLong();
        tm.durationMs = ms > 0 ? ms
                              : qint64(to.value(QStringLiteral("length")).toVariant().toLongLong());

        // ISRCs on the recording (from inc=isrcs).
        const QJsonArray isrcs = recording.value(QStringLiteral("isrcs")).toArray();
        if (!isrcs.isEmpty())
            tm.isrc = isrcs.first().toString();

        parseRecordingRels(recording, tm);
        out.tracks.append(tm);
    }

    applyMovementHeuristic(out.tracks);
    return out;
}

} // namespace musicbrainz
