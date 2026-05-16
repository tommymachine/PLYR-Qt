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

    Q_PROPERTY(int     currentIndex   READ currentIndex WRITE setCurrentIndex NOTIFY currentIndexChanged)
    Q_PROPERTY(int     count          READ count                               NOTIFY countChanged)
    Q_PROPERTY(QString folderPath     READ folderPath                          NOTIFY folderPathChanged)
    Q_PROPERTY(bool    isScanning     READ isScanning                          NOTIFY isScanningChanged)
    Q_PROPERTY(QStringList recentFolders READ recentFolders                    NOTIFY recentFoldersChanged)

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

    // Append one track to the model, sorted into place by
    // (discFolder, trackNumber). Used by the live-rip preview path:
    // each freshly-encoded track gets appended so the user can hear
    // disc 4's track 1 while disc 4's track 2 is still being read.
    // currentIndex shifts to track the same logical Track if a new
    // row gets inserted before it.
    Q_INVOKABLE void appendTrack(const QString& path);

    // Re-root every track URL that lives under `oldRoot` so it lives
    // under `newRoot` instead. Used after a save-at-end move: the rip
    // streamed playback from the temp dir; once the dir is renamed
    // into place, the playlist's URLs need to follow without losing
    // currentIndex or interrupting playback. Same volume only (the
    // worker's cross-volume save copies+removes — invalidating any
    // open FDs the audio engine holds, which v1 doesn't handle).
    Q_INVOKABLE void remapFolder(const QString& oldRoot,
                                 const QString& newRoot);

    QStringList recentFolders() const { return m_recentFolders; }
    Q_INVOKABLE void clearRecents();

    Q_INVOKABLE QUrl urlAt(int index) const {
        return (index >= 0 && index < int(m_tracks.size()))
               ? QUrl::fromLocalFile(m_tracks[index].url)
               : QUrl();
    }

    // SCANCERTO helper: find the playlist row holding the named FLAC.
    // Plain absolute-path match against Track::url. Returns -1 if not
    // present. Used by the rip view's double-click-to-play.
    Q_INVOKABLE int indexOfTrackByPath(const QString& flacPath) const;

    // SCANCERTO helper: find the row whose URL is
    // <discFolder>/track_NN.flac for the given 1-based track number,
    // where `discFolder` is the in-progress disc's playback path
    // (Ripper::currentDiscPlaybackPath). The rip worker names FLACs
    // as track_%02u.flac. Returns -1 if not present.
    Q_INVOKABLE int indexOfRipTrack(const QString& discFolder,
                                    int trackNumber) const;

signals:
    void currentIndexChanged();
    void countChanged();
    void folderPathChanged();
    void isScanningChanged();
    void recentFoldersChanged();

private:
    const Track* cur() const {
        return (m_currentIndex >= 0 && m_currentIndex < m_tracks.size())
               ? &m_tracks[m_currentIndex]
               : nullptr;
    }

    // Recent-folders history — persisted via QSettings. Cap is the most
    // entries we keep; older ones fall off the end on new opens.
    static constexpr int kRecentCap = 10;
    void pushRecent(const QString& path);
    void loadRecents();
    void saveRecents() const;

    QVector<Track> m_tracks;
    int            m_currentIndex = -1;
    QString        m_folderPath;
    bool           m_scanning = false;
    QStringList    m_recentFolders;
};
