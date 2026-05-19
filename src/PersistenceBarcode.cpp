#include "PersistenceBarcode.h"

#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QPen>
#include <QBrush>
#include <QRectF>

#include <algorithm>
#include <cmath>
#include <limits>


PersistenceBarcode::PersistenceBarcode(QQuickItem* parent)
    : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
    setOpaquePainting(false);
    m_sorted.reserve(256);
}


PersistenceBarcode::~PersistenceBarcode() = default;


// --- Property setters -----------------------------------------------------

void PersistenceBarcode::setAnalyzer(PersistentHomologyAnalyzer* a)
{
    if (m_analyzer.data() == a) return;
    if (m_analyzer) {
        disconnect(m_analyzer.data(), nullptr, this, nullptr);
    }
    m_analyzer = a;
    if (m_analyzer) {
        connect(m_analyzer.data(),
                &PersistentHomologyAnalyzer::barcodeUpdated,
                this, &PersistenceBarcode::onBarcodeUpdated,
                Qt::QueuedConnection);
    }
    emit analyzerChanged();
    update();
}


void PersistenceBarcode::setMaxRadius(float v)
{
    if (v <= 0.0f) v = 0.001f;
    if (std::fabs(v - m_maxRadius) < 1e-6f) return;
    m_maxRadius = v;
    emit maxRadiusChanged();
    update();
}


void PersistenceBarcode::setH0Color(const QColor& c)
{
    if (c == m_h0Color) return;
    m_h0Color = c;
    emit h0ColorChanged();
    update();
}


void PersistenceBarcode::setH1Color(const QColor& c)
{
    if (c == m_h1Color) return;
    m_h1Color = c;
    emit h1ColorChanged();
    update();
}


void PersistenceBarcode::setH2Color(const QColor& c)
{
    if (c == m_h2Color) return;
    m_h2Color = c;
    emit h2ColorChanged();
    update();
}


void PersistenceBarcode::setGridColor(const QColor& c)
{
    if (c == m_gridColor) return;
    m_gridColor = c;
    emit gridColorChanged();
    update();
}


void PersistenceBarcode::setLabelColor(const QColor& c)
{
    if (c == m_labelColor) return;
    m_labelColor = c;
    emit labelColorChanged();
    update();
}


void PersistenceBarcode::setMinPersistence(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (std::fabs(v - m_minPersistence) < 1e-6f) return;
    m_minPersistence = v;
    emit minPersistenceChanged();
    update();
}


void PersistenceBarcode::setAutoScaleRadius(bool v)
{
    if (v == m_autoScale) return;
    m_autoScale = v;
    emit autoScaleRadiusChanged();
    update();
}


void PersistenceBarcode::setActive(bool a)
{
    if (m_active == a) return;
    m_active = a;
    emit activeChanged();
}


// --- Hooks ----------------------------------------------------------------

void PersistenceBarcode::onBarcodeUpdated()
{
    if (!m_active) return;
    update();
}


// --- Paint ----------------------------------------------------------------

void PersistenceBarcode::paint(QPainter* painter)
{
    if (!painter) return;
    const QRectF rect(0.0, 0.0, width(), height());
    if (rect.width() < 4 || rect.height() < 4) return;

    painter->setRenderHint(QPainter::Antialiasing, true);

    // Snapshot pairs + diagnostics. Snapshot is cheap (QVector copy of
    // a few hundred floats) and runs on the GUI thread; the analyzer's
    // worker writer is fully released by the time barcodeUpdated()
    // arrives at the queued-connection slot that triggered this paint.
    m_sorted.clear();
    if (!m_analyzer) return;
    const QVector<PersistentHomologyAnalyzer::PersistencePair> pairs =
        m_analyzer->snapshotPairs();

    // Pick the x-axis ceiling. autoScale uses the analyzer's last
    // threshold (the longest possible finite death we'd see from this
    // run); otherwise honor the explicit maxRadius property.
    float xCeil = m_maxRadius;
    if (m_autoScale) {
        const float t = m_analyzer->lastThreshold();
        if (t > 0.0f) xCeil = t;
    }
    if (xCeil <= 0.0f) xCeil = 1.0f;

    // Filter + clamp.
    for (const auto& p : pairs) {
        // Clamp infinite deaths to the threshold (visual ceiling).
        float d = p.death;
        if (!std::isfinite(d)) d = xCeil;
        const float length = d - p.birth;
        if (length < m_minPersistence) continue;
        PersistentHomologyAnalyzer::PersistencePair q = p;
        q.death = d;
        m_sorted.append(q);
    }

    // Sort: dim ascending (H0 first), then persistence descending so the
    // longest bars sit at the top of each band.
    std::sort(m_sorted.begin(), m_sorted.end(),
              [](const auto& a, const auto& b) {
                  if (a.dim != b.dim) return a.dim < b.dim;
                  return (a.death - a.birth) > (b.death - b.birth);
              });

    // Layout. Two horizontal sections:
    //   * H0 along the bottom 40% of the item.
    //   * H1+ stacked above. We currently render dim <= 2.
    // Each bar is a thin filled rect. Y-step is computed so the
    // bars fit; min 1.5 px per row so the count cap is implicit.
    const qreal padL = 6.0;
    const qreal padR = 6.0;
    const qreal padTop = 14.0;
    const qreal padBot = 18.0;
    const qreal innerW = std::max(4.0, rect.width() - padL - padR);
    const qreal innerH = std::max(4.0, rect.height() - padTop - padBot);

    // Faint backdrop frame + grid.
    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(0, 0, 0, 80));
    painter->drawRoundedRect(QRectF(padL - 4, padTop - 4,
                                    innerW + 8, innerH + 8),
                             4.0, 4.0);

    // Vertical grid lines at 25/50/75% of the x-axis.
    {
        QPen gridPen(m_gridColor);
        gridPen.setWidthF(0.6);
        painter->setPen(gridPen);
        for (int g = 1; g <= 3; ++g) {
            const qreal x = padL + innerW * (qreal(g) * 0.25);
            painter->drawLine(QPointF(x, padTop),
                              QPointF(x, padTop + innerH));
        }
    }

    // Split horizontal band into H0 (bottom 40%) and H1+ (top 60%).
    // Pre-pass: count visible bars per dim so we can size their y-step.
    int countByDim[3] = {0, 0, 0};
    for (const auto& p : m_sorted) {
        if (p.dim >= 0 && p.dim < 3) countByDim[p.dim]++;
    }

    const qreal h0BandTop    = padTop + innerH * 0.60;
    const qreal h0BandBottom = padTop + innerH;
    const qreal h0BandH      = h0BandBottom - h0BandTop;

    const qreal h1BandTop    = padTop;
    const qreal h1BandBottom = h0BandTop - 2.0;
    const qreal h1BandH      = h1BandBottom - h1BandTop;

    auto colorFor = [&](int dim) -> QColor {
        if (dim == 0) return m_h0Color;
        if (dim == 1) return m_h1Color;
        return m_h2Color;
    };

    // Y position per bar within its dim's band.
    int idxByDim[3] = {0, 0, 0};

    auto barRect = [&](float birth, float death, int dim, int idx) -> QRectF {
        const float bClamp = std::clamp(birth, 0.0f, xCeil);
        const float dClamp = std::clamp(death, 0.0f, xCeil);
        const qreal x0 = padL + innerW * qreal(bClamp / xCeil);
        const qreal x1 = padL + innerW * qreal(dClamp / xCeil);
        const qreal w  = std::max(1.0, x1 - x0);

        qreal y;
        if (dim == 0) {
            const int n = std::max(1, countByDim[0]);
            const qreal step = std::min(qreal(4.0), h0BandH / qreal(n + 1));
            y = h0BandBottom - step * (idx + 1);
        } else {
            // H1+ stacked above with bigger bars for emphasis.
            const int n = std::max(1, countByDim[1] + countByDim[2]);
            const qreal step = std::min(qreal(5.5), h1BandH / qreal(n + 1));
            // For H2 (idx counted separately) offset by H1 count.
            const int linear = (dim == 1) ? idx : (countByDim[1] + idx);
            y = h1BandBottom - step * (linear + 1);
        }
        const qreal barH = (dim == 0) ? 2.5 : 4.0;
        return QRectF(x0, y - barH * 0.5, w, barH);
    };

    painter->setPen(Qt::NoPen);
    for (const auto& p : m_sorted) {
        if (p.dim < 0 || p.dim > 2) continue;
        const int idx = idxByDim[p.dim]++;
        const QColor c = colorFor(p.dim);
        const QRectF r = barRect(p.birth, p.death, p.dim, idx);
        painter->setBrush(c);
        painter->drawRoundedRect(r, 1.2, 1.2);
    }

    // Axis labels (filtration radius). Iosevka 8pt.
    QFont monoFont("Iosevka");
    monoFont.setPixelSize(8);
    painter->setFont(monoFont);
    QFontMetrics fm(monoFont);
    painter->setPen(m_labelColor);

    // Tick labels at 0, 25, 50, 75, 100% of xCeil.
    for (int g = 0; g <= 4; ++g) {
        const qreal x = padL + innerW * (qreal(g) * 0.25);
        const float r = xCeil * (float(g) * 0.25f);
        const QString s = QString::number(double(r), 'f', 2);
        const int    tw = fm.horizontalAdvance(s);
        qreal lx = x - tw * 0.5;
        if (g == 0) lx = padL;
        if (g == 4) lx = padL + innerW - tw;
        painter->drawText(QPointF(lx, padTop + innerH + 10), s);
    }

    // Section labels.
    if (countByDim[1] > 0) {
        painter->setPen(QColor(m_h1Color.red(), m_h1Color.green(),
                               m_h1Color.blue(), 0xC0));
        painter->drawText(QPointF(padL, padTop + 8), QStringLiteral("H1"));
    }
    if (countByDim[0] > 0) {
        painter->setPen(QColor(m_h0Color.red(), m_h0Color.green(),
                               m_h0Color.blue(), 0xC0));
        painter->drawText(QPointF(padL, h0BandTop + 6), QStringLiteral("H0"));
    }
}
