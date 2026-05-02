// QIODevice that buffers an entire decoded track's PCM (appended by a
// QAudioDecoder) and streams it to a QAudioSink on demand. Emits a
// `samplesServed` signal on every read so the FFT processor can tap the
// audio right as it's being played.
//
// The pipe is thread-safe across the usual split:
//   - main/decoder thread calls `append(...)` as buffers decode
//   - audio thread calls `readData(...)` via QAudioSink

#pragma once

#include "eq_engine.h"

#include <QByteArray>
#include <QIODevice>
#include <QMutex>

class PcmPipe : public QIODevice {
    Q_OBJECT
public:
    explicit PcmPipe(QObject* parent = nullptr);

    // Optional EQ post-process. Not owned; caller keeps the EqEngine alive
    // for the lifetime of the pipe. Setting either to null disables EQ.
    // Assumes interleaved float32 at the channel count set here.
    void setEqEngine        (EqEngine* eq) { m_eq = eq; }
    void setFloatFrameLayout(int numChannels) { m_numChannels = numChannels; }

    // Append decoded PCM bytes. Thread-safe.
    void append(const QByteArray& data);

    // Discard everything (for a new track or a hard reset).
    void clearAll();

    // Total bytes currently buffered (== what the decoder has produced).
    qint64 totalSize() const;

    // Current playhead position in bytes (== what the sink has consumed).
    qint64 readPos() const;

    // Diagnostics. An underrun is a readData() call that returned fewer
    // bytes than requested (possibly 0) — i.e. the decoder hadn't kept up
    // with the sink. For a gapless chain across tracks, this count should
    // stay at 0 from the first play() to the last trackEnded().
    qint64 underrunCount()   const;
    qint64 underrunBytes()   const;   // total bytes short across all reads
    qint64 totalBytesServed() const;  // cumulative bytes handed to the sink
    void   resetDiagnostics();

    // Move the playhead to `pos` bytes (caller ensures frame alignment).
    // Returns false if the position is beyond what's been decoded.
    bool seekToPos(qint64 pos);

    // QIODevice
    qint64 bytesAvailable() const override;
    // We are a streaming buffer: the sink pulls, we grow as the decoder
    // produces. Declaring this sequential tells Qt that "readData returned 0"
    // means "no data right now, wait for readyRead" — NOT end-of-stream.
    // Otherwise the sink goes Idle on every underrun and has to warm back
    // up, which audibly clicks at track boundaries.
    bool   isSequential() const override { return true; }

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

    // Diagnostics (see methods above).
    qint64 m_underrunCount  = 0;
    qint64 m_underrunBytes  = 0;
    qint64 m_totalServed    = 0;

    // EQ post-process (non-owning).
    EqEngine* m_eq           = nullptr;
    int       m_numChannels  = 0;
};
