#include "SystemPaths.h"

#include <QStorageInfo>
#include <QUrl>
#include <QVariantMap>

QVariantList SystemPaths::mountedVolumes() const
{
    QVariantList out;
    for (const auto& v : QStorageInfo::mountedVolumes()) {
        if (!v.isValid() || !v.isReady()) continue;
        const QString root = v.rootPath();
        // Filter to user-visible volumes: the boot drive and explicit
        // mounts. macOS exposes a lot of system "/System/Volumes/..."
        // and "/private/..." entries we don't want in the sidebar.
        if (root != QLatin1String("/") &&
            !root.startsWith(QLatin1String("/Volumes/"))) {
            continue;
        }

        QString name = v.displayName();
        if (name.isEmpty()) name = v.name();
        if (name.isEmpty()) name = root;

        QVariantMap m;
        m["name"] = name;
        m["url"]  = QUrl::fromLocalFile(root);
        out.append(m);
    }
    return out;
}
