#include "AudioTagIo.h"

#include <QDebug>
#include <QFileInfo>

// TagLib (Homebrew install, dynamic-linked per the LGPL pattern).
// We pull only the public, format-agnostic surface from `<taglib/*.h>`;
// PropertyMap covers every container we care about for read, and the
// FLAC concrete file gives us the writeback path.
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/tpropertymap.h>
#include <taglib/tstring.h>
#include <taglib/tstringlist.h>
#include <taglib/audioproperties.h>
#include <taglib/flacfile.h>
#include <taglib/oggflacfile.h>
#include <taglib/vorbisfile.h>

namespace concerto::library {

namespace {

QString qstr(const TagLib::String& s) {
    // TagLib::String is internally Unicode; toCString(true) returns
    // UTF-8 verbatim.
    return QString::fromUtf8(s.toCString(true));
}

TagLib::String tstr(const QString& s) {
    const QByteArray utf8 = s.toUtf8();
    return TagLib::String(utf8.constData(), TagLib::String::UTF8);
}

} // namespace

AudioTagIo::Format AudioTagIo::detect(const QString& path) {
    const QString ext = QFileInfo(path).suffix().toLower();
    if (ext == QLatin1String("flac")) return Format::Flac;
    if (ext == QLatin1String("mp3"))  return Format::Mp3;
    if (ext == QLatin1String("m4a")
     || ext == QLatin1String("m4b")
     || ext == QLatin1String("mp4")
     || ext == QLatin1String("aac")) return Format::Mp4;
    if (ext == QLatin1String("ogg")
     || ext == QLatin1String("oga")) return Format::Ogg;
    if (ext == QLatin1String("opus")) return Format::Opus;
    if (ext == QLatin1String("wav")
     || ext == QLatin1String("wave")) return Format::Wav;
    if (ext == QLatin1String("aif")
     || ext == QLatin1String("aiff")) return Format::Aiff;
    return Format::Unknown;
}

QString AudioTagIo::formatName(Format f) {
    switch (f) {
        case Format::Flac: return QStringLiteral("flac");
        case Format::Mp3:  return QStringLiteral("mp3");
        case Format::Mp4:  return QStringLiteral("m4a");
        case Format::Ogg:  return QStringLiteral("ogg");
        case Format::Opus: return QStringLiteral("opus");
        case Format::Wav:  return QStringLiteral("wav");
        case Format::Aiff: return QStringLiteral("aiff");
        case Format::Unknown:
        default:           return QStringLiteral("unknown");
    }
}

std::optional<std::map<QString, QStringList>> AudioTagIo::read(
    const QString& path)
{
    if (path.isEmpty()) return std::nullopt;
    // FileRef::isNull on the wrapped File is the open-fail check;
    // empty PropertyMap is a separate state (a file with no tags).
    TagLib::FileRef fr(path.toUtf8().constData(),
                       true,
                       TagLib::AudioProperties::Fast);
    if (fr.isNull() || !fr.file()) return std::nullopt;

    std::map<QString, QStringList> out;
    const TagLib::PropertyMap pm = fr.file()->properties();
    for (auto it = pm.begin(); it != pm.end(); ++it) {
        QStringList values;
        values.reserve(static_cast<int>(it->second.size()));
        for (auto sit = it->second.begin(); sit != it->second.end(); ++sit)
            values << qstr(*sit);
        // Upper-case the key for consistent lookup. Vorbis/ID3 are
        // already case-insensitive; we normalize so the consumer side
        // can match against "MUSICBRAINZ_ALBUMID" directly.
        out.emplace(qstr(it->first).toUpper(), std::move(values));
    }
    return out;
}

QString AudioTagIo::readField(
    const std::map<QString, QStringList>& tags,
    const QString& key,
    const QString& fallback)
{
    const auto it = tags.find(key.toUpper());
    if (it == tags.end() || it->second.isEmpty()) return fallback;
    return it->second.first();
}

double AudioTagIo::readDurationSec(const QString& path) {
    TagLib::FileRef fr(path.toUtf8().constData(),
                       true,
                       TagLib::AudioProperties::Fast);
    if (fr.isNull() || !fr.audioProperties()) return 0.0;
    // lengthInMilliseconds gives sub-second accuracy where TagLib has
    // it; lengthInSeconds is the int floor.
    const int ms = fr.audioProperties()->lengthInMilliseconds();
    return ms > 0 ? double(ms) / 1000.0
                  : double(fr.audioProperties()->lengthInSeconds());
}

bool AudioTagIo::write(const QString& path,
                       const std::map<QString, QStringList>& tags)
{
    const Format f = detect(path);
    // FLAC + OGG Vorbis (and OGG-encapsulated FLAC) share the Vorbis
    // comment block as their tag surface — TagLib lets us setProperties
    // on the concrete FLAC::File. The other formats stub out for now.
    if (f != Format::Flac && f != Format::Ogg) {
        qWarning() << "AudioTagIo::write not yet implemented for format"
                   << formatName(f) << "path:" << path;
        return false;
    }

    TagLib::FileRef fr(path.toUtf8().constData(),
                       false,    // no audio-property scan needed for write
                       TagLib::AudioProperties::Fast);
    if (fr.isNull() || !fr.file()) {
        qWarning() << "AudioTagIo::write: cannot open" << path;
        return false;
    }

    TagLib::PropertyMap pm;
    for (const auto& kv : tags) {
        TagLib::StringList values;
        for (const QString& v : kv.second) values.append(tstr(v));
        pm.insert(tstr(kv.first.toUpper()), values);
    }
    fr.file()->setProperties(pm);
    return fr.save();
}

} // namespace concerto::library
