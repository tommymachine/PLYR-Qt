#include "FlacTags.h"

#include <QFile>

namespace {

// Little-endian uint32 reader for Vorbis comments.
inline bool readU32LE(const QByteArray& b, int pos, quint32& out) {
    if (pos + 4 > b.size()) return false;
    const auto* u = reinterpret_cast<const unsigned char*>(b.constData());
    out = quint32(u[pos])
        | (quint32(u[pos + 1]) <<  8)
        | (quint32(u[pos + 2]) << 16)
        | (quint32(u[pos + 3]) << 24);
    return true;
}

} // namespace


std::optional<FlacTags> FlacTags::read(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return std::nullopt;

    // Magic: "fLaC"
    const auto magic = f.read(4);
    if (magic.size() != 4
        || static_cast<unsigned char>(magic[0]) != 0x66
        || static_cast<unsigned char>(magic[1]) != 0x4C
        || static_cast<unsigned char>(magic[2]) != 0x61
        || static_cast<unsigned char>(magic[3]) != 0x43) {
        return std::nullopt;
    }

    FlacTags out;

    // Walk metadata blocks until the last-flag is set.
    while (true) {
        const auto hdr = f.read(4);
        if (hdr.size() != 4) break;

        const auto bytes = reinterpret_cast<const unsigned char*>(hdr.constData());
        const bool isLast = (bytes[0] & 0x80) != 0;
        const int  type   = bytes[0] & 0x7F;
        const int  length = (int(bytes[1]) << 16) | (int(bytes[2]) << 8) | int(bytes[3]);

        if (type == 4) {            // VORBIS_COMMENT
            const auto block = f.read(length);
            if (block.size() == length) parseVorbisComment(block, out);
        } else if (type == 0) {     // STREAMINFO
            const auto block = f.read(length);
            if (block.size() == length) parseStreamInfo(block, out);
        } else {
            // Skip unknown blocks (PADDING, SEEKTABLE, PICTURE, etc.)
            f.seek(f.pos() + length);
        }

        if (isLast) break;
    }
    return out;
}


void FlacTags::parseStreamInfo(const QByteArray& data, FlacTags& out)
{
    if (data.size() < 18) return;
    const auto* b = reinterpret_cast<const unsigned char*>(data.constData());

    // 20 bits of sample rate starting at byte 10 (big-endian bitstream).
    const int sampleRate =
        (int(b[10]) << 12) | (int(b[11]) << 4) | (int(b[12]) >> 4);
    if (sampleRate <= 0) return;

    // 36 bits of total-sample count: low 4 bits of b[13] then b[14..17].
    const quint64 totalSamples =
          (quint64(b[13] & 0x0F) << 32)
        | (quint64(b[14])        << 24)
        | (quint64(b[15])        << 16)
        | (quint64(b[16])        <<  8)
        |  quint64(b[17]);

    out.duration = double(totalSamples) / double(sampleRate);
}


void FlacTags::parseVorbisComment(const QByteArray& data, FlacTags& out)
{
    int pos = 0;

    // Vendor string — skip.
    quint32 vendorLen = 0;
    if (!readU32LE(data, pos, vendorLen)) return;
    pos += 4;
    pos += int(vendorLen);

    // Number of user comments.
    quint32 count = 0;
    if (!readU32LE(data, pos, count)) return;
    pos += 4;

    for (quint32 i = 0; i < count; ++i) {
        quint32 len = 0;
        if (!readU32LE(data, pos, len)) return;
        pos += 4;
        const int end = pos + int(len);
        if (end > data.size()) return;

        const QString raw = QString::fromUtf8(data.constData() + pos, int(len));
        pos = end;

        const int eq = raw.indexOf(QLatin1Char('='));
        if (eq <= 0) continue;

        out.tags.insert(raw.left(eq).toUpper(), raw.mid(eq + 1));
    }
}
