#include "MetadataResolver.h"

#include "AcoustIdProvider.h"
#include "CdTextReader.h"

#include <QVariantMap>

namespace concerto::metadata {

namespace {

void logRow(QVariantList& log, const QString& stage,
            const QString& outcome, qint64 ms)
{
    QVariantMap m;
    m.insert(QStringLiteral("stage"),       stage);
    m.insert(QStringLiteral("outcome"),     outcome);
    m.insert(QStringLiteral("durationMs"),  ms);
    log.append(m);
}

AlbumMeta buildStub(const MetadataResolver::Request& req)
{
    AlbumMeta a;
    a.title         = QStringLiteral("Unknown Album (Disc ID: %1)").arg(req.discId);
    a.albumArtist   = QStringLiteral("Unknown Artist");
    a.artistCredit  = QStringLiteral("Unknown Artist");
    a.mbDiscId      = req.discId;
    a.discPosition  = 1;
    a.discTotalCount = 1;
    a.confidence    = 0;
    a.sourceTag     = QStringLiteral("stub");
    for (int i = 1; i <= req.trackCount; ++i) {
        TrackMeta t;
        t.position = i;
        t.title    = QStringLiteral("Track %1").arg(i, 2, 10, QLatin1Char('0'));
        a.tracks.append(t);
    }
    return a;
}

} // namespace

MetadataResolver::MetadataResolver(QObject* parent)
    : QObject(parent), m_mb(&m_nam, this)
{
    m_cache = std::make_unique<MetadataCache>(MetadataCache::defaultDir());

    connect(&m_mb, &musicbrainz::Client::discIdResolved,
            this, &MetadataResolver::onDiscIdResolved);
    connect(&m_mb, &musicbrainz::Client::discIdFailed,
            this, &MetadataResolver::onDiscIdFailed);
    connect(&m_mb, &musicbrainz::Client::releaseResolved,
            this, &MetadataResolver::onReleaseResolved);
    connect(&m_mb, &musicbrainz::Client::releaseFailed,
            this, &MetadataResolver::onReleaseFailed);
}

MetadataResolver::~MetadataResolver() = default;

void MetadataResolver::setUserAgent(const QString& ua) { m_mb.setUserAgent(ua); }

void MetadataResolver::setCacheDir(const QString& dir) {
    m_cache = std::make_unique<MetadataCache>(dir);
}

void MetadataResolver::resolve(const Request& req)
{
    m_req = req;
    m_failedStages.clear();
    m_pipelineLog.clear();
    m_scoringLog.clear();
    m_pickReason.clear();
    m_stageTimer.start();

    if (m_cache) {
        if (auto hit = m_cache->getByDiscId(req.discId); hit) {
            logRow(m_pipelineLog, QStringLiteral("cache"),
                   QStringLiteral("hit"), m_stageTimer.restart());
            AlbumMeta a = std::move(hit->album);
            a.pipelineLog = m_pipelineLog;
            QString source = a.sourceTag;
            emit stageChanged(QStringLiteral("cache"));
            emitResolved(std::move(a), source);
            return;
        }
    }

    emit stageChanged(QStringLiteral("musicbrainz"));
    m_mb.fetchDiscId(req.discId);
}

void MetadataResolver::onDiscIdResolved(QString discId, QJsonArray releases)
{
    if (discId != m_req.discId) return;

    if (releases.isEmpty()) {
        logRow(m_pipelineLog, QStringLiteral("musicbrainz"),
               QStringLiteral("miss"), m_stageTimer.restart());
        m_failedStages << QStringLiteral("musicbrainz");
        runStage2();
        return;
    }

    const scoring::Pick picked = scoring::pick(releases, m_req.tocSummary);
    m_scoringLog = picked.scoringLog;
    m_pickReason = picked.reason;

    logRow(m_pipelineLog, QStringLiteral("musicbrainz"),
           QStringLiteral("hit"), m_stageTimer.restart());

    if (picked.releaseId.isEmpty()) {
        m_failedStages << QStringLiteral("musicbrainz");
        runStage2();
        return;
    }
    m_mb.fetchRelease(picked.releaseId);
}

void MetadataResolver::onDiscIdFailed(QString discId, int /*httpStatus*/, QString /*message*/)
{
    if (discId != m_req.discId) return;
    logRow(m_pipelineLog, QStringLiteral("musicbrainz"),
           QStringLiteral("fail"), m_stageTimer.restart());
    m_failedStages << QStringLiteral("musicbrainz");
    runStage2();
}

void MetadataResolver::onReleaseResolved(QString /*releaseMbid*/, QJsonObject release)
{
    AlbumMeta a = musicbrainz::flattenRelease(
        release, m_req.discId, m_req.trackCount);
    a.confidence = 90;
    a.sourceTag  = QStringLiteral("musicbrainz");
    a.pickReason = m_pickReason;
    a.scoringLog = m_scoringLog;

    logRow(m_pipelineLog, QStringLiteral("musicbrainz-release"),
           QStringLiteral("hit"), m_stageTimer.restart());
    a.pipelineLog = m_pipelineLog;

    if (m_cache) m_cache->put(m_req.discId, a, release);
    emitResolved(std::move(a), QStringLiteral("musicbrainz"));
}

void MetadataResolver::onReleaseFailed(QString /*releaseMbid*/,
                                       int /*httpStatus*/, QString /*message*/)
{
    logRow(m_pipelineLog, QStringLiteral("musicbrainz-release"),
           QStringLiteral("fail"), m_stageTimer.restart());
    m_failedStages << QStringLiteral("musicbrainz-release");
    runStage2();
}

void MetadataResolver::runStage2()
{
    emit stageChanged(QStringLiteral("cd-text"));
    // Scaffold — always nullopt today. CdTextReader::read takes a device
    // path, but at this stage we don't carry one; once the macOS bridge
    // lands, the caller will pass the BSD name in via Request.
    const auto packs = CdTextReader::read(QString());
    if (packs) {
        // Will be hooked up alongside the macOS IOKit bridge.
        logRow(m_pipelineLog, QStringLiteral("cd-text"),
               QStringLiteral("hit"), m_stageTimer.restart());
    } else {
        logRow(m_pipelineLog, QStringLiteral("cd-text"),
               QStringLiteral("miss"), m_stageTimer.restart());
        m_failedStages << QStringLiteral("cd-text");
    }
    runStage3();
}

void MetadataResolver::runStage3()
{
    emit stageChanged(QStringLiteral("acoustid"));
    // Scaffold — Chromaprint isn't vendored yet; the provider returns
    // nullopt unconditionally.
    const auto match = AcoustIdProvider::lookup(QByteArray(), 0);
    if (match) {
        logRow(m_pipelineLog, QStringLiteral("acoustid"),
               QStringLiteral("hit"), m_stageTimer.restart());
    } else {
        logRow(m_pipelineLog, QStringLiteral("acoustid"),
               QStringLiteral("miss"), m_stageTimer.restart());
        m_failedStages << QStringLiteral("acoustid");
    }
    runStage4();
}

void MetadataResolver::runStage4()
{
    emit stageChanged(QStringLiteral("stub"));
    AlbumMeta a = buildStub(m_req);
    a.pipelineLog = m_pipelineLog;
    a.scoringLog  = m_scoringLog;

    // Persist to the append-only pending-submissions log.
    PendingSubmissions::append(m_req.discId, m_req.tocForSubmissions,
                               m_req.ripDir, m_failedStages);

    if (m_cache) m_cache->put(m_req.discId, a, QJsonObject{});
    logRow(m_pipelineLog, QStringLiteral("stub"),
           QStringLiteral("commit"), m_stageTimer.restart());
    a.pipelineLog = m_pipelineLog;
    emitResolved(std::move(a), QStringLiteral("stub"));
}

void MetadataResolver::emitResolved(AlbumMeta album, const QString& sourceTag)
{
    emit resolved(std::move(album), sourceTag);
}

void MetadataResolver::logStage(const QString& stage, const QString& outcome)
{
    logRow(m_pipelineLog, stage, outcome, m_stageTimer.restart());
}

} // namespace concerto::metadata
