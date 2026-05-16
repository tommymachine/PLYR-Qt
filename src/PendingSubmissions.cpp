#include "PendingSubmissions.h"

#include "SystemPaths.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

namespace concerto::metadata {

QString PendingSubmissions::defaultPath() {
    return concerto::paths::appDataDir()
         + QStringLiteral("/pending-submissions.jsonl");
}

void PendingSubmissions::append(const QString& mbDiscId,
                                const TocRow& toc,
                                const QString& ripDir,
                                const QStringList& stageFailures,
                                const QString& path)
{
    QDir().mkpath(QFileInfo(path).absolutePath());

    QJsonObject tocObj;
    tocObj.insert(QStringLiteral("firstTrack"), toc.firstTrack);
    tocObj.insert(QStringLiteral("lastTrack"),  toc.lastTrack);
    tocObj.insert(QStringLiteral("leadout"),    qint64(toc.leadoutLba));
    QJsonArray offsets;
    for (uint32_t off : toc.offsets) offsets.append(qint64(off));
    tocObj.insert(QStringLiteral("offsets"), offsets);

    QJsonObject row;
    row.insert(QStringLiteral("timestamp"),
               QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    row.insert(QStringLiteral("discId"),  mbDiscId);
    row.insert(QStringLiteral("toc"),     tocObj);
    row.insert(QStringLiteral("ripDir"),  ripDir);
    QJsonArray fails;
    for (const QString& s : stageFailures) fails.append(s);
    row.insert(QStringLiteral("stageFailures"), fails);

    QFile f(path);
    if (!f.open(QIODevice::Append | QIODevice::WriteOnly)) return;
    f.write(QJsonDocument(row).toJson(QJsonDocument::Compact));
    f.write("\n");
}

} // namespace concerto::metadata
