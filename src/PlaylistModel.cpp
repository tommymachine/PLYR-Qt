#include "PlaylistModel.h"

#include <QDir>
#include <QDirIterator>
#include <QUrl>
#include <algorithm>

PlaylistModel::PlaylistModel(QObject* parent)
    : QAbstractListModel(parent)
{}

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
