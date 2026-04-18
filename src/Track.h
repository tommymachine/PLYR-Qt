// Higher-level view of a single audio file. Wraps FlacTags + the
// filename and adds derived fields (disc number, display-form recording
// date, etc.) — direct port of the Swift `Track` struct.

#pragma once

#include "FlacTags.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QString>
#include <optional>

struct Track {
    QString  url;            // absolute path on disk
    QString  discFolder;     // parent directory basename
    int      discNumber  = 0;
    int      trackNumber = 0;
    QString  title;
    QString  artist;
    QString  album;
    QString  date;           // release date (e.g. "1992-09-15")
    QString  recordingDate;  // original recording date (e.g. "1929-04-02")
    QString  genre;
    QString  composer;
    double   duration = 0.0;

    QString year() const {
        return date.size() >= 4 ? date.left(4) : date;
    }

    QString recordingDateDisplay() const {
        const auto parts = recordingDate.split(QLatin1Char('-'));
        static const QStringList months = {
            "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
        };
        if (parts.size() == 3) {
            bool ok = false; int y = parts[0].toInt(&ok);
            int m = parts[1].toInt(); int d = parts[2].toInt();
            if (ok && m >= 1 && m <= 12 && d >= 1 && d <= 31)
                return QString("%1 %2 %3").arg(d).arg(months[m - 1]).arg(y);
        }
        if (parts.size() == 2) {
            bool ok = false; int y = parts[0].toInt(&ok);
            int m = parts[1].toInt();
            if (ok && m >= 1 && m <= 12)
                return QString("%1 %2").arg(months[m - 1]).arg(y);
        }
        return recordingDate;
    }

    static std::optional<Track> load(const QString& path) {
        const auto tags = FlacTags::read(path);
        if (!tags) return std::nullopt;

        const QFileInfo fi(path);
        const QString folder   = fi.dir().dirName();
        const QString filename = fi.completeBaseName();    // no extension

        // --- track number: TRACKNUMBER tag, else "NN - Title" filename prefix.
        int trackNum = tags->get("TRACKNUMBER").toInt();
        QString titleFromName = filename;
        const int sep = filename.indexOf(QStringLiteral(" - "));
        if (sep > 0) {
            if (trackNum == 0)
                trackNum = filename.left(sep).trimmed().toInt();
            titleFromName = filename.mid(sep + 3);
        }

        const QString title = tags->get("TITLE", titleFromName);

        // --- disc number: DISCNUMBER tag, else "(Disc N)" in album, else folder "CDnn_…".
        int discNum = tags->get("DISCNUMBER").toInt();
        if (discNum == 0) {
            const QString album = tags->get("ALBUM");
            static const QRegularExpression discRe(QStringLiteral(R"(\(Disc (\d+)\))"));
            const auto m = discRe.match(album);
            if (m.hasMatch()) discNum = m.captured(1).toInt();
        }
        if (discNum == 0) {
            static const QRegularExpression folderRe(QStringLiteral(R"(^CD(\d+))"));
            const auto m = folderRe.match(folder);
            if (m.hasMatch()) discNum = m.captured(1).toInt();
        }

        Track t;
        t.url           = path;
        t.discFolder    = folder;
        t.discNumber    = discNum;
        t.trackNumber   = trackNum;
        t.title         = title;
        t.artist        = tags->get("ARTIST", tags->get("ALBUMARTIST"));
        t.album         = tags->get("ALBUM");
        t.date          = tags->get("DATE");
        t.recordingDate = tags->get("RECORDINGDATE");
        t.genre         = tags->get("GENRE");
        t.composer      = tags->get("COMPOSER");
        t.duration      = tags->duration;
        return t;
    }
};
