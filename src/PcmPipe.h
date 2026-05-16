// QIODevice that buffers an entire decoded track's PCM (appended by a
// QAudioDecoder) and streams it to a QAudioSink on demand. Emits a
// `samplesServed` signal on every read for diagnostic taps.
//
// For visualizer/spectrum work, prefer the `peek(offset, ...)` interface:
// it samples *ahead* of the playhead so the FFT depicts audio that is
// about to be heard rather than audio that just landed in the sink's
// period buffer (see AudioWorker's lookahead tap and AudioEngine's
// lookaheadMs property).
//
// The pipe is thread-safe across the usual split:
//   - main/decoder thread calls `append(...)` as buffers decode
//   - audio thread calls `readData(...)` via QAudioSink
//   - audio thread calls `peek(...)` from the lookahead tick

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

    // Read-only tap: copy up to `maxLen` bytes starting `offset` bytes
    // ahead of the current playhead (`m_pos + offset`) into `out`,
    // WITHOUT advancing the playhead and WITHOUT emitting `samplesServed`.
    //
    // Returns the number of bytes actually copied. May be less than
    // `maxLen` if the decoder hasn't produced enough data yet — that's a
    // soft condition, not an error: the caller should push what's
    // available and try again next tick. Returns 0 if `offset` is past
    // the end of currently-buffered data (e.g. end-of-track).
    //
    // Used by the visualizer's lookahead tap so the FFT/AudioFeatures
    // reflect audio that is about to be heard, not audio that just
    // finished being copied into the sink's period buffer. Bytes are
    // returned PRE-EQ (lock is held during memcpy; EQ in readData() runs
    // post-lock). This is an accepted tradeoff — the alternative would
    // be running the EQ on the peek path too, which doubles the DSP
    // cost just to colour the spectrum. The visible drift is dominated
    // by sink latency, not by EQ shape.
    //
    // Thread-safe: takes the same lock as readData(). The held region
    // is a bounded memcpy.
    qint64 peek(qint64 offset, char* out, qint64 maxLen) const;

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
