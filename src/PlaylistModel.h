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

    // Convenience accessors for whatever track is currently selected. All
    // re-emit on currentIndexChanged so QML bindings update automatically.
    Q_PROPERTY(bool    hasCurrent     READ hasCurrent     NOTIFY currentIndexChanged)
    Q_PROPERTY(QUrl    currentUrl     READ currentUrl     NOTIFY currentIndexChanged)
    Q_PROPERTY(QString currentTitle   READ currentTitle   NOTIFY currentIndexChanged)
    Q_PROPERTY(QString currentArtist  READ currentArtist  NOTIFY currentIndexChanged)
    Q_PROPERTY(QString currentAlbum   READ currentAlbum   NOTIFY currentIndexChanged)
    Q_PROPERTY(QString currentComposer READ currentComposer NOTIFY currentIndexChanged)
    Q_PROPERTY(int     currentDiscNumber  READ currentDiscNumber  NOTIFY currentIndexChanged)
    Q_PROPERTY(int     currentTrackNumber READ currentTrackNumber NOTIFY currentIndexChanged)
    Q_PROPERTY(QString currentRecordingDateDisplay READ currentRecordingDateDisplay NOTIFY currentIndexChanged)
    Q_PROPERTY(QString currentYear   READ currentYear   NOTIFY currentIndexChanged)
    Q_PROPERTY(QString currentGenre  READ currentGenre  NOTIFY currentIndexChanged)
    Q_PROPERTY(double  currentDuration READ currentDuration NOTIFY currentIndexChanged)

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

    bool    hasCurrent() const                  { return cur() != nullptr; }
    QUrl    currentUrl() const                  { return cur() ? QUrl::fromLocalFile(cur()->url) : QUrl(); }
    QString currentTitle() const                { return cur() ? cur()->title    : QString(); }
    QString currentArtist() const               { return cur() ? cur()->artist   : QString(); }
    QString currentAlbum() const                { return cur() ? cur()->album    : QString(); }
    QString currentComposer() const             { return cur() ? cur()->composer : QString(); }
    int     currentDiscNumber() const           { return cur() ? cur()->discNumber  : 0; }
    int     currentTrackNumber() const          { return cur() ? cur()->trackNumber : 0; }
    QString currentRecordingDateDisplay() const { return cur() ? cur()->recordingDateDisplay() : QString(); }
    QString currentYear() const                 { return cur() ? cur()->year() : QString(); }
    QString currentGenre() const                { return cur() ? cur()->genre    : QString(); }
    double  currentDuration() const             { return cur() ? cur()->duration : 0.0; }

    Q_INVOKABLE void openFolder(const QString& path);
    Q_INVOKABLE void openFolderUrl(const QUrl& url);
    Q_INVOKABLE void next();
    Q_INVOKABLE void previous();

signals:
    void currentIndexChanged();
    void countChanged();
    void folderPathChanged();
    void isScanningChanged();

private:
    const Track* cur() const {
        return (m_currentIndex >= 0 && m_currentIndex < m_tracks.size())
               ? &m_tracks[m_currentIndex]
               : nullptr;
    }

    QVector<Track> m_tracks;
    int            m_currentIndex = -1;
    QString        m_folderPath;
    bool           m_scanning = false;
};
