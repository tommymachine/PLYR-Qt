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
    if (n <= 0) return 0;
    std::memcpy(out, m_buf.constData() + m_pos, size_t(n));
    m_pos += n;
    // Emit while still holding the lock? Risky — receivers might re-enter.
    // Emit after releasing — but m_buf could still change under us.
    // Safe-enough: copy the consumed slice first, release, emit with
    // the local copy's lifetime. Since `out` is caller-provided, just
    // emit with it; the receiver must finish synchronously.
    lk.unlock();
    emit samplesServed(out, n);
    return n;
}
