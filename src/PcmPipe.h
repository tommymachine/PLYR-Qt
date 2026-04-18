// QIODevice that buffers an entire decoded track's PCM (appended by a
// QAudioDecoder) and streams it to a QAudioSink on demand. Emits a
// `samplesServed` signal on every read so the FFT processor can tap the
// audio right as it's being played.
//
// The pipe is thread-safe across the usual split:
//   - main/decoder thread calls `append(...)` as buffers decode
//   - audio thread calls `readData(...)` via QAudioSink

#pragma once

#include <QByteArray>
#include <QIODevice>
#include <QMutex>

class PcmPipe : public QIODevice {
    Q_OBJECT
public:
    explicit PcmPipe(QObject* parent = nullptr);

    // Append decoded PCM bytes. Thread-safe.
    void append(const QByteArray& data);

    // Discard everything (for a new track or a hard reset).
    void clearAll();

    // Total bytes currently buffered (== what the decoder has produced).
    qint64 totalSize() const;

    // Current playhead position in bytes (== what the sink has consumed).
    qint64 readPos() const;

    // Move the playhead to `pos` bytes (caller ensures frame alignment).
    // Returns false if the position is beyond what's been decoded.
    bool seekToPos(qint64 pos);

    // QIODevice
    qint64 bytesAvailable() const override;

signals:
    // Emitted whenever the sink pulls audio out of the pipe. Only valid
    // during the call. Copy if you need it longer.
    void samplesServed(const char* data, qint64 bytes);

protected:
    qint64 readData(char* out, qint64 maxLen) override;
    qint64 writeData(const char*, qint64) override { return -1; }

private:
    mutable QMutex m_mutex;
    QByteArray     m_buf;
    qint64         m_pos = 0;
};
