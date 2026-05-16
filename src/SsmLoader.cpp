#include "SsmLoader.h"
#include "SsmGenerator.h"

#include <QFileInfo>
#include <QUrl>

#include <cstring>


SsmLoader::SsmLoader(QObject* parent) : QObject(parent) {}
SsmLoader::~SsmLoader() = default;


void SsmLoader::setSourceFile(const QString& path)
{
    // Accept either a filesystem path or a file:// URL. QML often hands us
    // QUrl-stringified paths because audio.position bindings use URLs.
    QString resolved = path;
    if (resolved.startsWith(QStringLiteral("file://"))) {
        resolved = QUrl(resolved).toLocalFile();
    }

    if (resolved == m_sourceFile) return;
    m_sourceFile = resolved;
    emit sourceFileChanged();

    clear();
    if (resolved.isEmpty()) {
        emit loadedChanged();
        return;
    }

    if (!QFileInfo(resolved).exists()) {
        m_error = QStringLiteral("file not found: %1").arg(resolved);
        emit loadedChanged();
        return;
    }

    SsmGenerator::SidecarHeader hdr;
    if (!SsmGenerator::readSidecar(resolved, hdr, m_matrix)) {
        m_error = QStringLiteral("invalid SSM sidecar: %1").arg(resolved);
        m_matrix.clear();
        emit loadedChanged();
        return;
    }

    m_dim    = int(hdr.T);
    m_hopSec = hdr.hopSec;
    m_loaded = true;
    m_error.clear();
    emit loadedChanged();
}


void SsmLoader::clear()
{
    m_loaded = false;
    m_dim    = 0;
    m_hopSec = 0.0f;
    m_matrix.clear();
    m_matrix.shrink_to_fit();
    m_error.clear();
}


int SsmLoader::cellAt(int i, int j) const
{
    if (!m_loaded) return 0;
    if (i < 0 || i >= m_dim || j < 0 || j >= m_dim) return 0;
    return int(m_matrix[size_t(i) * size_t(m_dim) + size_t(j)]);
}


QByteArray SsmLoader::rowBytes(int i) const
{
    if (!m_loaded || i < 0 || i >= m_dim) return QByteArray();
    QByteArray out(m_dim, Qt::Uninitialized);
    std::memcpy(out.data(),
                &m_matrix[size_t(i) * size_t(m_dim)],
                size_t(m_dim));
    return out;
}


QByteArray SsmLoader::matrixBytes() const
{
    if (!m_loaded) return QByteArray();
    return QByteArray(reinterpret_cast<const char*>(m_matrix.data()),
                      qsizetype(m_matrix.size()));
}
