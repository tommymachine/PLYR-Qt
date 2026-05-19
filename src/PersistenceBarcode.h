// PersistenceBarcode -- Layer 4a renderer. Stacked horizontal bars,
// one per persistence pair in the latest barcode published by
// PersistentHomologyAnalyzer.
//
// Each bar runs from x = birth*scale to x = death*scale at some
// y-coordinate determined by sort order (H0 below, H1 above, longest
// bars at top). H0 bars are muted gray; H1 bars are bright cyan --
// they're the interesting ones (1-D holes = musical loops).
//
// Threading: the analyzer's barcodeUpdated signal arrives on the
// analyzer's owning thread (the GUI thread, since QML instantiates it
// there). paint() reads the latest snapshot via
// PersistentHomologyAnalyzer::snapshotPairs() under the analyzer's
// internal mutex. No allocations on the paint path: the sorted list
// of bars is held in a reusable member vector.

#pragma once

#include "PersistentHomologyAnalyzer.h"

#include <QColor>
#include <QPointer>
#include <QQuickPaintedItem>
#include <qqmlregistration.h>
#include <vector>

class PersistenceBarcode : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(PersistentHomologyAnalyzer* analyzer READ analyzer
               WRITE setAnalyzer NOTIFY analyzerChanged)
    Q_PROPERTY(float maxRadius READ maxRadius WRITE setMaxRadius
               NOTIFY maxRadiusChanged)
    Q_PROPERTY(QColor h0Color READ h0Color WRITE setH0Color
               NOTIFY h0ColorChanged)
    Q_PROPERTY(QColor h1Color READ h1Color WRITE setH1Color
               NOTIFY h1ColorChanged)
    Q_PROPERTY(QColor h2Color READ h2Color WRITE setH2Color
               NOTIFY h2ColorChanged)
    Q_PROPERTY(QColor gridColor READ gridColor WRITE setGridColor
               NOTIFY gridColorChanged)
    Q_PROPERTY(QColor labelColor READ labelColor WRITE setLabelColor
               NOTIFY labelColorChanged)
    // Filter bars whose persistence (death - birth) is below this
    // threshold. Default 0.0 = render everything. The verification
    // CLI prefers 0.0; the live UI prefers ~0.05 to suppress the
    // mountain of near-zero "noise" pairs.
    Q_PROPERTY(float minPersistence READ minPersistence WRITE setMinPersistence
               NOTIFY minPersistenceChanged)
    // When true, scale the displayed x-axis to the analyzer's last
    // threshold (so the rightmost edge always lines up with the
    // filtration ceiling). When false, use maxRadius directly.
    Q_PROPERTY(bool autoScaleRadius READ autoScaleRadius WRITE setAutoScaleRadius
               NOTIFY autoScaleRadiusChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)

public:
    explicit PersistenceBarcode(QQuickItem* parent = nullptr);
    ~PersistenceBarcode() override;

    PersistentHomologyAnalyzer* analyzer() const { return m_analyzer.data(); }
    void setAnalyzer(PersistentHomologyAnalyzer* a);

    float maxRadius() const { return m_maxRadius; }
    void  setMaxRadius(float v);

    QColor h0Color() const { return m_h0Color; }
    void   setH0Color(const QColor& c);
    QColor h1Color() const { return m_h1Color; }
    void   setH1Color(const QColor& c);
    QColor h2Color() const { return m_h2Color; }
    void   setH2Color(const QColor& c);
    QColor gridColor() const { return m_gridColor; }
    void   setGridColor(const QColor& c);
    QColor labelColor() const { return m_labelColor; }
    void   setLabelColor(const QColor& c);

    float minPersistence() const { return m_minPersistence; }
    void  setMinPersistence(float v);

    bool autoScaleRadius() const { return m_autoScale; }
    void setAutoScaleRadius(bool v);

    bool active() const { return m_active; }
    void setActive(bool a);

    void paint(QPainter* painter) override;

signals:
    void analyzerChanged();
    void maxRadiusChanged();
    void h0ColorChanged();
    void h1ColorChanged();
    void h2ColorChanged();
    void gridColorChanged();
    void labelColorChanged();
    void minPersistenceChanged();
    void autoScaleRadiusChanged();
    void activeChanged();

private slots:
    void onBarcodeUpdated();

private:
    QPointer<PersistentHomologyAnalyzer> m_analyzer;

    float  m_maxRadius      = 2.0f;
    QColor m_h0Color        = QColor(0x88, 0x88, 0x88, 0xC8);
    QColor m_h1Color        = QColor(0x33, 0xC8, 0xFF, 0xFF);
    QColor m_h2Color        = QColor(0xFF, 0xC4, 0x33, 0xFF);   // gold for H2 (if requested)
    QColor m_gridColor      = QColor(0xFF, 0xFF, 0xFF, 0x18);
    QColor m_labelColor     = QColor(0xFF, 0xFF, 0xFF, 0x6E);
    float  m_minPersistence = 0.0f;
    bool   m_autoScale      = true;
    bool   m_active         = true;

    // Sorted bar list: pre-allocated so paint() doesn't allocate.
    QVector<PersistentHomologyAnalyzer::PersistencePair> m_sorted;
};
