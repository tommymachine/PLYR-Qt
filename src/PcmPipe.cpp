#include "PcmPipe.h"

#include <QMutexLocker>
#include <cstring>

PcmPipe::PcmPipe(QObject* parent)
    : QIODevice(parent)
{
    open(QIODevice::ReadOnly);
}

void PcmPipe::append(const QByteArray& data)
{
    {
        QMutexLocker lk(&m_mutex);
        m_buf.append(data);
    }
    emit readyRead();
}

void PcmPipe::clearAll()
{
    QMutexLocker lk(&m_mutex);
    m_buf.clear();
    m_pos = 0;
    m_underrunCount = 0;
    m_underrunBytes = 0;
    m_totalServed   = 0;
}

qint64 PcmPipe::totalSize() const
{
    QMutexLocker lk(&m_mutex);
    return m_buf.size();
}

qint64 PcmPipe::readPos() const
{
    QMutexLocker lk(&m_mutex);
    return m_pos;
}

bool PcmPipe::seekToPos(qint64 pos)
{
    QMutexLocker lk(&m_mutex);
    if (pos < 0 || pos > m_buf.size()) return false;
    m_pos = pos;
    return true;
}

qint64 PcmPipe::bytesAvailable() const
{
    QMutexLocker lk(&m_mutex);
    return (m_buf.size() - m_pos) + QIODevice::bytesAvailable();
}

qint64 PcmPipe::peek(qint64 offset, char* out, qint64 maxLen) const
{
    if (!out || maxLen <= 0) return 0;
    QMutexLocker lk(&m_mutex);
    // Negative offsets are legal — they let the visualizer FFT tap
    // samples that Qt's sink already pulled from the pipe but that
    // are still sitting in QAudioSink's internal ringbuffer waiting
    // to be handed to the AURenderCallback. We don't free already-
    // read bytes from m_buf (the whole decoded track stays resident),
    // so the index math only needs the bounds check below.
    const qint64 start = m_pos + offset;
    if (start < 0) return 0;                     // before the start of decoded data
    if (start >= m_buf.size()) return 0;         // beyond decoded tail
    const qint64 avail = m_buf.size() - start;
    const qint64 n = qMin(maxLen, avail);
    if (n <= 0) return 0;
    std::memcpy(out, m_buf.constData() + start, size_t(n));
    return n;
}

qint64 PcmPipe::readData(char* out, qint64 maxLen)
{
    QMutexLocker lk(&m_mutex);
    const qint64 avail = m_buf.size() - m_pos;
    const qint64 n = qMin(maxLen, avail);

    // Underrun accounting: the sink asked for maxLen bytes; if we gave
    // fewer, that's a shortfall that causes the sink to wait. For a
    // perfectly gapless chain, this should stay at 0.
    if (n < maxLen) {
        ++m_underrunCount;
        m_underrunBytes += (maxLen - n);
    }

    if (n <= 0) return 0;  // sequential pipe: "no data right now", not EOF
    std::memcpy(out, m_buf.constData() + m_pos, size_t(n));
    m_pos += n;
    m_totalServed += n;
    lk.unlock();

    // EQ post-process in place, so the audible output (and any future
    // tap into `out`) sees the EQ shape. Bypassed EQ is a fast no-op.
    if (m_eq && m_numChannels > 0) {
        const size_t frameBytes = sizeof(float) * size_t(m_numChannels);
        const size_t nframes    = size_t(n) / frameBytes;
        float* samples = reinterpret_cast<float*>(out);
        eq_process(m_eq, samples, samples, nframes);
    }

    return n;
}

qint64 PcmPipe::underrunCount() const
{
    QMutexLocker lk(&m_mutex);
    return m_underrunCount;
}

qint64 PcmPipe::underrunBytes() const
{
    QMutexLocker lk(&m_mutex);
    return m_underrunBytes;
}

qint64 PcmPipe::totalBytesServed() const
{
    QMutexLocker lk(&m_mutex);
    return m_totalServed;
}

void PcmPipe::resetDiagnostics()
{
    QMutexLocker lk(&m_mutex);
    m_underrunCount = 0;
    m_underrunBytes = 0;
    m_totalServed   = 0;
}
