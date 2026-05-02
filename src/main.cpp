#include "AudioEngine.h"
#include "EqController.h"
#include "FftProcessor.h"
#include "PlaylistModel.h"
#include "SystemPaths.h"
#include "macos_prewarm.h"

#include <QDir>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QSettings>
#include <QTimer>

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    QGuiApplication::setApplicationName("Concerto");
    QGuiApplication::setOrganizationName("Thompson");
    QQuickStyle::setStyle("Basic");

    // macOS: pay the NSOpenPanel cold-start cost now (during app launch)
    // rather than later on the user's first Open Folder click — otherwise
    // the main-thread stall drains Qt's audio ring buffer and skips.
    // See docs/AUDIO_THREADING.md for the full explanation.
    prewarmOpenPanel();

    PlaylistModel playlist;
    FftProcessor  fft;
    AudioEngine   audio;
    audio.setFftProcessor(&fft);
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
        settings.setValue("folderPath",   playlist.folderPath());
        settings.setValue("currentIndex", playlist.currentIndex());
        settings.setValue("positionMs",   audio.position());
        settings.setValue("volume",       audio.volume());
    };

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

    QQmlApplicationEngine engine;
    auto* ctx = engine.rootContext();
    ctx->setContextProperty("playlist",    &playlist);
    ctx->setContextProperty("fft",         &fft);
    ctx->setContextProperty("audio",       &audio);
    ctx->setContextProperty("eq",          &eq);
    ctx->setContextProperty("systemPaths", &systemPaths);

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

    return app.exec();
}
