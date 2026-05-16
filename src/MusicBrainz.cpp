#include "MusicBrainz.h"

#include <QByteArray>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QString>
#include <QUrl>

namespace musicbrainz {

namespace {

// Synchronous HTTP GET via the caller's QNetworkAccessManager. Returns the
// body on HTTP 200; empty QByteArray on any other status or transport
// failure. Callers don't need to distinguish — both mean "no result".
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

// MB's `artist-credit` is an array of `{name, joinphrase}` objects; the
// "phrase" is just those concatenated. `musicbrainzngs` (the Python lib)
// pre-computes this as `artist-credit-phrase`; the raw web service does not.
std::string joinArtistCredit(const QJsonArray& credits) {
    std::string out;
    for (const QJsonValue& v : credits) {
        const QJsonObject co = v.toObject();
        out += co.value("name").toString().toStdString();
        out += co.value("joinphrase").toString().toStdString();
    }
    return out;
}

// Extract the per-track list from one medium's `tracks` array.
std::vector<Track> extractTracks(const QJsonArray& tracksJson) {
    std::vector<Track> tracks;
    tracks.reserve(static_cast<size_t>(tracksJson.size()));
    for (const QJsonValue& tv : tracksJson) {
        const QJsonObject to = tv.toObject();
        Track t;
        t.position = to.value("position").toInt();
        // Title prefers the recording title (the actual work being performed)
        // and falls back to the track title (which may be a different name
        // when a track is a "subtrack" of a longer work).
        const QJsonObject recording = to.value("recording").toObject();
        t.title = recording.value("title").toString().toStdString();
        if (t.title.empty())
            t.title = to.value("title").toString().toStdString();
        // `length` in MB JSON is in milliseconds, as int. Older entries may
        // have a null length; QJsonValue::toInt returns 0 for that, which
        // we leave as "unknown".
        t.durationMs = recording.value("length").toInt(
            to.value("length").toInt(0));
        t.recordingId = recording.value("id").toString().toStdString();
        tracks.push_back(std::move(t));
    }
    return tracks;
}

// Pick the medium in a release that matches our query. Precedence:
//   1. The medium whose `discs[].id` contains our queried disc ID.
//   2. The first medium whose track count matches `trackCount` (if > 0).
//   3. The first medium of the release.
// Returns the medium's JSON object and writes the disc position into
// `*outPosition` (1-based). Same precedence rip_cd.sh uses.
QJsonObject pickMedium(const QJsonArray& mediaJson,
                       const std::string& discId, int trackCount,
                       int* outPosition)
{
    // Pass 1: match by disc ID.
    for (const QJsonValue& mv : mediaJson) {
        const QJsonObject mo = mv.toObject();
        const QJsonArray discs = mo.value("discs").toArray();
        for (const QJsonValue& dv : discs) {
            if (dv.toObject().value("id").toString().toStdString() == discId) {
                *outPosition = mo.value("position").toInt(1);
                return mo;
            }
        }
    }
    // Pass 2: match by track count.
    if (trackCount > 0) {
        for (const QJsonValue& mv : mediaJson) {
            const QJsonObject mo = mv.toObject();
            const int count = mo.value("track-count").toInt(
                mo.value("tracks").toArray().size());
            if (count == trackCount) {
                *outPosition = mo.value("position").toInt(1);
                return mo;
            }
        }
    }
    // Pass 3: first medium.
    if (!mediaJson.isEmpty()) {
        const QJsonObject mo = mediaJson.first().toObject();
        *outPosition = mo.value("position").toInt(1);
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
    // `inc=release-groups` brings the release-group object onto each
    // release — its `id` is what multi-disc batches key on (same across
    // every pressing of the same box-set release group).
    const QUrl url(QStringLiteral(
            "https://musicbrainz.org/ws/2/discid/%1?fmt=json&inc=artist-credits+recordings+release-groups")
        .arg(QString::fromStdString(discId)));
    const QByteArray body = httpGet(nam, url, QString::fromStdString(userAgent));
    if (body.isEmpty())
        return {};

    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (!doc.isObject())
        return {};

    // The response wraps releases inside the top-level object directly when
    // the disc ID resolved. A "not found" reply has a top-level `error`.
    const QJsonObject root = doc.object();
    const QJsonArray releasesJson = root.value("releases").toArray();
    if (releasesJson.isEmpty())
        return {};

    std::vector<Release> result;
    result.reserve(static_cast<size_t>(releasesJson.size()));
    for (const QJsonValue& rv : releasesJson) {
        const QJsonObject ro = rv.toObject();
        Release rel;
        rel.id      = ro.value("id").toString().toStdString();
        rel.releaseGroupId =
            ro.value("release-group").toObject().value("id").toString().toStdString();
        rel.title   = ro.value("title").toString().toStdString();
        rel.date    = ro.value("date").toString().toStdString();
        rel.country = ro.value("country").toString().toStdString();
        rel.artist  = joinArtistCredit(ro.value("artist-credit").toArray());

        const QJsonArray mediaJson = ro.value("media").toArray();
        int pos = 1;
        const QJsonObject medium = pickMedium(mediaJson, discId, trackCount, &pos);
        rel.disc.position = pos;
        rel.disc.totalCount = mediaJson.size() > 0 ? mediaJson.size() : 1;
        rel.disc.tracks = extractTracks(medium.value("tracks").toArray());

        result.push_back(std::move(rel));
    }
    return result;
}

} // namespace musicbrainz
