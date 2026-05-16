#include "AudioEngine.h"
#include "AudioFeatures.h"
#include "CdShield.h"
#include "EqController.h"
#include "FftProcessor.h"
#include "MusicBlocker.h"
#include "PlaylistModel.h"
#include "Ripper.h"
#include "SystemPaths.h"
#include "macos_prewarm.h"
#ifdef Q_OS_MACOS
#include "macos_titlebar.h"
#endif

#include <QDir>
#include <QFont>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSettings>
#include <QTimer>

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    QGuiApplication::setApplicationName("Concerto");
    QGuiApplication::setApplicationVersion("0.1");
    QGuiApplication::setOrganizationName("Thompson");
    QQuickStyle::setStyle("Basic");

    QGuiApplication::setFont(QFont("Inter"));

    // macOS: pay the NSOpenPanel cold-start cost now (during app launch)
    // rather than later on the user's first Open Folder click — otherwise
    // the main-thread stall drains Qt's audio ring buffer and skips.
    // See docs/AUDIO_THREADING.md for the full explanation.
    prewarmOpenPanel();

    // CD-ripping QoL: while Concerto is open, intercept audio-CD
    // insertion events before Music.app's auto-launch handler can react.
    // CdShield DA-claims any audio-bearing IOCDMedia (data CDs fall
    // through to Finder); MusicBlocker is a secondary observer that
    // force-terminates Music/iTunes if anything still manages to fresh-
    // launch them. Both no-op on iOS / non-Apple. Stack lifetime — they
    // stop() in their destructors after app.exec() returns.
    plyr::MusicBlocker musicBlocker;
    musicBlocker.start();
    plyr::cd::CdShield cdShield;
    cdShield.start();

    PlaylistModel playlist;
    FftProcessor  fft;
    AudioFeatures features;
    AudioEngine   audio;
    audio.setFftProcessor(&fft);
    audio.setAudioFeatures(&features);
    EqController  eq(&audio);

    // Gapless wiring.
    //
    //   • User changes currentIndex  → audio.playAt + enqueue next
    //   • Audio crosses segment      → audio.activeTrackChanged
    //                                  → silently move currentIndex +
    //                                    enqueue the track after that
    //   • Decoder idle, no queue     → audio.readyForNextTrack (unused)
    bool advancingByEngine = false;

    auto playCurrentAndQueueNext = [&]() {
        const int idx = playlist.currentIndex();
        if (idx < 0 || idx >= playlist.count()) return;
        audio.playAt(idx, playlist.urlAt(idx));
        if (idx + 1 < playlist.count())
            audio.enqueueAt(idx + 1, playlist.urlAt(idx + 1));
    };

    QObject::connect(&playlist, &PlaylistModel::currentIndexChanged,
                     &audio, [&]() {
                         if (advancingByEngine) return;
                         playCurrentAndQueueNext();
                     });

    QObject::connect(&audio, &AudioEngine::activeTrackChanged,
                     &playlist, [&](int idx) {
                         advancingByEngine = true;
                         playlist.setCurrentIndex(idx);
                         advancingByEngine = false;

                         // Keep the pipeline full — enqueue the one after
                         // the newly-active track.
                         if (idx + 1 < playlist.count())
                             audio.enqueueAt(idx + 1, playlist.urlAt(idx + 1));
                     });

    // ------------------------------------------------------------------
    // Persistent state — QSettings is the cross-platform Qt abstraction
    // over whatever the host OS uses (NSUserDefaults on iOS, a .plist
    // under ~/Library/Preferences on macOS, the Registry on Windows,
    // SharedPreferences on Android, ~/.config INI on Linux). Since we
    // called setOrganizationName + setApplicationName above, QSettings
    // with no args writes to the correct per-app location on each OS.
    //
    //   keys: folderPath (QString), currentIndex (int),
    //         positionMs (qint64), volume (float)
    // ------------------------------------------------------------------
    QSettings settings;

    auto saveState = [&]() {
        settings.setValue("folderPath",        playlist.folderPath());
        settings.setValue("currentIndex",      playlist.currentIndex());
        settings.setValue("positionMs",        audio.position());
        settings.setValue("volume",            audio.volume());
        settings.setValue("syncCalibrationMs", audio.syncCalibrationMs());
        settings.setValue("viz16BandSlope",    fft.displaySlope());
    };

    // Restore the A/V-sync calibration bias.
    //
    // Before the analytic A/V-sync path landed, this property was named
    // `lookaheadMs` and held a static lookahead in ms (default 35). Now
    // the formula computes the lookahead analytically and this value is
    // an additive bias (default 0). On first launch after the rename:
    // read the old key for back-compat, treat it as an offset relative
    // to the old default, write the new key, delete the old one.
    {
        if (settings.contains("syncCalibrationMs")) {
            const int saved = settings.value("syncCalibrationMs", 0).toInt();
            if (saved != audio.syncCalibrationMs()) {
                audio.setSyncCalibrationMs(saved);
            }
        } else if (settings.contains("lookaheadMs")) {
            // Migrate: legacy default was 35; treat (saved − 35) as the
            // new calibration bias so users who tuned it preserve their
            // perceived offset. Then drop the old key.
            const int savedOld = settings.value("lookaheadMs", 35).toInt();
            const int migrated = savedOld - 35;
            audio.setSyncCalibrationMs(migrated);
            settings.setValue("syncCalibrationMs", migrated);
            settings.remove("lookaheadMs");
            qInfo() << "[settings] migrated lookaheadMs=" << savedOld
                    << "to syncCalibrationMs=" << migrated;
        }
    }

    // Restore the 16-band visualizer's SPAN-style display tilt. Default
    // +3 dB/oct compensates for music's ~pink spectrum so the bars sit
    // roughly flat on typical material — see FftProcessor.cpp.
    if (settings.contains("viz16BandSlope")) {
        fft.setDisplaySlope(float(settings.value("viz16BandSlope", 3.0).toDouble()));
    }

    // Restore where we left off, or fall back to the default demo folder
    // on first launch.
    const QString savedFolder = settings.value("folderPath").toString();
    if (!savedFolder.isEmpty() && QDir(savedFolder).exists()) {
        playlist.openFolder(savedFolder);
        const int     savedIdx = settings.value("currentIndex", -1).toInt();
        const qint64  savedPos = settings.value("positionMs",   0).toLongLong();
        const float   savedVol = float(settings.value("volume", 1.0).toDouble());
        audio.setVolume(savedVol);
        if (savedIdx >= 0 && savedIdx < playlist.count()) {
            playlist.setCurrentIndex(savedIdx);
            // Seek AFTER the decoder's had a moment to produce bytes —
            // AudioEngine::seek clamps to what's decoded, so seeking
            // immediately lands at 0. ~1.2s gives the decoder plenty
            // of head start even for big FLACs.
            if (savedPos > 0) {
                QTimer::singleShot(1200, &audio, [&audio, savedPos]() {
                    audio.seek(savedPos);
                });
            }
        }
    } else {
        const QString desktopRach =
            QDir::homePath() + "/Desktop/Rachmaninoff_Rips";
        if (QDir(desktopRach).exists()) {
            playlist.openFolder(desktopRach);
            if (playlist.count() > 0)
                playlist.setCurrentIndex(0);
        }
    }

    // Periodic save every 3s so a crash/force-quit still preserves
    // roughly where we were.
    QTimer autoSaveTimer;
    autoSaveTimer.setInterval(3000);
    QObject::connect(&autoSaveTimer, &QTimer::timeout, saveState);
    autoSaveTimer.start();

    // Final save on clean exit — covers ⌘Q, window close, SIGTERM.
    QObject::connect(&app, &QCoreApplication::aboutToQuit, saveState);

    SystemPaths systemPaths;

    // CD-rip orchestrator. Lives on the GUI thread; the actual disc reads
    // happen on a worker thread it owns (RipWorker). When a disc is saved
    // we auto-open it in the playlist and start playback — this is what
    // makes a 14-disc batch usable: disc 1 plays while disc 2 keeps ripping.
    //
    // CdShield is passed in so the Ripper can register a disc-appeared
    // listener — that's what wakes the WaitingForDisc → Identifying
    // transition without polling.
    plyr::cd::Ripper ripper(&cdShield);

    // ---- CD rip streaming preview ------------------------------------
    // Raw-PCM signals from the worker → AudioEngine queued slots. CDDA
    // bytes flow drive → disc buffer → audio sink within ~2 s of rip
    // start; the user is listening to disc N while disc N's sectors are
    // still being read.
    //
    // When the stream kicks off (= audio is about to start playing from
    // the disc), also swing the main playlist over to the batch's
    // parent folder so the user sees the full set's already-saved
    // discs. Skip the reopen if the playlist is already inside the
    // parent hierarchy — don't disturb a "just saved disc 1, playing
    // it" state. No-batch / first-disc-of-new-batch cases get handled
    // by `discTrackReady` (it falls back to opening the temp dir).
    QObject::connect(&ripper, &plyr::cd::Ripper::previewStreamStart,
                     &audio,
                     [&audio, &playlist, &ripper](qint64 ms) {
                         audio.startPreviewStream(ms);
                         const QString parent = ripper.batchParentFolder();
                         if (parent.isEmpty()) return;
                         const QString cur = playlist.folderPath();
                         if (cur == parent) return;
                         if (cur.startsWith(parent + QChar('/'))) return;
                         playlist.openFolder(parent);
                     });
    QObject::connect(&ripper, &plyr::cd::Ripper::previewPcm,
                     &audio,  &AudioEngine::pushPreviewPcm);
    QObject::connect(&ripper, &plyr::cd::Ripper::previewStreamStop,
                     &audio,  &AudioEngine::stopPreviewStream);

    // discTrackReady: each FLAC encode lands. Decide where the main
    // playlist should be rooted:
    //   * Batch with parent_folder set: open the parent so the user
    //     sees previously-saved discs from the same set. The worker
    //     writes this disc directly into <parent>/<finalName>/, so
    //     the in-progress tracks belong there too — we appendTrack
    //     them to the playlist as they encode.
    //   * No parent yet (standalone / first disc of a fresh batch):
    //     open the temp dir itself; same appendTrack flow.
    // Streaming preview is doing the actual audio either way — we
    // don't setCurrentIndex / play here.
    QObject::connect(&ripper, &plyr::cd::Ripper::discTrackReady,
                     &playlist,
                     [&playlist, &ripper](const QString& tempDir,
                                          const QString& flacPath,
                                          int /*trackNumber*/) {
                         const QString parent = ripper.batchParentFolder();
                         const QString desiredFolder =
                             !parent.isEmpty() ? parent : tempDir;
                         if (playlist.folderPath() != desiredFolder) {
                             playlist.openFolder(desiredFolder);
                         }
                         playlist.appendTrack(flacPath);
                     });

    // discSaved: the disc is now durably saved.
    //
    // Streaming preview may still be playing out the buffered CDDA from
    // the rip. We must NOT teardown the preview and restart from track
    // 1 — that'd yank the audio back to the disc start. Instead, swap
    // the playlist's URLs to the saved files (so the UI shows the
    // right paths), align currentIndex with whatever track the stream
    // is currently in (without triggering playAt — advancingByEngine
    // suppresses that), and let the preview run out naturally. The
    // user picks a track or seeks to switch into FLAC playback.
    QObject::connect(&ripper, &plyr::cd::Ripper::discSaved,
                     &playlist,
                     [&playlist, &audio, &ripper, &advancingByEngine]
                     (const QString& fromTempDir, const QString& finalPath) {
                         const bool previewActive =
                             audio.source().toString().startsWith("preview://");

                         // Stash where playback is RIGHT NOW so we can
                         // figure out which track to highlight after
                         // any folder reshuffle.
                         const QVariantList tocRows = ripper.tracks();
                         const qint64 posMs = audio.position();
                         const qint64 durMs = audio.duration();
                         int streamingTrackIdx = -1;
                         if (previewActive && durMs > 0 && !tocRows.isEmpty()) {
                             const double frac =
                                 double(posMs) / double(durMs);
                             for (int i = 0; i < tocRows.size(); ++i) {
                                 const auto m = tocRows[i].toMap();
                                 const double sf =
                                     m.value("startFraction").toDouble();
                                 const double ef =
                                     m.value("endFraction").toDouble();
                                 if (ef <= sf) continue;
                                 if (frac >= sf && frac < ef) {
                                     streamingTrackIdx = i;
                                     break;
                                 }
                             }
                             if (streamingTrackIdx < 0)
                                 streamingTrackIdx = tocRows.size() - 1;
                         }

                         // Update the playlist's view of the files.
                         if (fromTempDir == finalPath) {
                             // In-place rip: URLs already point at the
                             // final location. Nothing to remap.
                         } else if (fromTempDir.isEmpty()
                                    || playlist.folderPath() != fromTempDir) {
                             // Out-of-place save AND playlist isn't on
                             // the temp dir — open the saved folder.
                             playlist.openFolder(finalPath);
                         } else {
                             // Out-of-place save with playlist on the
                             // temp dir: re-target URLs without
                             // disrupting playback.
                             playlist.remapFolder(fromTempDir, finalPath);
                         }

                         if (previewActive) {
                             // Align the playlist highlight with the
                             // streamed track without yanking the
                             // engine. advancingByEngine blocks the
                             // currentIndexChanged → audio.playAt path.
                             if (streamingTrackIdx >= 0
                                 && streamingTrackIdx < playlist.count())
                             {
                                 advancingByEngine = true;
                                 playlist.setCurrentIndex(streamingTrackIdx);
                                 advancingByEngine = false;
                             }
                         } else if (playlist.count() > 0) {
                             // Preview already done / never streamed
                             // (user navigated away). Cold-open start.
                             playlist.setCurrentIndex(0);
                             audio.play();
                         }
                     });

    QQmlApplicationEngine engine;
    auto* ctx = engine.rootContext();
    ctx->setContextProperty("playlist",      &playlist);
    ctx->setContextProperty("fft",           &fft);
    ctx->setContextProperty("audioFeatures", &features);
    ctx->setContextProperty("audio",         &audio);
    ctx->setContextProperty("eq",            &eq);
    ctx->setContextProperty("ripper",        &ripper);
    ctx->setContextProperty("systemPaths",   &systemPaths);

    // Pick the phone-portrait layout on iOS/Android, the desktop layout
    // elsewhere. PLYR_FORCE_MOBILE=1 overrides so the mobile UI can be
    // demoed on a Mac during development.
    const bool forceMobile = qEnvironmentVariableIntValue("PLYR_FORCE_MOBILE") == 1;
#if defined(Q_OS_IOS) || defined(Q_OS_ANDROID)
    const bool isMobile = true;
#else
    const bool isMobile = forceMobile;
#endif
    engine.loadFromModule("PLYR", isMobile ? "MainMobile" : "Main");
    if (engine.rootObjects().isEmpty())
        return -1;

    // Hand the QML window to AudioEngine so it can spin up the macOS
    // CAMetalDisplayLink on top of QQuickWindow's CAMetalLayer. attach
    // is deferred internally until the scene graph has initialized
    // (i.e. the CAMetalLayer exists). No-op on non-Mac builds.
    if (auto* w = qobject_cast<QQuickWindow*>(engine.rootObjects().first())) {
        audio.attachToWindow(w);
#ifdef Q_OS_MACOS
        // Qt 6.9+ NoTitleBarBackgroundHint flag (set in Main.qml) makes
        // the title-bar background transparent, but the title text still
        // renders on top. Hide it natively.
        plyr::hideMacWindowTitle(w);
#endif
    }

    return app.exec();
}
