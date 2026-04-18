#include "PlaylistModel.h"

#include <QDir>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QStandardPaths>

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    QGuiApplication::setApplicationName("PLYR Qt");
    QGuiApplication::setOrganizationName("Thompson");
    QQuickStyle::setStyle("Basic");

    PlaylistModel playlist;
    // Auto-open the default folder on launch, matching the Swift behaviour.
    const QString desktopRach =
        QDir::homePath() + "/Desktop/Rachmaninoff_Rips";
    if (QDir(desktopRach).exists()) {
        playlist.openFolder(desktopRach);
    }

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("playlist", &playlist);
    engine.loadFromModule("PLYR", "Main");
    if (engine.rootObjects().isEmpty())
        return -1;

    return app.exec();
}
