#include "PlaylistModel.h"

#include <QDir>
#include <QDirIterator>
#include <QSettings>
#include <QUrl>
#include <algorithm>

PlaylistModel::PlaylistModel(QObject* parent)
    : QAbstractListModel(parent)
{
    loadRecents();
}

QVariant PlaylistModel::data(const QModelIndex& idx, int role) const
{
    if (!idx.isValid() || idx.row() < 0 || idx.row() >= int(m_tracks.size()))
        return {};
    const auto& t = m_tracks[idx.row()];
    switch (role) {
        case UrlRole:                  return t.url;
        case DiscFolderRole:           return t.discFolder;
        case DiscNumberRole:           return t.discNumber;
        case TrackNumberRole:          return t.trackNumber;
        case TitleRole:                return t.title;
        case ArtistRole:               return t.artist;
        case AlbumRole:                return t.album;
        case DateRole:                 return t.date;
        case RecordingDateRole:        return t.recordingDate;
        case RecordingDateDisplayRole: return t.recordingDateDisplay();
        case YearRole:                 return t.year();
        case GenreRole:                return t.genre;
        case ComposerRole:             return t.composer;
        case DurationRole:             return t.duration;
        default:                       return {};
    }
}

QHash<int, QByteArray> PlaylistModel::roleNames() const
{
    return {
        { UrlRole,                  "url"                  },
        { DiscFolderRole,           "discFolder"           },
        { DiscNumberRole,           "discNumber"           },
        { TrackNumberRole,          "trackNumber"          },
        { TitleRole,                "title"                },
        { ArtistRole,               "artist"               },
        { AlbumRole,                "album"                },
        { DateRole,                 "date"                 },
        { RecordingDateRole,        "recordingDate"        },
        { RecordingDateDisplayRole, "recordingDateDisplay" },
        { YearRole,                 "year"                 },
        { GenreRole,                "genre"                },
        { ComposerRole,             "composer"             },
        { DurationRole,             "duration"             },
    };
}

void PlaylistModel::setCurrentIndex(int i)
{
    if (i < -1 || i >= int(m_tracks.size())) return;
    if (i == m_currentIndex) return;
    m_currentIndex = i;
    emit currentIndexChanged();
}

void PlaylistModel::openFolder(const QString& path)
{
    if (path.isEmpty()) return;

    m_folderPath = path;
    emit folderPathChanged();
    pushRecent(path);

    m_scanning = true;
    emit isScanningChanged();

    QVector<Track> tracks;
    QDirIterator it(path,
                    {"*.flac", "*.FLAC"},
                    QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString file = it.next();
        if (auto t = Track::load(file)) {
            tracks.push_back(*t);
        }
    }

    // Sort: disc folder first, then track number within the disc.
    std::sort(tracks.begin(), tracks.end(),
              [](const Track& a, const Track& b) {
                  if (a.discFolder != b.discFolder)
                      return a.discFolder < b.discFolder;
                  return a.trackNumber < b.trackNumber;
              });

    beginResetModel();
    m_tracks = std::move(tracks);
    m_currentIndex = -1;
    endResetModel();
    emit countChanged();
    emit currentIndexChanged();

    m_scanning = false;
    emit isScanningChanged();
}

void PlaylistModel::openFolderUrl(const QUrl& url)
{
    openFolder(url.toLocalFile());
}

void PlaylistModel::appendTrack(const QString& path)
{
    auto t = Track::load(path);
    if (!t) return;

    // Skip if we already know this URL — defensive against the worker
    // emitting twice (shouldn't happen, but a duplicate row would
    // confuse the gapless queue).
    for (const auto& existing : m_tracks) {
        if (existing.url == t->url) return;
    }

    // Insertion point in (discFolder, trackNumber) sort order — matches
    // openFolder's comparator so the model stays sorted.
    const auto cmp = [](const Track& a, const Track& b) {
        if (a.discFolder != b.discFolder) return a.discFolder < b.discFolder;
        return a.trackNumber < b.trackNumber;
    };
    auto it = std::lower_bound(m_tracks.begin(), m_tracks.end(), *t, cmp);
    const int idx = int(it - m_tracks.begin());

    beginInsertRows(QModelIndex(), idx, idx);
    m_tracks.insert(it, *t);
    endInsertRows();
    emit countChanged();

    // If the new row was inserted at or before currentIndex, the
    // currently-playing logical track shifted one row down.
    if (m_currentIndex >= idx) {
        ++m_currentIndex;
        emit currentIndexChanged();
    }
}

void PlaylistModel::remapFolder(const QString& oldRoot, const QString& newRoot)
{
    if (oldRoot.isEmpty() || newRoot.isEmpty()) return;
    bool any = false;
    for (int i = 0; i < int(m_tracks.size()); ++i) {
        auto& t = m_tracks[i];
        if (!t.url.startsWith(oldRoot)) continue;
        t.url = newRoot + t.url.mid(oldRoot.length());
        any = true;
    }
    if (!any) return;
    if (m_folderPath == oldRoot) {
        m_folderPath = newRoot;
        emit folderPathChanged();
    }
    // dataChanged on every row — UrlRole is the only changed bit, but
    // QML doesn't bind on it directly so this is mostly bookkeeping.
    emit dataChanged(index(0, 0),
                     index(int(m_tracks.size()) - 1, 0),
                     {UrlRole});
    // urlAt() observers re-read on currentIndexChanged.
    emit currentIndexChanged();
}

int PlaylistModel::indexOfTrackByPath(const QString& flacPath) const
{
    if (flacPath.isEmpty()) return -1;
    for (int i = 0; i < int(m_tracks.size()); ++i) {
        if (m_tracks[i].url == flacPath) return i;
    }
    return -1;
}

int PlaylistModel::indexOfRipTrack(const QString& discFolder,
                                   int trackNumber) const
{
    if (discFolder.isEmpty() || trackNumber <= 0) return -1;
    // Match the rip worker's naming: track_%02u.flac in the disc's
    // playback folder (temp dir during rip; saved path after).
    const QString suffix = QStringLiteral("/track_%1.flac")
                               .arg(trackNumber, 2, 10, QChar('0'));
    const QString full = discFolder + suffix;
    for (int i = 0; i < int(m_tracks.size()); ++i) {
        if (m_tracks[i].url == full) return i;
    }
    // Fallback: match by the trailing filename only, in case the disc
    // folder has been remapped (post-save out-of-place rename) but the
    // playlist hasn't refreshed yet, or some other edge case where the
    // exact prefix doesn't line up.
    for (int i = 0; i < int(m_tracks.size()); ++i) {
        if (m_tracks[i].url.endsWith(suffix)) return i;
    }
    return -1;
}

void PlaylistModel::next()
{
    if (m_currentIndex >= 0 && m_currentIndex + 1 < int(m_tracks.size()))
        setCurrentIndex(m_currentIndex + 1);
}

void PlaylistModel::previous()
{
    if (m_currentIndex > 0)
        setCurrentIndex(m_currentIndex - 1);
}

void PlaylistModel::clearRecents()
{
    if (m_recentFolders.isEmpty()) return;
    m_recentFolders.clear();
    saveRecents();
    emit recentFoldersChanged();
}

void PlaylistModel::pushRecent(const QString& path)
{
    if (path.isEmpty()) return;
    // Dedup first so re-opening a folder just moves it to the top.
    m_recentFolders.removeAll(path);
    m_recentFolders.prepend(path);
    while (m_recentFolders.size() > kRecentCap)
        m_recentFolders.removeLast();
    saveRecents();
    emit recentFoldersChanged();
}

void PlaylistModel::loadRecents()
{
    QSettings s;
    m_recentFolders = s.value(QStringLiteral("recentFolders")).toStringList();
    // Cap at kRecentCap in case persistence ever grows it (defensive).
    while (m_recentFolders.size() > kRecentCap)
        m_recentFolders.removeLast();
}

void PlaylistModel::saveRecents() const
{
    QSettings s;
    s.setValue(QStringLiteral("recentFolders"), m_recentFolders);
}
