#include "RipBatchStore.h"

#include "SystemPaths.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUuid>

namespace concerto::cd {

namespace {

QString isoOrEmpty(const QDateTime& t) {
    return t.isValid() ? t.toString(Qt::ISODate) : QString();
}

QJsonObject discToJson(const RipBatchDisc& d) {
    QJsonObject o;
    o.insert(QStringLiteral("position"), d.position);
    if (!d.mbDiscId.isEmpty()) o.insert(QStringLiteral("mb_disc_id"), d.mbDiscId);
    o.insert(QStringLiteral("status"), d.status);
    if (!d.savedPath.isEmpty()) o.insert(QStringLiteral("saved_path"), d.savedPath);
    if (!d.tempDir.isEmpty())   o.insert(QStringLiteral("temp_dir"),   d.tempDir);
    return o;
}

RipBatchDisc discFromJson(const QJsonObject& o) {
    RipBatchDisc d;
    d.position  = o.value(QStringLiteral("position")).toInt();
    d.mbDiscId  = o.value(QStringLiteral("mb_disc_id")).toString();
    d.status    = o.value(QStringLiteral("status")).toString(QStringLiteral("pending"));
    d.savedPath = o.value(QStringLiteral("saved_path")).toString();
    d.tempDir   = o.value(QStringLiteral("temp_dir")).toString();
    return d;
}

QJsonObject batchToJson(const RipBatch& b) {
    QJsonObject o;
    o.insert(QStringLiteral("batch_id"),            b.id);
    o.insert(QStringLiteral("album_title"),         b.albumTitle);
    o.insert(QStringLiteral("artist"),              b.artist);
    o.insert(QStringLiteral("mb_release_group_id"), b.releaseGroupId);
    o.insert(QStringLiteral("total_discs"),         b.totalDiscs);
    if (!b.parentFolder.isEmpty())
        o.insert(QStringLiteral("parent_folder"),   b.parentFolder);

    QJsonArray discs;
    for (const auto& d : b.discs) discs.append(discToJson(d));
    o.insert(QStringLiteral("discs"), discs);

    const QString ca = isoOrEmpty(b.createdAt);
    const QString ua = isoOrEmpty(b.updatedAt);
    if (!ca.isEmpty()) o.insert(QStringLiteral("created_at"), ca);
    if (!ua.isEmpty()) o.insert(QStringLiteral("updated_at"), ua);
    return o;
}

std::optional<RipBatch> batchFromJson(const QJsonDocument& doc) {
    if (!doc.isObject()) return std::nullopt;
    const QJsonObject o = doc.object();
    RipBatch b;
    b.id              = o.value(QStringLiteral("batch_id")).toString();
    b.albumTitle      = o.value(QStringLiteral("album_title")).toString();
    b.artist          = o.value(QStringLiteral("artist")).toString();
    b.releaseGroupId  = o.value(QStringLiteral("mb_release_group_id")).toString();
    b.totalDiscs      = o.value(QStringLiteral("total_discs")).toInt();
    b.parentFolder    = o.value(QStringLiteral("parent_folder")).toString();
    b.createdAt       = QDateTime::fromString(
        o.value(QStringLiteral("created_at")).toString(), Qt::ISODate);
    b.updatedAt       = QDateTime::fromString(
        o.value(QStringLiteral("updated_at")).toString(), Qt::ISODate);

    const QJsonArray discs = o.value(QStringLiteral("discs")).toArray();
    b.discs.reserve(discs.size());
    for (const auto& v : discs) b.discs.append(discFromJson(v.toObject()));
    if (b.id.isEmpty()) return std::nullopt;
    return b;
}

QString discBatchPath(const QString& root, const QString& batchId) {
    return root + QStringLiteral("/") + batchId + QStringLiteral(".json");
}

} // namespace

QString RipBatchStore::defaultRoot() {
    // Routed through concerto::paths::appDataDir() so the app's on-disk
    // root is named exactly once (in main.cpp's setApplicationName).
    // On macOS this resolves to
    //   ~/Library/Application Support/<applicationName>/rip_batches
    // matching the rip-in-progress temp dir, the metadata cache, and
    // the pending-submissions log.
    return concerto::paths::appDataDir() + QStringLiteral("/rip_batches");
}

bool RipBatchStore::save(const RipBatch& batch, const QString& root) {
    if (batch.id.isEmpty()) return false;
    QDir().mkpath(root);

    RipBatch b = batch;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (!b.createdAt.isValid()) b.createdAt = now;
    b.updatedAt = now;

    QSaveFile f(discBatchPath(root, b.id));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const QJsonDocument doc(batchToJson(b));
    f.write(doc.toJson(QJsonDocument::Indented));
    return f.commit();
}

std::optional<RipBatch> RipBatchStore::lookupByReleaseGroup(
    const QString& releaseGroupId, const QString& root)
{
    if (releaseGroupId.isEmpty()) return std::nullopt;
    QDir dir(root);
    if (!dir.exists()) return std::nullopt;

    std::optional<RipBatch> best;
    QDateTime bestUpdated;
    for (const auto& name :
         dir.entryList(QStringList{QStringLiteral("*.json")}, QDir::Files)) {
        QFile f(dir.absoluteFilePath(name));
        if (!f.open(QIODevice::ReadOnly)) continue;
        const auto b = batchFromJson(QJsonDocument::fromJson(f.readAll()));
        if (!b) continue;
        if (b->releaseGroupId != releaseGroupId) continue;
        if (!best || b->updatedAt > bestUpdated) {
            best = b;
            bestUpdated = b->updatedAt;
        }
    }
    return best;
}

std::optional<RipBatch> RipBatchStore::lookupByDiscId(
    const QString& mbDiscId, const QString& root)
{
    if (mbDiscId.isEmpty()) return std::nullopt;
    QDir dir(root);
    if (!dir.exists()) return std::nullopt;

    std::optional<RipBatch> best;
    QDateTime bestUpdated;
    for (const auto& name :
         dir.entryList(QStringList{QStringLiteral("*.json")}, QDir::Files)) {
        QFile f(dir.absoluteFilePath(name));
        if (!f.open(QIODevice::ReadOnly)) continue;
        const auto b = batchFromJson(QJsonDocument::fromJson(f.readAll()));
        if (!b) continue;
        bool match = false;
        for (const auto& d : b->discs) {
            if (!d.mbDiscId.isEmpty() && d.mbDiscId == mbDiscId) {
                match = true; break;
            }
        }
        if (!match) continue;
        if (!best || b->updatedAt > bestUpdated) {
            best = b;
            bestUpdated = b->updatedAt;
        }
    }
    return best;
}

QVector<RipBatch> RipBatchStore::listResumable(const QString& root) {
    QVector<RipBatch> out;
    QDir dir(root);
    if (!dir.exists()) return out;

    for (const auto& name :
         dir.entryList(QStringList{QStringLiteral("*.json")}, QDir::Files)) {
        QFile f(dir.absoluteFilePath(name));
        if (!f.open(QIODevice::ReadOnly)) continue;
        const auto b = batchFromJson(QJsonDocument::fromJson(f.readAll()));
        if (!b) continue;

        // Only include batches with at least one pending disc — "done"
        // and "skipped" both count as resolved.
        bool anyPending = false;
        for (const auto& d : b->discs) {
            if (d.status != QStringLiteral("done")
                && d.status != QStringLiteral("skipped"))
            {
                anyPending = true;
                break;
            }
        }
        if (anyPending) out.append(*b);
    }
    std::sort(out.begin(), out.end(),
              [](const RipBatch& a, const RipBatch& b) {
                  return a.updatedAt > b.updatedAt;
              });
    return out;
}

bool RipBatchStore::remove(const QString& batchId, const QString& root) {
    if (batchId.isEmpty()) return false;
    const QString p = discBatchPath(root, batchId);
    QFile f(p);
    if (!f.exists()) return true;
    return f.remove();
}

QString RipBatchStore::newBatchId() {
    // QUuid v4 in the "no braces, no hyphens" form: 32 hex chars. Avoids
    // filesystem-unfriendly characters and stays under any sane name
    // length limit.
    return QUuid::createUuid().toString(QUuid::WithoutBraces).remove(QChar('-'));
}

} // namespace concerto::cd
