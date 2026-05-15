// Ripper — stub pipeline. See Ripper.h for the long-form intent.
//
// The advance() timer walks the state machine through every visible phase
// on a compressed timeline (~12s end-to-end) so the QML side can be built
// and iterated against realistic-feeling signal shapes. Each phase emits
// the same signals the real pipeline will, in roughly the same order:
//
//   WaitingForDisc → Identifying → Reading → Encoding → Verifying →
//   SavePending → (saveTo) → Saving → Done
//
// Real I/O lands later in RipWorker (a QObject on its own QThread); this
// class then becomes a thin facade that proxies the worker's queued signals.

#include "Ripper.h"

#include <QDateTime>
#include <QDir>
#include <QRegularExpression>
#include <QTimer>
#include <QVariantMap>

namespace plyr::cd {

namespace {

// Stub disc: 17 tracks, ~70 minutes — vaguely Ravel-shaped. Used by the
// stub pipeline to populate the rip view with realistic data while the
// real CdDevice path is being wired up.
struct StubTrack { int durationSec; const char* title; };
const StubTrack kStubTracks[] = {
    { 358, "Boléro" },
    { 412, "La Valse" },
    { 287, "Pavane pour une infante défunte" },
    { 522, "Daphnis et Chloé — Suite No. 2 (Lever du jour)" },
    { 401, "Daphnis et Chloé — Suite No. 2 (Pantomime)" },
    { 263, "Daphnis et Chloé — Suite No. 2 (Danse générale)" },
    { 195, "Ma mère l'Oye — Pavane de la Belle au bois dormant" },
    { 218, "Ma mère l'Oye — Petit Poucet" },
    { 207, "Ma mère l'Oye — Laideronnette" },
    { 234, "Ma mère l'Oye — Les entretiens de la Belle et de la Bête" },
    { 197, "Ma mère l'Oye — Le jardin féerique" },
    { 252, "Rapsodie espagnole — Prélude à la nuit" },
    { 138, "Rapsodie espagnole — Malagueña" },
    { 196, "Rapsodie espagnole — Habanera" },
    { 359, "Rapsodie espagnole — Feria" },
    { 244, "Une barque sur l'océan" },
    { 287, "Alborada del gracioso" },
};

QVariantList buildStubTracks(double offsetFraction = 0.0) {
    QVariantList out;
    int total = 0;
    for (const auto& t : kStubTracks) total += t.durationSec;
    int acc = 0;
    int trackNumber = 1;
    for (const auto& t : kStubTracks) {
        QVariantMap row;
        row["number"]         = trackNumber++;
        row["title"]          = QString::fromUtf8(t.title);
        row["durationSec"]    = t.durationSec;
        row["startFraction"]  = double(acc) / double(total);
        acc += t.durationSec;
        row["endFraction"]    = double(acc) / double(total);
        row["status"]         = QStringLiteral("pending");
        out.append(row);
        Q_UNUSED(offsetFraction);
    }
    return out;
}

void setTrackStatus(QVariantList& tracks, int oneBased, const QString& status) {
    const int i = oneBased - 1;
    if (i < 0 || i >= tracks.size()) return;
    auto m = tracks[i].toMap();
    m["status"] = status;
    tracks[i] = m;
}

} // namespace

Ripper::Ripper(QObject* parent) : QObject(parent) {
    m_stubTimer = new QTimer(this);
    m_stubTimer->setInterval(80);
    connect(m_stubTimer, &QTimer::timeout, this, &Ripper::stubAdvance);
}

Ripper::~Ripper() = default;

void Ripper::setState(State s) {
    if (m_state == s) return;
    m_state = s;
    emit stateChanged();
}

void Ripper::startSession(const QString& resumeBatchId) {
    if (m_state != State::Idle) return;

    if (!resumeBatchId.isEmpty()) {
        // Stub: pretend the named batch exists and is partway through.
        m_inBatch           = true;
        m_batchId           = resumeBatchId;
        m_batchAlbumTitle   = QStringLiteral("Ravel — Complete Edition");
        m_batchDoneCount    = 2;
        m_batchTotalCount   = 14;
        m_batchExpectedDisc = 3;
        emit batchChanged();
    }

    // Start at WaitingForDisc so the "Insert a CD to rip" prompt is
    // visible on first open. The auto-advance timer simulates disc
    // insertion ~2 s later by sliding into Identifying.
    m_demoStep = m_inBatch ? 1 : 0;
    applyDemoStep(m_demoStep);
    m_stubTick = 0;
    m_stubTimer->start();
}

void Ripper::endSession() {
    m_stubTimer->stop();
    // Reset progress state but keep batch info (it's persisted separately).
    m_readProgress = m_encodeProgress = m_verifyProgress = 0.0;
    m_currentLba = 0;
    m_currentSpeedSecPerSec = 0.0;
    m_currentMultiplier = 0.0;
    m_etaSec = 0;
    m_currentTrackNumber = 0;
    m_verifySummary.clear();
    m_zeroFilledRanges.clear();
    m_tracks.clear();
    m_errorMessage.clear();
    m_discPresent = false;
    m_hasMatch = false;
    emit discChanged();
    emit tracksChanged();
    emit progressChanged();
    emit zeroFilledRangesChanged();
    setState(State::Idle);
}

void Ripper::stopRip(bool deleteBatch) {
    if (m_state == State::Idle || m_state == State::Done) {
        if (deleteBatch && !m_batchId.isEmpty()) {
            // Real impl removes the batch JSON file here.
            m_inBatch = false;
            m_batchId.clear();
            emit batchChanged();
        }
        return;
    }

    setState(State::Cancelling);
    m_stubTimer->stop();
    // Real impl signals the worker to stop and waits for it; stub just
    // resets immediately.
    if (deleteBatch && !m_batchId.isEmpty()) {
        m_inBatch = false;
        m_batchId.clear();
        emit batchChanged();
    }
    endSession();
}

void Ripper::saveTo(const QUrl& parentFolder, const QString& folderNameOverride) {
    if (m_state != State::SavePending) return;

    setState(State::Saving);

    QString name = folderNameOverride;
    if (name.isEmpty()) {
        // Auto-name from MB match; fall back to date-stamped slug.
        if (m_hasMatch) {
            QString slug = m_albumTitle;
            slug.replace(QRegularExpression{QStringLiteral("[^A-Za-z0-9]+")},
                         QStringLiteral("_"));
            if (m_discTotalCount > 1) {
                name = QStringLiteral("%1__Disc_%2")
                           .arg(slug)
                           .arg(m_discPosition, 2, 10, QChar('0'));
            } else {
                name = slug;
            }
        } else {
            name = QStringLiteral("Untitled_CD_")
                       + QDateTime::currentDateTime().toString("yyyyMMdd_HHmm");
        }
    }

    // Real impl: move temp dir → parentFolder/name. Stub: report the
    // intended path so QML can show a Done view + auto-play.
    const QString parent = parentFolder.toLocalFile();
    const QString fullPath = QDir::cleanPath(parent + QDir::separator() + name);
    emit discSaved(fullPath);
    setState(State::Done);
}

void Ripper::ejectDisc() {
    // Real impl: open the CdDevice and call eject(). Stub no-op.
    emit warning(tr("Eject requested (stub)."));
}

void Ripper::deleteResumableBatch(const QString& batchId) {
    // Real impl removes the batch JSON file. Stub no-op.
    Q_UNUSED(batchId);
    m_resumableBatches.clear();
    emit resumableBatchesChanged();
}

// ---------------------------------------------------------------------
// Stub-only: demo stage stepping. Each entry below is a representative
// snapshot of the rip flow at a visible milestone — left/right arrows
// in the rip view advance through them so the design can be reviewed
// without waiting on the timer. Will be removed when the real
// RipWorker lands.
// ---------------------------------------------------------------------

namespace {

struct DemoStep {
    Ripper::State state;
    double  readProgress;
    double  encodeProgress;
    double  verifyProgress;
    bool    discPresent;
    bool    hasMatch;
    bool    inBatch;
    int     batchExpectedDisc;
    int     currentLba;
    double  speed;
    double  multiplier;
    int     etaSec;
    int     currentTrackNumber;
    bool    addZeroFill;        // append the synthetic zero-fill marker
    QString verifySummary;
};

const DemoStep kDemoSteps[] = {
    // 0  WaitingForDisc, no batch
    { Ripper::State::WaitingForDisc, 0, 0, 0, false, false, false, 0,
      0, 0, 0, 0, 0, false, {} },
    // 1  WaitingForDisc, in batch (Insert disc 3 of 14)
    { Ripper::State::WaitingForDisc, 0, 0, 0, false, false, true, 3,
      0, 0, 0, 0, 0, false, {} },
    // 2  Identifying — disc claimed, MB lookup pending
    { Ripper::State::Identifying, 0, 0, 0, true, false, false, 0,
      0, 0, 0, 0, 0, false, {} },
    // 3  Reading at 15%
    { Ripper::State::Reading, 0.15, 0, 0, true, true, false, 0,
      49500, 540.0, 7.2, 360, 3, false, {} },
    // 4  Reading at 50% (zero-fill warning has fired)
    { Ripper::State::Reading, 0.50, 0, 0, true, true, false, 0,
      165000, 540.0, 7.2, 180, 8, true, {} },
    // 5  Reading at 90%
    { Ripper::State::Reading, 0.90, 0, 0, true, true, false, 0,
      297000, 540.0, 7.2, 36, 14, true, {} },
    // 6  Encoding mid-flight
    { Ripper::State::Encoding, 1.0, 0.55, 0, true, true, false, 0,
      0, 0, 0, 0, 0, true, {} },
    // 7  Encoding complete
    { Ripper::State::Encoding, 1.0, 1.0, 0, true, true, false, 0,
      0, 0, 0, 0, 0, true, {} },
    // 8  Verifying mid-flight
    { Ripper::State::Verifying, 1.0, 1.0, 0.55, true, true, false, 0,
      0, 0, 0, 0, 0, true, {} },
    // 9  Verifying complete — track 5 came back warn
    { Ripper::State::Verifying, 1.0, 1.0, 1.0, true, true, false, 0,
      0, 0, 0, 0, 0, true,
      QStringLiteral("16/17 ACCURATE · 1 with warnings · AR offset 0 · CTDB conf 12") },
    // 10 SavePending — save picker auto-opens
    { Ripper::State::SavePending, 1.0, 1.0, 1.0, true, true, false, 0,
      0, 0, 0, 0, 0, true,
      QStringLiteral("16/17 ACCURATE · 1 with warnings · AR offset 0 · CTDB conf 12") },
    // 11 Done
    { Ripper::State::Done, 1.0, 1.0, 1.0, true, true, false, 0,
      0, 0, 0, 0, 0, true,
      QStringLiteral("16/17 ACCURATE · 1 with warnings · AR offset 0 · CTDB conf 12") },
};
constexpr int kDemoStepCount = sizeof(kDemoSteps) / sizeof(kDemoSteps[0]);

} // namespace

int Ripper::demoStepCount() const { return kDemoStepCount; }

void Ripper::demoStep(int delta) {
    if (m_stubTimer) m_stubTimer->stop();
    int next = m_demoStep + delta;
    if (next < 0) next = 0;
    if (next > kDemoStepCount - 1) next = kDemoStepCount - 1;
    m_demoStep = next;
    applyDemoStep(m_demoStep);
}

void Ripper::demoToggleAutoAdvance() {
    if (!m_stubTimer) return;
    if (m_stubTimer->isActive()) {
        m_stubTimer->stop();
    } else {
        // Resume from current state. The stub's advance() reads from
        // m_stubTick relatively — reset to 0 so the next phase starts
        // fresh and progress doesn't snap backwards.
        m_stubTick = 0;
        m_stubTimer->start();
    }
}

void Ripper::applyDemoStep(int idx) {
    if (idx < 0 || idx >= kDemoStepCount) return;
    const auto& s = kDemoSteps[idx];

    // Batch context — only set if the step says so. We don't unset an
    // already-active batch between steps so the user can flip back and
    // forth while staying inside a batch.
    if (s.inBatch && !m_inBatch) {
        m_inBatch           = true;
        m_batchId           = QStringLiteral("demo-batch");
        m_batchAlbumTitle   = QStringLiteral("Ravel — Complete Edition");
        m_batchDoneCount    = 2;
        m_batchTotalCount   = 14;
        m_batchExpectedDisc = s.batchExpectedDisc;
        emit batchChanged();
    } else if (!s.inBatch && m_inBatch && s.state == State::WaitingForDisc) {
        // Only clear the batch when explicitly going to a non-batch
        // waiting state — otherwise the rip flow is ongoing.
        m_inBatch = false;
        m_batchId.clear();
        emit batchChanged();
    }

    m_discPresent = s.discPresent;
    if (s.discPresent) {
        m_driveName          = QStringLiteral("APPLE SUPERDRIVE rev 1.04");
        m_driveOffsetSamples = 6;
        m_driveOffsetFromDb  = true;
        m_trackCount         = sizeof(kStubTracks) / sizeof(kStubTracks[0]);
        m_totalDurationSec   = 0;
        for (const auto& t : kStubTracks) m_totalDurationSec += t.durationSec;
    } else {
        m_driveName.clear();
        m_driveOffsetSamples = 0;
        m_driveOffsetFromDb  = false;
        m_trackCount         = 0;
        m_totalDurationSec   = 0;
    }

    m_hasMatch = s.hasMatch;
    if (s.hasMatch) {
        m_albumTitle     = QStringLiteral("Ravel — Complete Edition");
        m_artist         = QStringLiteral("Maurice Ravel");
        m_date           = QStringLiteral("2017-03-31");
        m_discPosition   = m_inBatch ? m_batchExpectedDisc : 2;
        m_discTotalCount = m_inBatch ? m_batchTotalCount   : 14;
        m_mbDiscId       = QStringLiteral("Lv8KZH0gXJTLgmcOIxqXdLp9hjE-");
        m_accurateRipId  = QStringLiteral("00112233-44556677-aabbccdd");
    } else {
        m_albumTitle.clear();
        m_artist.clear();
        m_date.clear();
        m_discPosition = m_inBatch ? m_batchExpectedDisc : 1;
        m_discTotalCount = m_inBatch ? m_batchTotalCount : 1;
        m_mbDiscId.clear();
        m_accurateRipId.clear();
    }

    // Tracks only exist once we've read a TOC. WaitingForDisc shows an
    // empty data band — the lines appear when the disc is claimed and
    // identification starts. (For a "known disc" resumable batch, the
    // real impl will pre-fill from the cached TOC; not modelled here.)
    if (s.discPresent) {
        m_tracks = buildStubTracks();
    } else {
        m_tracks.clear();
    }
    const int n = m_tracks.size();
    auto setStatus = [&](int oneBased, const QString& status) {
        setTrackStatus(m_tracks, oneBased, status);
    };

    if (s.state == State::Reading) {
        const int currentTrack = s.currentTrackNumber;
        for (int i = 1; i <= n; ++i) {
            const int idx0 = i - 1;
            const double endFrac = m_tracks[idx0].toMap()
                .value("endFraction").toDouble();
            if (endFrac <= s.readProgress)       setStatus(i, "read");
            else if (i == currentTrack)          setStatus(i, "reading");
        }
    } else if (s.state == State::Encoding) {
        const int upTo = int(s.encodeProgress * n);
        for (int i = 1; i <= n; ++i) {
            if (i <= upTo) setStatus(i, "encoded");
            else           setStatus(i, "read");
        }
    } else if (s.state == State::Verifying
               || s.state == State::SavePending
               || s.state == State::Done) {
        const int upTo = (s.state == State::Verifying)
            ? int(s.verifyProgress * n) : n;
        for (int i = 1; i <= n; ++i) {
            if (i <= upTo) setStatus(i, (i == 5) ? "warn" : "ok");
            else           setStatus(i, "encoded");
        }
    }

    m_readProgress         = s.readProgress;
    m_encodeProgress       = s.encodeProgress;
    m_verifyProgress       = s.verifyProgress;
    m_currentLba           = s.currentLba;
    m_currentSpeedSecPerSec = s.speed;
    m_currentMultiplier    = s.multiplier;
    m_etaSec               = s.etaSec;
    m_currentTrackNumber   = s.currentTrackNumber;
    m_verifySummary        = s.verifySummary;

    m_zeroFilledRanges.clear();
    if (s.addZeroFill) {
        QVariantMap zf;
        zf["fraction"] = 0.35;
        zf["sectors"]  = 4;
        m_zeroFilledRanges.append(zf);
    }

    setState(s.state);
    emit discChanged();
    emit tracksChanged();
    emit progressChanged();
    emit zeroFilledRangesChanged();
}

// --------------------------------------------------------------------
// Stub pipeline — ticks at 80ms, walks through every visible phase on
// a compressed timeline. ~12 seconds end-to-end.
// --------------------------------------------------------------------
void Ripper::stubAdvance() {
    ++m_stubTick;

    switch (m_state) {
    case State::WaitingForDisc: {
        // ~25 ticks (~2 s) of the "Insert a CD to rip" prompt, then
        // simulate disc insertion by advancing to Identifying.
        if (m_stubTick < 25) return;
        m_stubTick = 0;
        m_demoStep = 2;
        applyDemoStep(m_demoStep);
        return;
    }

    case State::Identifying: {
        // 12 ticks (~1s) of "identifying", then publish disc + MB info
        // and drop into Reading.
        if (m_stubTick < 12) return;

        m_discPresent        = true;
        m_driveName          = QStringLiteral("APPLE SUPERDRIVE rev 1.04");
        m_driveOffsetSamples = 6;
        m_driveOffsetFromDb  = true;
        m_trackCount         = sizeof(kStubTracks) / sizeof(kStubTracks[0]);
        m_totalDurationSec   = 0;
        for (const auto& t : kStubTracks) m_totalDurationSec += t.durationSec;

        m_hasMatch         = true;
        m_albumTitle       = QStringLiteral("Ravel — Complete Edition");
        m_artist           = QStringLiteral("Maurice Ravel");
        m_date             = QStringLiteral("2017-03-31");
        m_discPosition     = m_inBatch ? m_batchExpectedDisc : 2;
        m_discTotalCount   = m_inBatch ? m_batchTotalCount   : 14;
        m_mbDiscId         = QStringLiteral("Lv8KZH0gXJTLgmcOIxqXdLp9hjE-");
        m_accurateRipId    = QStringLiteral("00112233-44556677-aabbccdd");

        m_tracks = buildStubTracks();

        emit discChanged();
        emit tracksChanged();

        m_stubTick = 0;
        setState(State::Reading);
        return;
    }

    case State::Reading: {
        // Advance read progress to 100% over ~75 ticks (~6s).
        m_readProgress = std::min(1.0, m_stubTick / 75.0);
        m_currentLba   = int(m_readProgress * 330000);  // ~74min disc
        m_currentSpeedSecPerSec = 540.0;
        m_currentMultiplier     = 7.2;
        m_etaSec = int((1.0 - m_readProgress) * 60.0);

        // Light up the current track based on progress.
        for (int i = 0; i < m_tracks.size(); ++i) {
            const auto m = m_tracks[i].toMap();
            const double s = m.value("startFraction").toDouble();
            const double e = m.value("endFraction").toDouble();
            if (m_readProgress >= s && m_readProgress < e) {
                m_currentTrackNumber = m.value("number").toInt();
                if (m.value("status").toString() == QLatin1String("pending"))
                    setTrackStatus(m_tracks, m_currentTrackNumber,
                                   QStringLiteral("reading"));
            } else if (m_readProgress >= e
                       && m.value("status").toString() == QLatin1String("reading")) {
                setTrackStatus(m_tracks, m.value("number").toInt(),
                               QStringLiteral("read"));
            }
        }

        // Synthetic zero-fill warning around 35% to exercise the marker.
        if (m_stubTick == 26) {
            QVariantMap zf;
            zf["fraction"] = 0.35;
            zf["sectors"]  = 4;
            m_zeroFilledRanges.append(zf);
            emit zeroFilledRangesChanged();
            emit warning(tr("Zero-filled LBA 115500..115504 (4 sectors) — verifier will flag affected track"));
        }

        emit progressChanged();
        emit tracksChanged();

        if (m_readProgress >= 1.0) {
            m_currentSpeedSecPerSec = 0.0;
            m_currentMultiplier     = 0.0;
            m_etaSec                = 0;
            m_currentTrackNumber    = 0;
            for (int i = 0; i < m_tracks.size(); ++i) {
                const auto m = m_tracks[i].toMap();
                if (m.value("status").toString() == QLatin1String("reading"))
                    setTrackStatus(m_tracks, m.value("number").toInt(),
                                   QStringLiteral("read"));
            }
            emit tracksChanged();
            m_stubTick = 0;
            setState(State::Encoding);
        }
        return;
    }

    case State::Encoding: {
        // ~25 ticks (~2s) for all tracks; light them up as they encode.
        const int n = m_tracks.size();
        m_encodeProgress = std::min(1.0, m_stubTick / 25.0);
        const int upTo = int(m_encodeProgress * n);
        for (int i = 0; i < n; ++i) {
            const auto m = m_tracks[i].toMap();
            const QString status = m.value("status").toString();
            if (i < upTo && status != QLatin1String("encoded"))
                setTrackStatus(m_tracks, m.value("number").toInt(),
                               QStringLiteral("encoded"));
        }
        emit progressChanged();
        emit tracksChanged();
        if (m_encodeProgress >= 1.0) {
            m_stubTick = 0;
            setState(State::Verifying);
        }
        return;
    }

    case State::Verifying: {
        // ~25 ticks; each track resolves to ok / warn / unknown.
        const int n = m_tracks.size();
        m_verifyProgress = std::min(1.0, m_stubTick / 25.0);
        const int upTo = int(m_verifyProgress * n);
        for (int i = 0; i < n; ++i) {
            const auto m = m_tracks[i].toMap();
            const QString status = m.value("status").toString();
            if (i < upTo && (status == QLatin1String("encoded"))) {
                // Pretend track 5 (synthetic zero-fill) verifies as warn.
                const int num = m.value("number").toInt();
                const QString next = (num == 5) ? QStringLiteral("warn")
                                                : QStringLiteral("ok");
                setTrackStatus(m_tracks, num, next);
            }
        }
        emit progressChanged();
        emit tracksChanged();
        if (m_verifyProgress >= 1.0) {
            m_verifySummary = QStringLiteral("16/17 ACCURATE · 1 with warnings · AR offset 0 · CTDB conf 12");
            emit progressChanged();
            m_stubTimer->stop();
            setState(State::SavePending);
        }
        return;
    }

    default:
        m_stubTimer->stop();
        return;
    }
}

} // namespace plyr::cd
