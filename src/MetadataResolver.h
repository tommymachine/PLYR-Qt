// Always-commits metadata pipeline orchestrator.
//
// One async resolve() call per disc. Internally walks
//   Stage 1  — MusicBrainz disc-ID
//   Stage 1b — second-hop release fetch + score-rank-pick on the disc-ID's
//              candidates, deterministic across machines
//   Stage 2  — CD-TEXT (scaffold; nullopt today)
//   Stage 3  — AcoustID (scaffold; nullopt today)
//   Stage 4  — stub builder + pending-submissions.jsonl append
//
// Emits stageChanged(<stage>) for the UI and resolved(album, sourceTag)
// when the chain settles. The pipeline NEVER fails — the worst case is
// a stub AlbumMeta with confidence=0 and sourceTag="stub".
//
// Threading: lives on the rip-worker thread (mirror RipWorker pattern in
// RipWorker.h). Network I/O is fully async via QNetworkAccessManager —
// no nested event loops, deliberately, see METADATA_PLAN.md §6.4.

#pragma once

#include "MetadataCache.h"
#include "MetadataModel.h"
#include "MetadataScoring.h"
#include "MusicBrainz.h"
#include "PendingSubmissions.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QVector>
#include <QElapsedTimer>

#include <memory>

namespace concerto::metadata {

class MetadataResolver : public QObject {
    Q_OBJECT
public:
    explicit MetadataResolver(QObject* parent = nullptr);
    ~MetadataResolver() override;

    void setUserAgent(const QString& ua);
    void setCacheDir(const QString& dir);

    struct Request {
        QString             discId;
        int                 trackCount = 0;
        scoring::TocSummary tocSummary;
        QString             ripDir;       // for the pending-submissions row
        TocRow              tocForSubmissions;
    };

    // Kick off one resolve. Always emits resolved(...) once.
    void resolve(const Request& req);

signals:
    void stageChanged(QString stage);
    void resolved(concerto::metadata::AlbumMeta album, QString sourceTag);

private slots:
    void onDiscIdResolved(QString discId, QJsonArray releases);
    void onDiscIdFailed(QString discId, int httpStatus, QString message);
    void onReleaseResolved(QString releaseMbid, QJsonObject release);
    void onReleaseFailed(QString releaseMbid, int httpStatus, QString message);

private:
    void runStage2();
    void runStage3();
    void runStage4();
    void emitResolved(AlbumMeta album, const QString& sourceTag);
    void logStage(const QString& stage, const QString& outcome);

    Request m_req;
    QElapsedTimer m_stageTimer;
    QStringList m_failedStages;
    QVariantList m_pipelineLog;
    QVariantList m_scoringLog;
    QString m_pickReason;

    QNetworkAccessManager m_nam;
    musicbrainz::Client   m_mb;
    std::unique_ptr<MetadataCache> m_cache;
};

} // namespace concerto::metadata
