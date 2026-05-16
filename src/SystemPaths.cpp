#include "SystemPaths.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QUrl>
#include <QVariantMap>

namespace concerto::paths {

namespace {

// The on-disk root used before the product was renamed Concerto. Kept
// here (not exposed in the header) so the migration is the only place
// in the source tree that knows the old name.
constexpr const char* kLegacyAppDirName = "PLYR-Qt";

QString genericDataRoot() {
    return QStandardPaths::writableLocation(
        QStandardPaths::GenericDataLocation);
}

bool dirIsEmptyOrMissing(const QString& path) {
    QDir d(path);
    if (!d.exists()) return true;
    return d.isEmpty(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
}

} // namespace

QString appDataDir() {
    return genericDataRoot()
         + QLatin1Char('/')
         + QCoreApplication::applicationName();
}

void migrateAppData() {
    const QString root = genericDataRoot();
    if (root.isEmpty()) return;

    const QString legacy = root + QLatin1Char('/') + QLatin1String(kLegacyAppDirName);
    const QString current = appDataDir();

    // Skip if the names happen to match (i.e. someone re-renamed back).
    if (legacy == current) return;

    QDir legacyDir(legacy);
    if (!legacyDir.exists()) {
        // Nothing to migrate — the typical post-migration case.
        return;
    }

    // If both exist with content, refuse to touch either — a manual merge
    // is a dev decision (could be conflicting cache state from a build
    // that ran each name in parallel).
    QDir currentDir(current);
    if (currentDir.exists() && !dirIsEmptyOrMissing(current)) {
        qWarning() << "[paths] migrateAppData: both"
                   << legacy << "and" << current
                   << "exist with content; leaving both alone.";
        return;
    }

    // If the destination exists but is empty (e.g. some module ran
    // mkpath before migration), remove the empty shell so rename can
    // claim the name. rmdir only succeeds on truly empty dirs, so this
    // is safe.
    if (currentDir.exists()) {
        QDir().rmdir(current);
    }

    // Atomic directory rename — single syscall on POSIX; the inode and
    // contents stay put.
    if (QDir().rename(legacy, current)) {
        qInfo() << "[paths] migrated app-data:" << legacy << "->" << current;
    } else {
        qWarning() << "[paths] migrateAppData: rename failed"
                   << legacy << "->" << current
                   << "; leaving legacy dir in place.";
    }
}

} // namespace concerto::paths

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
