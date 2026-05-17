// library_cli — test harness for the folder-time library pipeline.
//
// Mirrors mbquery_cli's shape. Three flags:
//
//   --folder <path>     Path to a folder of audio files. Required.
//   --no-db             Skip the SQLite library DB write step.
//   --no-cache          Use an empty metadata cache directory.
//
// Output: file count, the FolderIdentifier diagnostic chain (which
// stages tried + outcome), the resolved AlbumMeta dump (via the
// existing debugDumpTrack helper for parity with mbquery_cli), and a
// SQLite write confirmation.
//
// Scope: validates Step 1 (LibraryDatabase), Step 2 (AudioTagIo +
// AudioFrameHash), and Step 3 stages Z+A+B of LIBRARY_METADATA_PLAN.md.

#include "AudioFrameHash.h"
#include "AudioTagIo.h"
#include "FolderIdentifier.h"
#include "LibraryDatabase.h"
#include "MetadataCache.h"
#include "MetadataModel.h"
#include "MusicBrainz.h"
#include "SystemPaths.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <QTimer>

namespace {

QTextStream out(stdout);
QTextStream err(stderr);

const QStringList& audioExtensions() {
    static const QStringList exts = {
        QStringLiteral("*.flac"),
        QStringLiteral("*.mp3"),
        QStringLiteral("*.m4a"),
        QStringLiteral("*.aac"),
        QStringLiteral("*.ogg"),
        QStringLiteral("*.opus"),
        QStringLiteral("*.wav"),
        QStringLiteral("*.aiff"),
        QStringLiteral("*.aif"),
    };
    return exts;
}

QStringList listAudioFiles(const QString& folder) {
    QDir d(folder);
    QStringList names = d.entryList(audioExtensions(),
                                    QDir::Files, QDir::Name);
    QStringList out;
    out.reserve(names.size());
    for (const QString& n : names) out << d.absoluteFilePath(n);
    return out;
}

int runFolder(const QString& folder, bool writeDb, bool useCache) {
    out << "=== library_cli ===\n";
    out << "folder: " << folder << "\n";
    out.flush();

    if (!QFileInfo(folder).isDir()) {
        err << "  not a directory: " << folder << "\n";
        return 2;
    }

    const QStringList files = listAudioFiles(folder);
    out << "files:  " << files.size() << "\n";
    for (const QString& f : files)
        out << "  - " << QFileInfo(f).fileName() << "\n";
    out << "\n";
    out.flush();

    if (files.isEmpty()) {
        err << "  no audio files found.\n";
        return 1;
    }

    // Bring up shared services.
    QNetworkAccessManager nam;
    musicbrainz::Client mb(&nam);
    mb.setUserAgent(QStringLiteral(
        "concerto-library/0.1 ( https://github.com/tfletcher/plyr-qt )"));

    QString cacheDir;
    if (useCache) {
        cacheDir = concerto::metadata::MetadataCache::defaultDir();
    } else {
        cacheDir = QDir::tempPath()
                 + QStringLiteral("/concerto-library-cli-empty-cache");
        // Best-effort wipe so --no-cache means what it says.
        QDir(cacheDir).removeRecursively();
    }
    concerto::metadata::MetadataCache cache(cacheDir);

    concerto::library::LibraryDatabase db;
    if (writeDb) {
        if (!db.open()) {
            err << "  failed to open library DB: "
                << db.lastErrorString() << "\n";
            return 1;
        }
        out << "library DB: " << db.dbPath()
            << " (schema v" << db.schemaVersion() << ")\n\n";
        out.flush();
    } else {
        out << "library DB: SKIPPED (--no-db)\n\n";
        out.flush();
    }

    concerto::library::FolderIdentifier ident(
        &mb, &cache, writeDb ? &db : nullptr);

    QEventLoop loop;
    concerto::library::FolderIdentifier::Result result;
    QObject::connect(&ident,
                     &concerto::library::FolderIdentifier::identified,
                     &loop,
                     [&](const concerto::library::FolderIdentifier::Result& r) {
                         result = r;
                         loop.quit();
                     });

    // 10s overall timeout for the test harness — Stage B's MB call is
    // the only real bottleneck and shouldn't take more than ~2s.
    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
    guard.start(15000);

    concerto::library::FolderIdentifier::Request req;
    req.folderPath = folder;
    req.filePaths  = files;
    QTimer::singleShot(0, &ident, [&] { ident.identify(req); });
    loop.exec();

    out << "=== Identification diagnostic ===\n";
    out << result.diagnostic << "\n";
    out << "\n";

    out << "=== Result ===\n";
    out << "  identified:    " << (result.identified ? "yes" : "no") << "\n";
    out << "  source:        " << result.source << "\n";
    if (result.identified) {
        const auto& a = result.album;
        out << "  releaseId:     " << a.releaseId << "\n";
        out << "  title:         " << a.title << "\n";
        out << "  artistCredit:  " << a.artistCredit << "\n";
        out << "  date:          " << a.date << "\n";
        out << "  country:       " << a.country << "\n";
        out << "  barcode:       " << a.barcode << "\n";
        out << "  label:         " << a.label << "\n";
        out << "  catalog:       " << a.catalogNumber << "\n";
        out << "  disc:          " << a.discPosition << " of "
            << a.discTotalCount << "\n";
        out << "  tracks:        " << a.tracks.size() << "\n";
        out << "  confidence:    " << a.confidence << "\n";
        out << "\n";
        // First few tracks only — full dump matches mbquery_cli style.
        const int limit = std::min(int(a.tracks.size()), 3);
        for (int i = 0; i < limit; ++i) {
            out << "--- Track " << (i + 1) << " ---\n";
            out << concerto::metadata::debugDumpTrack(a, i);
            out << "\n";
        }
        if (a.tracks.size() > limit)
            out << "  (... " << (a.tracks.size() - limit)
                << " more track(s))\n\n";
    }

    // DB writeback.
    if (writeDb && result.identified) {
        // Hash + persist each file's row first so subsequent Stage Z
        // DB-as-authority hits work on re-open.
        const qint64 ts = QDateTime::currentSecsSinceEpoch();
        int filesWritten = 0;
        for (int i = 0; i < files.size(); ++i) {
            const QString& path = files[i];
            const QString hash = concerto::library::AudioFrameHash::compute(path, 10);
            if (hash.isEmpty()) {
                out << "  warn: hash failed for " << QFileInfo(path).fileName()
                    << "\n";
                continue;
            }
            concerto::library::FileRow f;
            f.contentHash    = hash;
            f.path           = path;
            f.format         = concerto::library::AudioTagIo::formatName(
                                   concerto::library::AudioTagIo::detect(path));
            f.durationSec    = concerto::library::AudioTagIo::readDurationSec(path);
            f.sizeBytes      = QFileInfo(path).size();
            f.firstSeenTs    = ts;
            f.lastSeenTs     = ts;
            f.releaseMbid    = result.album.releaseId;
            f.mediumPosition = result.album.discPosition;
            // Best-effort track position: parse TRACKNUMBER from tags
            // first, fall back to file-index ordinal.
            auto tags = concerto::library::AudioTagIo::read(path);
            if (tags) {
                const auto v = concerto::library::AudioTagIo::readField(
                    *tags, QStringLiteral("TRACKNUMBER"));
                bool ok = false;
                const int tn = v.split(QLatin1Char('/')).first().toInt(&ok);
                f.trackPosition = ok ? tn : (i + 1);
            } else {
                f.trackPosition = i + 1;
            }
            if (db.upsertFile(f) > 0) ++filesWritten;
        }

        const int64_t rowid = db.upsertAlbumMeta(result.album);
        out << "=== SQLite write ===\n";
        out << "  releases row: " << (rowid > 0 ? "OK" : "FAILED")
            << " (rowid=" << rowid << ")\n";
        out << "  files written: " << filesWritten << "/"
            << files.size() << "\n";

        concerto::library::FolderRow fr;
        fr.path               = folder;
        fr.lastIdentifySource = result.source;
        fr.releaseMbid        = result.album.releaseId;
        fr.fileCount          = files.size();
        fr.lastScannedTs      = ts;
        db.upsertFolder(fr);
        out << "  folder row:   OK\n";
    }

    out.flush();
    return result.identified ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Concerto"));
    QCoreApplication::setOrganizationName(QStringLiteral("Thompson"));
    concerto::paths::migrateAppData();

    const QStringList args = app.arguments();
    QString folder;
    bool writeDb = true;
    bool useCache = true;
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == QStringLiteral("--folder") && i + 1 < args.size()) {
            folder = args[++i];
        } else if (args[i] == QStringLiteral("--no-db")) {
            writeDb = false;
        } else if (args[i] == QStringLiteral("--no-cache")) {
            useCache = false;
        } else if (args[i] == QStringLiteral("--help")
                || args[i] == QStringLiteral("-h")) {
            out << "usage: library_cli --folder <path> [--no-db] [--no-cache]\n";
            return 0;
        }
    }
    if (folder.isEmpty()) {
        err << "usage: library_cli --folder <path> [--no-db] [--no-cache]\n";
        return 2;
    }
    return runFolder(folder, writeDb, useCache);
}
