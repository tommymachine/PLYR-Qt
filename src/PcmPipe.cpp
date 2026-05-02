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
    // Emit while still holding the lock? Risky — receivers might re-enter.
    // Emit after releasing — but m_buf could still change under us.
    // Safe-enough: copy the consumed slice first, release, emit with
    // the local copy's lifetime. Since `out` is caller-provided, just
    // emit with it; the receiver must finish synchronously.
    lk.unlock();

    // EQ post-process in place, before the FFT tap sees the samples so the
    // visualizer reflects what's audible. Bypassed EQ is a fast memcpy-skip.
    if (m_eq && m_numChannels > 0) {
        const size_t frameBytes = sizeof(float) * size_t(m_numChannels);
        const size_t nframes    = size_t(n) / frameBytes;
        float* samples = reinterpret_cast<float*>(out);
        eq_process(m_eq, samples, samples, nframes);
    }

    emit samplesServed(out, n);
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
