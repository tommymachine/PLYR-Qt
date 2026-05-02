// Tiny QObject exposing OS-level path information to QML. Currently just
// mounted volumes for the FolderPicker sidebar.
//
// Volumes live in QStorageInfo (Qt C++) with no QML-side equivalent, so
// this is the minimal bridge.

#pragma once

#include <QObject>
#include <QVariantList>

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
