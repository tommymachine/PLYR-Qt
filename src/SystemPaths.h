// Tiny QObject exposing OS-level path information to QML. Currently just
// mounted volumes for the FolderPicker sidebar.
//
// Volumes live in QStorageInfo (Qt C++) with no QML-side equivalent, so
// this is the minimal bridge.
//
// Also hosts the C++-side app-data-dir helpers used by the metadata /
// rip pipeline. Centralizing here means there's one place where the
// application's on-disk root is named (it's `applicationName()` —
// i.e. whatever `QCoreApplication::setApplicationName()` was called
// with in main.cpp), so a future product-name change is a one-line
// edit rather than a grep-and-replace across every cache module.

#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>

namespace concerto::paths {

// Returns `<GenericDataLocation>/<applicationName>`, e.g.
// `~/Library/Application Support/Concerto` on macOS. Trailing slashes
// are NOT appended — callers concatenate their subdir directly. The
// directory is NOT created here; callers do `QDir().mkpath(...)` as
// needed (matches the pre-existing per-module pattern).
//
// Single source of truth for the on-disk root. All metadata / rip
// persistence modules go through this.
QString appDataDir();

// One-time, idempotent migration from the legacy `PLYR-Qt` app-data
// dir to the current `<applicationName>` dir. Call this once from
// main(), right after setApplicationName() and before any module
// touches its cache.
//
//   * No-op if the legacy dir doesn't exist.
//   * No-op if the new dir already exists with content (collision —
//     leave both alone, log a warning; manual merge is a dev choice).
//   * Otherwise: atomic rename via QDir::rename (single syscall on
//     macOS, no copy). The legacy dir's *name* changes; its inode
//     and contents are untouched.
//
// Logs the outcome to qDebug. Safe to call multiple times.
void migrateAppData();

} // namespace concerto::paths

class SystemPaths : public QObject {
    Q_OBJECT
public:
    explicit SystemPaths(QObject* parent = nullptr) : QObject(parent) {}

    // Returns a list of {name, url} maps — one per mounted volume that's
    // user-visible (root volume + /Volumes/*; system internals filtered).
    // Re-queried each call so newly mounted/ejected drives show up next
    // time the picker opens.
    Q_INVOKABLE QVariantList mountedVolumes() const;
};
