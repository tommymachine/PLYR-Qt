#include "TonnetzView.h"
#include "ChromaAnalyzer.h"

#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>


namespace {

// 12 pitch-class names. Sharps, never flats, per the task spec. Index ==
// pitch class (0 = C).
const std::array<const char*, 12> kPcNames = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

// Bonus added to the lit value when ALL THREE of a triad's vertices are
// in the top-K active set above the threshold. Encourages a triad that's
// fully present to outshine triads sharing two of its notes (e.g., C and
// Em both contain E+G; the bonus picks the one with the third vertex
// also lit).
constexpr float kAllActiveBonus = 0.2f;

// Maximum lit value (raw min + bonus) -- clamped here so additive blending
// can't push individual pixels above ~1.0 alpha and blow the gold/cyan
// glow into a colourless white smear.
constexpr float kLitMax = 1.0f;

// Padding around the lattice patch so vertex circles + labels stay
// inside the item rect.
constexpr qreal kPaintMargin = 18.0;

}  // namespace


TonnetzView::TonnetzView(QQuickItem* parent)
    : QQuickPaintedItem(parent)
{
    // The lattice is built lazily once we know our size. Antialiased
    // QPainter on a backing image -- the default QQuickPaintedItem render
    // target. setOpaquePainting(false) keeps alpha working through the
    // composition pipeline (we want to overlay on top of darker
    // visualizers without occluding them).
    setAntialiasing(true);
    setOpaquePainting(false);

    // Initialize chroma snapshot to zeros so the very first paint (before
    // any hop has fired) just shows the static lattice without lit
    // triangles -- a clean canvas instead of a flicker on startup.
    for (auto& v : m_lastChroma) v = 0.0f;
    for (auto& a : m_activeSet)  a = false;
}


TonnetzView::~TonnetzView() = default;


void TonnetzView::setChromaSource(ChromaAnalyzer* s)
{
    if (m_chromaSource == s) return;
    if (m_chromaSource) {
        disconnect(m_chromaSource, &ChromaAnalyzer::chromaUpdated,
                   this, &TonnetzView::onChromaUpdated);
    }
    m_chromaSource = s;
    if (m_chromaSource) {
        connect(m_chromaSource, &ChromaAnalyzer::chromaUpdated,
                this, &TonnetzView::onChromaUpdated,
                Qt::DirectConnection);
    }
    emit chromaSourceChanged();
}


void TonnetzView::setLitThreshold(float v)
{
    if (std::fabs(v - m_litThreshold) < 1e-4f) return;
    m_litThreshold = std::clamp(v, 0.0f, 1.0f);
    emit litThresholdChanged();
}


void TonnetzView::setTopK(int v)
{
    if (v < 1) v = 1; if (v > PITCH_CLASSES) v = PITCH_CLASSES;
    if (v == m_topK) return;
    m_topK = v;
    emit topKChanged();
}


void TonnetzView::setMajorColor(const QColor& c)
{
    if (c == m_majorColor) return;
    m_majorColor = c;
    emit majorColorChanged();
    update();
}


void TonnetzView::setMinorColor(const QColor& c)
{
    if (c == m_minorColor) return;
    m_minorColor = c;
    emit minorColorChanged();
    update();
}


void TonnetzView::setRestColor(const QColor& c)
{
    if (c == m_restColor) return;
    m_restColor = c;
    emit restColorChanged();
    update();
}


void TonnetzView::setLineColor(const QColor& c)
{
    if (c == m_lineColor) return;
    m_lineColor = c;
    emit lineColorChanged();
    update();
}


void TonnetzView::setLabelColor(const QColor& c)
{
    if (c == m_labelColor) return;
    m_labelColor = c;
    emit labelColorChanged();
    update();
}


void TonnetzView::setShowLabels(bool v)
{
    if (v == m_showLabels) return;
    m_showLabels = v;
    emit showLabelsChanged();
    update();
}


void TonnetzView::setShowAxes(bool v)
{
    if (v == m_showAxes) return;
    m_showAxes = v;
    emit showAxesChanged();
    update();
}


void TonnetzView::geometryChange(const QRectF& newG, const QRectF& oldG)
{
    QQuickPaintedItem::geometryChange(newG, oldG);
    // Resize -> rebuild the lattice so triangles tile the new area at
    // a sensible scale. Defer to next paint by simply invalidating;
    // recomputeLattice is cheap (<1 ms for ~40 triangles).
    if (newG.size() != oldG.size()) {
        rebuildLattice();
        update();
    }
}


// --- Lattice construction ----------------------------------------------------

void TonnetzView::rebuildLattice()
{
    m_triads.clear();
    m_vertices.clear();
    m_edges.clear();

    const qreal w = width();
    const qreal h = height();
    if (w <= 1.0 || h <= 1.0) return;

    // Choose a grid spacing so the lattice fills the available area with
    // roughly 6-8 vertices across the longer dimension. With equilateral
    // triangles each "row" (j+1) is dx * sqrt(3)/2 tall. Pick dx from the
    // available width first, then verify the height fits; clamp upward
    // if not.
    const qreal availW = w - 2 * kPaintMargin;
    const qreal availH = h - 2 * kPaintMargin;
    if (availW <= 0.0 || availH <= 0.0) return;

    // i ranges over [0, iMax]; with j-shear the x coverage is
    // (iMax * dx) + (jMax * dx / 2). Aim for iMax ~ 5-6 across the width.
    constexpr int kITargetCells = 5;
    const qreal dxByWidth  = availW / qreal(kITargetCells + 1.5);
    // j ranges over [0, jMax]; total height is jMax * dx * sqrt(3)/2.
    // Aim for jMax ~ 3-4 so we get major+minor triads across each row.
    constexpr int kJTargetCells = 3;
    const qreal dxByHeight = (availH / (qreal(kJTargetCells + 0.5)))
                             / (std::sqrt(3.0) * 0.5);
    const qreal dx = std::min(dxByWidth, dxByHeight);
    if (dx < 8.0) return;   // item is too tiny; skip silently

    const qreal dy = dx * std::sqrt(3.0) * 0.5;
    const int   iMax = std::max(3, int(std::floor(availW / dx)) - 1);
    const int   jMax = std::max(2, int(std::floor(availH / dy)) - 1);

    // Origin so the patch is roughly centered horizontally. The j-shear
    // adds j*dx/2 to x, so the natural left edge at j=jMax is jMax*dx/2
    // farther right than at j=0. Pick the x offset so the rightmost
    // vertex still fits inside the margin.
    const qreal rowShear = jMax * dx * 0.5;
    const qreal usedW    = iMax * dx + rowShear;
    const qreal originX  = kPaintMargin + (availW - usedW) * 0.5;
    // Y origin: place j=0 near the BOTTOM of the available area so j+
    // goes UP visually (musicians read M3 stacks going up the page).
    const qreal originY  = h - kPaintMargin;

    // Allocate vertex array. Index = i + (iMax+1) * j.
    const int iCount = iMax + 1;
    const int jCount = jMax + 1;
    m_vertices.resize(iCount * jCount);
    auto vidx = [iCount](int i, int j) { return i + iCount * j; };

    for (int j = 0; j < jCount; ++j) {
        for (int i = 0; i < iCount; ++i) {
            // PC formula: P5 axis is (i, 0) -> +7 mod 12; M3 axis is
            // (0, j) -> +4 mod 12. So PC at (i, j) = (7i + 4j) mod 12.
            const int pc = ((i * 7 + j * 4) % 12 + 12) % 12;
            VertexNode v;
            v.pc  = pc;
            v.pos = QPointF(originX + i * dx + j * (dx * 0.5),
                            originY - j * dy);
            m_vertices[vidx(i, j)] = v;
        }
    }

    // Triangles: per (i, j) unit cell we get an upright triangle
    // (i, j), (i+1, j), (i, j+1) -- MAJOR root at PC(i,j) -- and an
    // inverted triangle (i+1, j), (i+1, j+1), (i, j+1) -- MINOR root at
    // PC(i, j+1). Cells beyond iMax-1 in i or jMax-1 in j contribute only
    // the vertices, no triangles (no neighbours to close the cell).
    auto triadName = [](int rootPC, bool isMajor) {
        QString s = QString::fromUtf8(kPcNames[rootPC]);
        if (!isMajor) s += QStringLiteral("m");
        return s;
    };

    auto addEdge = [&](int va, int vb) {
        int a = std::min(va, vb);
        int b = std::max(va, vb);
        // Linear scan against existing edges -- the count is ~3 * triads,
        // i.e. low double-digits, so a hash set would be overkill.
        for (const auto& e : m_edges) {
            if (e.first == a && e.second == b) return;
        }
        m_edges.append(qMakePair(a, b));
    };

    m_triads.reserve(iMax * jMax * 2);

    for (int j = 0; j < jMax; ++j) {
        for (int i = 0; i < iMax; ++i) {
            const int v00 = vidx(i,     j);
            const int v10 = vidx(i + 1, j);
            const int v01 = vidx(i,     j + 1);
            const int v11 = vidx(i + 1, j + 1);

            // --- Upright (major) triangle: (i,j), (i+1,j), (i,j+1) ---
            {
                Triad t;
                const int rootPC = m_vertices[v00].pc;
                t.rootPC  = rootPC;
                t.pcs[0]  = rootPC;                       // root
                t.pcs[1]  = m_vertices[v01].pc;           // third (root+4)
                t.pcs[2]  = m_vertices[v10].pc;           // fifth (root+7)
                t.isMajor = true;
                t.triangle = QPolygonF({m_vertices[v00].pos,
                                        m_vertices[v10].pos,
                                        m_vertices[v01].pos});
                t.centroid = (m_vertices[v00].pos
                              + m_vertices[v10].pos
                              + m_vertices[v01].pos) / 3.0;
                t.label    = triadName(rootPC, true);
                m_triads.append(t);
            }
            // --- Inverted (minor) triangle: (i+1,j), (i+1,j+1), (i,j+1) ---
            {
                Triad t;
                // Minor triad rooted at PC(i, j+1) -- the M3 axis vertex.
                const int rootPC = m_vertices[v01].pc;
                t.rootPC = rootPC;
                t.pcs[0] = rootPC;                        // root (= PC(i,j+1))
                t.pcs[1] = m_vertices[v11].pc;            // third (root+3)
                t.pcs[2] = m_vertices[v10].pc;            // fifth (root+7)
                t.isMajor = false;
                t.triangle = QPolygonF({m_vertices[v10].pos,
                                        m_vertices[v11].pos,
                                        m_vertices[v01].pos});
                t.centroid = (m_vertices[v10].pos
                              + m_vertices[v11].pos
                              + m_vertices[v01].pos) / 3.0;
                t.label    = triadName(rootPC, false);
                m_triads.append(t);
            }

            // Edges -- dedupe across triangles. The shared diagonal
            // (v10, v01) appears in both triangles; addEdge handles dedupe.
            addEdge(v00, v10);
            addEdge(v00, v01);
            addEdge(v10, v01);   // shared with the inverted triangle
            addEdge(v10, v11);
            addEdge(v11, v01);
        }
    }
}


// --- Hop -> lit recomputation ------------------------------------------------

void TonnetzView::setActive(bool a)
{
    if (m_active == a) return;
    m_active = a;
    emit activeChanged();
}


void TonnetzView::onChromaUpdated()
{
    if (!m_active) return;
    if (!m_chromaSource) return;
    m_chromaSource->fillChromaSmoothed(m_lastChroma.data());
    recomputeLit();
    emit chromaTickComplete();
    update();
}


void TonnetzView::recomputeLit()
{
    // 1. Identify the active PC set: top-K above the threshold. The
    //    chromagram is already normalized so max=1; threshold is an
    //    absolute fraction of that max.
    struct Cand { int pc; float v; };
    std::array<Cand, PITCH_CLASSES> ranked;
    int nRanked = 0;
    for (int p = 0; p < PITCH_CLASSES; ++p) {
        if (m_lastChroma[p] >= m_litThreshold) {
            ranked[nRanked++] = {p, m_lastChroma[p]};
        }
    }
    // Partial sort descending by magnitude, keep top-K.
    std::partial_sort(ranked.begin(), ranked.begin() + std::min(nRanked, m_topK),
                      ranked.begin() + nRanked,
                      [](const Cand& a, const Cand& b) { return a.v > b.v; });
    const int activeCount = std::min(nRanked, m_topK);

    for (int p = 0; p < PITCH_CLASSES; ++p) m_activeSet[p] = false;
    for (int k = 0; k < activeCount; ++k) m_activeSet[ranked[k].pc] = true;

    // 2. Score each triangle and track the brightest.
    float bestLit  = 0.0f;
    int   bestIdx  = -1;
    for (int i = 0; i < m_triads.size(); ++i) {
        Triad& t = m_triads[i];
        const float c0 = m_lastChroma[t.pcs[0]];
        const float c1 = m_lastChroma[t.pcs[1]];
        const float c2 = m_lastChroma[t.pcs[2]];
        float lit = std::min(c0, std::min(c1, c2));
        if (m_activeSet[t.pcs[0]] && m_activeSet[t.pcs[1]]
            && m_activeSet[t.pcs[2]])
        {
            lit += kAllActiveBonus;
        }
        lit = std::clamp(lit, 0.0f, kLitMax);
        t.lit = lit;

        if (lit > bestLit) {
            bestLit = lit;
            bestIdx = i;
        }
    }

    // 3. brightestTriadLabel binding. Empty when nothing is lit above the
    //    threshold; otherwise the chord name of the top triad.
    if (bestIdx >= 0 && bestLit >= m_litThreshold) {
        m_brightestLabel = m_triads[bestIdx].label;
    } else {
        m_brightestLabel.clear();
    }
}


void TonnetzView::debugSetChroma(const float* c12)
{
    if (!c12) return;
    std::memcpy(m_lastChroma.data(), c12, sizeof(float) * PITCH_CLASSES);
    if (m_triads.isEmpty()) rebuildLattice();
    recomputeLit();
}


// --- Painting ----------------------------------------------------------------

void TonnetzView::paint(QPainter* painter)
{
    if (!painter) return;
    if (m_triads.isEmpty()) rebuildLattice();
    if (m_triads.isEmpty()) return;

    painter->setRenderHint(QPainter::Antialiasing, true);

    // 1. Background panels per triangle -- gives the lattice a subtle
    //    "honeycomb" texture independent of the lit triads. Uses normal
    //    composition (over) at a low alpha; we'll switch to additive for
    //    the lit layer below.
    {
        QColor restA = m_restColor;
        if (restA.alpha() == 255) {
            // Default rest color is opaque dark grey; soften it so the
            // shape is visible without occluding anything behind.
            restA.setAlpha(96);
        }
        painter->setPen(Qt::NoPen);
        painter->setBrush(restA);
        for (const Triad& t : m_triads) {
            painter->drawPolygon(t.triangle);
        }
    }

    // 2. Edge lines -- one per unique edge, deduped during build.
    {
        QPen pen(m_lineColor);
        pen.setWidthF(1.0);
        pen.setCosmetic(true);
        painter->setPen(pen);
        painter->setBrush(Qt::NoBrush);
        for (const auto& e : m_edges) {
            painter->drawLine(m_vertices[e.first].pos,
                              m_vertices[e.second].pos);
        }
    }

    // 3. Lit triangle fills with additive blending. Each triangle's lit
    //    value scales the colour's alpha; major vs minor uses the
    //    configured tint. CompositionMode_Plus is what gives the chord
    //    "glow" look -- overlapping lit regions saturate gracefully
    //    without going to flat white.
    {
        painter->setCompositionMode(QPainter::CompositionMode_Plus);
        painter->setPen(Qt::NoPen);
        for (const Triad& t : m_triads) {
            if (t.lit <= 0.001f) continue;
            QColor c = t.isMajor ? m_majorColor : m_minorColor;
            const float alpha = std::clamp(t.lit, 0.0f, 1.0f);
            c.setAlphaF(qreal(alpha));
            painter->setBrush(c);
            painter->drawPolygon(t.triangle);
        }
        painter->setCompositionMode(QPainter::CompositionMode_SourceOver);
    }

    // Optional: P/L/R axis guides. When showAxes is on, draw three short
    // arrows from the centroid of the patch indicating the P5, M3, m3
    // directions. Purely decorative; off by default to keep the chord
    // display uncluttered.
    if (m_showAxes && !m_vertices.isEmpty()) {
        // Use the first cell's vertices as direction anchors.
        const QPointF center = m_vertices[m_vertices.size() / 2].pos;
        const QPointF dirP5  = m_vertices[1].pos - m_vertices[0].pos;
        // M3 axis -- j=1 row from origin.
        // The iCount/iMax math is lost outside rebuildLattice; pull from
        // vertex spacing instead: pick the second-row index 0 vertex by
        // scanning -- guaranteed (i=0,j=1) lives at vidx(0,1).
        // We can't recover iCount here without restoring it; instead use
        // a robust heuristic: scan for the vertex whose y is one dy below
        // m_vertices[0].pos.y(). This runs once per paint -- cheap.
        QPointF dirM3 = dirP5;   // safe default
        const qreal y0 = m_vertices[0].pos.y();
        for (const VertexNode& v : m_vertices) {
            if (std::abs(v.pos.y() - y0) > 1.0
                && v.pos.x() > m_vertices[0].pos.x() - 1.0
                && v.pos.x() < m_vertices[0].pos.x() + 60.0)
            {
                dirM3 = v.pos - m_vertices[0].pos;
                break;
            }
        }
        const QPointF dirm3 = dirP5 - dirM3;

        QPen axisPen(QColor(255, 196, 51, 200));
        axisPen.setWidthF(2.0);
        painter->setPen(axisPen);
        painter->setBrush(Qt::NoBrush);
        painter->drawLine(center, center + dirP5 * 0.6);
        painter->setPen(QPen(QColor(255, 89, 89, 200), 2.0));
        painter->drawLine(center, center + dirM3 * 0.6);
        painter->setPen(QPen(QColor(51, 200, 255, 200), 2.0));
        painter->drawLine(center, center + dirm3 * 0.6);

        QFont f = painter->font();
        f.setPixelSize(10);
        f.setFamily("Iosevka");
        painter->setFont(f);
        painter->setPen(QColor(255, 196, 51, 220));
        painter->drawText(center + dirP5 * 0.65, "P5");
        painter->setPen(QColor(255, 89, 89, 220));
        painter->drawText(center + dirM3 * 0.65, "M3");
        painter->setPen(QColor(51, 200, 255, 220));
        painter->drawText(center + dirm3 * 0.65, "m3");
    }

    // 4. Vertex circles -- small disc per unique vertex. Soft blue-grey
    //    so they read as "lattice points" rather than competing with the
    //    lit triangles.
    {
        const qreal r = 11.0;
        QColor discCol(36, 42, 56, 220);
        painter->setPen(QPen(QColor(255, 255, 255, 51), 1.0));
        painter->setBrush(discCol);
        for (const VertexNode& v : m_vertices) {
            painter->drawEllipse(v.pos, r, r);
        }
    }

    // 5. Vertex labels.
    if (m_showLabels) {
        QFont f = painter->font();
        f.setPixelSize(11);
        f.setFamily("Iosevka");
        f.setBold(true);
        painter->setFont(f);
        painter->setPen(m_labelColor);
        // Center text on each vertex; QPainter's drawText with a single
        // QPointF uses the baseline, so offset by ~ascent/2 - descent/2.
        const QFontMetricsF fm(f);
        for (const VertexNode& v : m_vertices) {
            const QString name = QString::fromUtf8(kPcNames[v.pc]);
            const qreal tw = fm.horizontalAdvance(name);
            const qreal th = fm.ascent() - fm.descent();
            painter->drawText(QPointF(v.pos.x() - tw * 0.5,
                                      v.pos.y() + th * 0.5),
                              name);
        }
    }
}


QString TonnetzView::triadName(int rootPC, bool isMajor) const
{
    if (rootPC < 0 || rootPC >= 12) return {};
    QString s = QString::fromUtf8(kPcNames[rootPC]);
    if (!isMajor) s += QStringLiteral("m");
    return s;
}
