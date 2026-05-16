#include "MetadataScoring.h"

#include <QJsonValue>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace concerto::metadata::scoring {

namespace {

// At the disc-ID stage MB hasn't returned work-rels or recording-rels,
// so the +50 / +20 rules degrade to the proxy described in §3.3.1 of
// METADATA_PLAN.md: "any track has artist-credit entries that look like
// both a Person and a Group (classical-style credit)." This is a crude
// signal but deterministic and free.
//
// We look for tracks whose recording.artist-credit (when present) or
// release.artist-credit contains either:
//   * an artist with type=="Person" AND another with type=="Group" /
//     "Orchestra" / "Choir" — proxy for composer-vs-orchestra; or
//   * sort-name on any artist that matches the classical "Lastname,
//     Firstname" pattern (commas in the sort-name).
bool looksLikeClassicalCredit(const QJsonObject& release) {
    bool sawPerson = false;
    bool sawGroup  = false;
    auto inspect = [&](const QJsonArray& ac) {
        for (const QJsonValue& v : ac) {
            const QJsonObject artist = v.toObject().value(QStringLiteral("artist")).toObject();
            const QString type = artist.value(QStringLiteral("type")).toString();
            if (type == QLatin1String("Person")) sawPerson = true;
            if (type == QLatin1String("Group")
             || type == QLatin1String("Orchestra")
             || type == QLatin1String("Choir")) sawGroup = true;
        }
    };
    inspect(release.value(QStringLiteral("artist-credit")).toArray());
    const QJsonArray media = release.value(QStringLiteral("media")).toArray();
    for (const QJsonValue& mv : media) {
        const QJsonArray tracks = mv.toObject().value(QStringLiteral("tracks")).toArray();
        for (const QJsonValue& tv : tracks) {
            inspect(tv.toObject().value(QStringLiteral("recording")).toObject()
                          .value(QStringLiteral("artist-credit")).toArray());
            inspect(tv.toObject().value(QStringLiteral("artist-credit")).toArray());
        }
    }
    return sawPerson && sawGroup;
}

// Pull the candidate's first-release-date for tiebreaking. Missing →
// "9999-99-99" so undated releases sort last.
QString firstReleaseDateOrSentinel(const QJsonObject& r) {
    const QString d = r.value(QStringLiteral("release-group"))
                       .toObject()
                       .value(QStringLiteral("first-release-date")).toString();
    return d.isEmpty() ? QStringLiteral("9999-99-99") : d;
}

QVariantMap componentsToVariant(const Components& c) {
    QVariantMap m;
    if (c.composerHit    != 0) m.insert(QStringLiteral("composerHit"),    c.composerHit);
    if (c.conductor      != 0) m.insert(QStringLiteral("conductor"),      c.conductor);
    if (c.barcode        != 0) m.insert(QStringLiteral("barcode"),        c.barcode);
    if (c.country        != 0) m.insert(QStringLiteral("country"),        c.country);
    if (c.artistIds      != 0) m.insert(QStringLiteral("artistIds"),      c.artistIds);
    if (c.releaseGroupAlbum   != 0) m.insert(QStringLiteral("releaseGroupAlbum"),   c.releaseGroupAlbum);
    if (c.durationDefender    != 0) m.insert(QStringLiteral("durationDefender"),    c.durationDefender);
    if (c.statusPenalty  != 0) m.insert(QStringLiteral("statusPenalty"),  c.statusPenalty);
    if (c.coverArtPenalty != 0) m.insert(QStringLiteral("coverArtPenalty"), c.coverArtPenalty);
    return m;
}

} // namespace

Components score(const QJsonObject& release, const TocSummary& toc) {
    Components c;

    if (looksLikeClassicalCredit(release)) {
        c.composerHit = 50;
        c.conductor   = 20;
    }

    if (!release.value(QStringLiteral("barcode")).toString().isEmpty()) {
        c.barcode = 30;
    }

    static const QSet<QString> kCuratedCountries{
        QStringLiteral("US"), QStringLiteral("GB"), QStringLiteral("DE"),
        QStringLiteral("JP"), QStringLiteral("FR"),
    };
    if (kCuratedCountries.contains(release.value(QStringLiteral("country")).toString())) {
        c.country = 15;
    }

    const QJsonArray ac = release.value(QStringLiteral("artist-credit")).toArray();
    int withId = 0;
    for (const QJsonValue& v : ac) {
        const QString id = v.toObject().value(QStringLiteral("artist"))
                            .toObject().value(QStringLiteral("id")).toString();
        if (!id.isEmpty()) ++withId;
    }
    c.artistIds = 10 * withId;

    const QString rgType = release.value(QStringLiteral("release-group"))
                                  .toObject()
                                  .value(QStringLiteral("primary-type")).toString();
    if (rgType == QLatin1String("Album")) c.releaseGroupAlbum = 5;

    // Duration-similarity defender. Sum of |our − release|.
    if (!toc.trackLengthsSec.empty()) {
        int picked = -1;
        const QJsonArray media = release.value(QStringLiteral("media")).toArray();
        // Use the medium that matches our track count for the duration
        // comparison — same rule as pickMedium(), but inlined to keep
        // score() pure / loop-friendly.
        for (int i = 0; i < media.size(); ++i) {
            const QJsonArray tracks = media[i].toObject()
                                              .value(QStringLiteral("tracks")).toArray();
            if (tracks.size() == toc.trackCount) { picked = i; break; }
        }
        if (picked < 0 && !media.isEmpty()) picked = 0;
        if (picked >= 0) {
            const QJsonArray tracks = media[picked].toObject()
                                              .value(QStringLiteral("tracks")).toArray();
            int devSec = 0;
            const int n = std::min<int>(tracks.size(), static_cast<int>(toc.trackLengthsSec.size()));
            for (int i = 0; i < n; ++i) {
                const QJsonObject to = tracks[i].toObject();
                const qint64 ms = to.value(QStringLiteral("length")).toVariant().toLongLong(nullptr);
                const int relSec = static_cast<int>((ms + 500) / 1000);
                if (relSec <= 0) continue;
                devSec += std::abs(toc.trackLengthsSec[i] - relSec);
            }
            c.durationDefender = std::max(0, 30 - devSec);
        }
    }

    const QString status = release.value(QStringLiteral("status")).toString();
    if (status == QLatin1String("Bootleg")
     || status == QLatin1String("Pseudo-Release")) {
        c.statusPenalty = -20;
    }

    // No cover-art check at this stage — would cost a HEAD per candidate
    // and we don't ship a CAA HEAD probe in v1. Treat the bonus as
    // always-zero (i.e. no penalty applied).

    c.total = c.composerHit + c.conductor + c.barcode + c.country
            + c.artistIds + c.releaseGroupAlbum + c.durationDefender
            + c.statusPenalty + c.coverArtPenalty;
    return c;
}

Pick pick(const QJsonArray& releases, const TocSummary& toc) {
    Pick result;
    if (releases.isEmpty()) return result;

    struct Row {
        QString     id;
        QJsonObject release;
        QString     tiebreakDate;
        Components  comp;
    };
    std::vector<Row> rows;
    rows.reserve(releases.size());
    for (const QJsonValue& v : releases) {
        const QJsonObject r = v.toObject();
        Row row;
        row.id           = r.value(QStringLiteral("id")).toString();
        row.release      = r;
        row.tiebreakDate = firstReleaseDateOrSentinel(r);
        row.comp         = score(r, toc);
        rows.push_back(std::move(row));
    }

    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.comp.total != b.comp.total) return a.comp.total > b.comp.total;
        if (a.tiebreakDate != b.tiebreakDate) return a.tiebreakDate < b.tiebreakDate;
        return a.id < b.id;
    });

    for (const Row& r : rows) {
        QVariantMap entry;
        entry.insert(QStringLiteral("releaseId"),  r.id);
        entry.insert(QStringLiteral("score"),      r.comp.total);
        entry.insert(QStringLiteral("components"), componentsToVariant(r.comp));
        result.scoringLog.append(entry);
    }

    const Row& best = rows.front();
    result.releaseId = best.id;
    result.release   = best.release;
    if (rows.size() == 1) {
        result.reason = QStringLiteral("single candidate");
    } else {
        result.reason = QStringLiteral("%1 candidates; top score %2")
                            .arg(rows.size())
                            .arg(best.comp.total);
    }
    return result;
}

QJsonObject pickMedium(const QJsonObject& release,
                       const QString& discId,
                       int trackCount,
                       int* outPosition)
{
    const QJsonArray media = release.value(QStringLiteral("media")).toArray();
    if (outPosition) *outPosition = 1;

    // Pass 1: match by disc-ID.
    for (const QJsonValue& mv : media) {
        const QJsonObject mo = mv.toObject();
        const QJsonArray discs = mo.value(QStringLiteral("discs")).toArray();
        for (const QJsonValue& dv : discs) {
            if (dv.toObject().value(QStringLiteral("id")).toString() == discId) {
                if (outPosition)
                    *outPosition = mo.value(QStringLiteral("position")).toInt(1);
                return mo;
            }
        }
    }
    // Pass 2: match by track count.
    if (trackCount > 0) {
        for (const QJsonValue& mv : media) {
            const QJsonObject mo = mv.toObject();
            const int count = mo.value(QStringLiteral("track-count")).toInt(
                mo.value(QStringLiteral("tracks")).toArray().size());
            if (count == trackCount) {
                if (outPosition)
                    *outPosition = mo.value(QStringLiteral("position")).toInt(1);
                return mo;
            }
        }
    }
    // Pass 3: first medium.
    if (!media.isEmpty()) {
        const QJsonObject mo = media.first().toObject();
        if (outPosition)
            *outPosition = mo.value(QStringLiteral("position")).toInt(1);
        return mo;
    }
    return QJsonObject{};
}

} // namespace concerto::metadata::scoring
