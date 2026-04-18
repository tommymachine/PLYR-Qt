#include "FftProcessor.h"
#include "PlaylistModel.h"

#include <QDir>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    QGuiApplication::setApplicationName("PLYR Qt");
    QGuiApplication::setOrganizationName("Thompson");
    QQuickStyle::setStyle("Basic");

    PlaylistModel playlist;
    FftProcessor  fft;

    // Auto-open the default folder on launch, matching Swift PLYR.
    const QString desktopRach =
        QDir::homePath() + "/Desktop/Rachmaninoff_Rips";
    if (QDir(desktopRach).exists())
        playlist.openFolder(desktopRach);

    QQmlApplicationEngine engine;
    auto* ctx = engine.rootContext();
    ctx->setContextProperty("playlist", &playlist);
    ctx->setContextProperty("fft",      &fft);
    engine.loadFromModule("PLYR", "Main");
    if (engine.rootObjects().isEmpty())
        return -1;

    return app.exec();
}
