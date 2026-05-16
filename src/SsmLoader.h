// SsmLoader -- loads a .ssm sidecar produced by SsmGenerator into memory
// and exposes it for QML rendering.
//
// The matrix is immutable once loaded. cellAt() / rowBytes() / matrixBytes()
// are safe to call from any thread without locking: they read into a
// std::vector that's never written to after setSourceFile() returns.
// (setSourceFile() itself is GUI-thread only.)
//
// QML usage:
//   SsmLoader { id: loader; sourceFile: "/path/to/track.flac.ssm" }
//   ShaderEffect { property var src: ssmTex; ... }
//
// Render-time access through SsmTexture (which references the loaded
// bytes and uploads them to a QSGTexture) is the fast path; the
// Q_INVOKABLE accessors here are for JS-side scrubbing UIs that want to
// pull a single row.

#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <cstdint>
#include <qqmlregistration.h>
#include <vector>

class SsmLoader : public QObject {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QString sourceFile READ sourceFile WRITE setSourceFile
                                  NOTIFY sourceFileChanged)
    Q_PROPERTY(bool    loaded     READ loaded     NOTIFY loadedChanged)
    Q_PROPERTY(int     dim        READ dim        NOTIFY loadedChanged)
    Q_PROPERTY(float   hopSec     READ hopSec     NOTIFY loadedChanged)
    Q_PROPERTY(QString error      READ error      NOTIFY loadedChanged)

public:
    explicit SsmLoader(QObject* parent = nullptr);
    ~SsmLoader() override;

    QString sourceFile() const { return m_sourceFile; }
    void    setSourceFile(const QString& path);

    bool    loaded() const { return m_loaded; }
    int     dim()    const { return m_dim;    }
    float   hopSec() const { return m_hopSec; }
    QString error()  const { return m_error;  }

    // Get the byte at (i, j). i, j in [0, dim). Out-of-range returns 0.
    Q_INVOKABLE int cellAt(int i, int j) const;

    // Get one row as a QByteArray (length dim). Out-of-range returns
    // an empty QByteArray.
    Q_INVOKABLE QByteArray rowBytes(int i) const;

    // Get the full matrix as raw bytes (length dim * dim). Const-ref
    // accessor for the texture pump on the render thread; the underlying
    // vector is immutable once loaded.
    const std::vector<uint8_t>& matrixRef() const { return m_matrix; }
    QByteArray matrixBytes() const;

signals:
    void sourceFileChanged();
    void loadedChanged();

private:
    void clear();

    QString               m_sourceFile;
    bool                  m_loaded = false;
    int                   m_dim    = 0;
    float                 m_hopSec = 0.0f;
    QString               m_error;
    std::vector<uint8_t>  m_matrix;   // size dim*dim
};
