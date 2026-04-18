// QAbstractListModel wrapping a sorted collection of Tracks.
// Exposed to QML as `PlaylistModel`.  Provides:
//
//   - `openFolder(path)` : recursive scan of all *.flac under path,
//                         sorted by (discFolder, trackNumber).
//   - `currentIndex`     : which row is "now playing", bindable in QML.
//   - named roles for every Track field, usable in QML delegates.

#pragma once

#include "Track.h"

#include <QAbstractListModel>
#include <QQmlEngine>
#include <QVector>

class PlaylistModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(int     currentIndex READ currentIndex WRITE setCurrentIndex NOTIFY currentIndexChanged)
    Q_PROPERTY(int     count        READ count                               NOTIFY countChanged)
    Q_PROPERTY(QString folderPath   READ folderPath                          NOTIFY folderPathChanged)
    Q_PROPERTY(bool    isScanning   READ isScanning                          NOTIFY isScanningChanged)

public:
    enum Roles {
        UrlRole = Qt::UserRole + 1,
        DiscFolderRole,
        DiscNumberRole,
        TrackNumberRole,
        TitleRole,
        ArtistRole,
        AlbumRole,
        DateRole,
        RecordingDateRole,
        RecordingDateDisplayRole,
        YearRole,
        GenreRole,
        ComposerRole,
        DurationRole,
    };
    Q_ENUM(Roles)

    explicit PlaylistModel(QObject* parent = nullptr);

    // QAbstractListModel
    int      rowCount(const QModelIndex& = {}) const override { return int(m_tracks.size()); }
    QVariant data(const QModelIndex& idx, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int     currentIndex() const        { return m_currentIndex; }
    void    setCurrentIndex(int i);
    int     count() const               { return int(m_tracks.size()); }
    QString folderPath() const          { return m_folderPath; }
    bool    isScanning() const          { return m_scanning; }

    // Access the backing C++ data (non-QML).
    const Track& at(int i) const        { return m_tracks[i]; }

    Q_INVOKABLE void openFolder(const QString& path);
    Q_INVOKABLE void openFolderUrl(const QUrl& url);

signals:
    void currentIndexChanged();
    void countChanged();
    void folderPathChanged();
    void isScanningChanged();

private:
    QVector<Track> m_tracks;
    int            m_currentIndex = -1;
    QString        m_folderPath;
    bool           m_scanning = false;
};
