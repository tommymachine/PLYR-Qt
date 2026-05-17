#include "AudioFrameHash.h"

#include <QAudioBuffer>
#include <QAudioDecoder>
#include <QAudioFormat>
#include <QCryptographicHash>
#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QObject>
#include <QTimer>
#include <QUrl>

namespace concerto::library {

QString AudioFrameHash::compute(const QString& path, int firstSeconds)
{
    if (path.isEmpty() || !QFileInfo::exists(path)) return {};
    if (firstSeconds <= 0) firstSeconds = 30;

    // Use QAudioDecoder for format-agnostic decode. We don't care what
    // the source codec is — what we hash is the PCM the player would
    // actually feed to QAudioSink.
    QAudioDecoder decoder;

    // Force a deterministic output layout (44.1 kHz / stereo / int16)
    // so the hash is stable across decoder backend changes. Qt picks
    // the source rate by default; pinning the output keeps tag-edit
    // round-trips from changing the hash on us.
    QAudioFormat fmt;
    fmt.setSampleRate(44100);
    fmt.setChannelCount(2);
    fmt.setSampleFormat(QAudioFormat::Int16);
    decoder.setAudioFormat(fmt);

    decoder.setSource(QUrl::fromLocalFile(path));

    QCryptographicHash hash(QCryptographicHash::Sha256);
    const qint64 byteCap =
        qint64(firstSeconds) * fmt.sampleRate() * fmt.bytesPerFrame();
    qint64 bytesHashed = 0;
    bool finished = false;
    bool errored  = false;

    QEventLoop loop;
    QObject::connect(&decoder, &QAudioDecoder::bufferReady,
        &loop, [&]() {
            while (decoder.bufferAvailable() && bytesHashed < byteCap) {
                const QAudioBuffer buf = decoder.read();
                if (!buf.isValid()) continue;
                const qint64 bytesLeft = byteCap - bytesHashed;
                const qint64 bytesTake = std::min<qint64>(buf.byteCount(),
                                                          bytesLeft);
                // QAudioBuffer's public data accessor is the typed
                // template constData<T>(); the raw void* overload is
                // private. char is byte-equivalent for raw PCM bytes.
                hash.addData(QByteArrayView(buf.constData<char>(),
                                            bytesTake));
                bytesHashed += bytesTake;
            }
            if (bytesHashed >= byteCap) {
                decoder.stop();
                loop.quit();
            }
        });
    QObject::connect(&decoder, &QAudioDecoder::finished,
        &loop, [&]() { finished = true; loop.quit(); });
    QObject::connect(&decoder,
        QOverload<QAudioDecoder::Error>::of(&QAudioDecoder::error),
        &loop, [&](QAudioDecoder::Error e) {
            qWarning() << "AudioFrameHash: decoder error" << e
                       << "path:" << path;
            errored = true;
            loop.quit();
        });

    // Belt-and-braces timeout — 30s of PCM should decode well under 5s
    // even for heavy MP3, but a hung backend shouldn't stall the CLI.
    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, &loop, [&]() {
        qWarning() << "AudioFrameHash: timeout decoding" << path;
        errored = true;
        loop.quit();
    });
    guard.start(10000);

    decoder.start();
    loop.exec();
    guard.stop();

    if (errored || bytesHashed == 0) return {};
    // It's fine if the file is shorter than `firstSeconds` — we just
    // hash whatever we got. The full-decode case still produces a
    // stable hash because (a) byteCap is the same upper bound for any
    // re-run, and (b) any "tag-edit round-trip" produces the same PCM
    // bytes back, since tags don't change the audio frames.
    Q_UNUSED(finished);
    return QString::fromLatin1(hash.result().toHex());
}

} // namespace concerto::library
