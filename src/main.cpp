#include "AudioEngine.h"
#include "FftProcessor.h"
#include "PlaylistModel.h"

#include <QDir>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>

int main(int argc, char *argv[])
{
    // QAudioBufferOutput is reliably supported on the FFmpeg Multimedia
    // backend; on macOS's default native (darwin/AVFoundation) backend
    // it is not, which would silence our visualizer. Force FFmpeg before
    // Qt Multimedia initializes.
    qputenv("QT_MEDIA_BACKEND", "ffmpeg");

    QGuiApplication app(argc, argv);

    QGuiApplication::setApplicationName("PLYR Qt");
    QGuiApplication::setOrganizationName("Thompson");
    QQuickStyle::setStyle("Basic");

    PlaylistModel playlist;
    FftProcessor  fft;
    AudioEngine   audio;
    audio.setFftProcessor(&fft);

    // Connect the engine to the playlist BEFORE populating so the first
    // currentIndexChanged fires with the listener already attached.
    QObject::connect(&playlist, &PlaylistModel::currentIndexChanged,
                     &audio, [&](){
                         audio.setSource(playlist.currentUrl());
                     });
    QObject::connect(&audio, &AudioEngine::trackEnded,
                     &playlist, &PlaylistModel::next);

    // Auto-open the default folder on launch, matching Swift PLYR.
    const QString desktopRach =
        QDir::homePath() + "/Desktop/Rachmaninoff_Rips";
    if (QDir(desktopRach).exists()) {
        playlist.openFolder(desktopRach);
        // Auto-play the first track so we can verify the audio pipeline
        // on startup without having to click. Can be removed later.
        if (playlist.count() > 0)
            playlist.setCurrentIndex(0);
    }

    QQmlApplicationEngine engine;
    auto* ctx = engine.rootContext();
    ctx->setContextProperty("playlist", &playlist);
    ctx->setContextProperty("fft",      &fft);
    ctx->setContextProperty("audio",    &audio);
    engine.loadFromModule("PLYR", "Main");
    if (engine.rootObjects().isEmpty())
        return -1;

    return app.exec();
}
