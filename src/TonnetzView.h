// TonnetzView -- live neo-Riemannian Tonnetz overlay (Layer 2a).
//
// The Tonnetz is a planar lattice where each vertex is a pitch class and
// each upright/inverted triangle is a major/minor triad. We lay out a
// finite hexagonal patch covering the visible item:
//
//   vertex_pitch_class = (i * 7 + j * 4) mod 12
//   world_x = i * dx + j * (dx / 2)
//   world_y = j * dy * sqrt(3) / 2
//
// i is the perfect-fifth axis (P5 = +7 semitones), j is the major-third
// axis (M3 = +4 semitones). The minor-third axis (m3 = +3) is implicit
// in the (-i, +j+1) direction. Every upright triangle has vertices
// (p, p+4, p+7) -> a MAJOR triad. Every inverted triangle has vertices
// (p, p+3, p+7) -> a MINOR triad.
//
// On each ChromaAnalyzer hop we read the 12-PC smoothed chromagram,
// score every visible triangle as min(chroma[v0], chroma[v1], chroma[v2])
// (min because we want ALL three vertices present, not just two), and
// repaint. The smoothing already lives in ChromaAnalyzer so the visual
// inherits it for free.
//
// QQuickPaintedItem is the right tool here: ~30-40 triangles redrawn
// once per chroma update (60 Hz) at typical visualizer sizes is well
// under any rendering budget, and QPainter gives us free anti-aliasing
// + additive blending for the glow.

#pragma once

#include "ChromaAnalyzer.h"   // full definition needed for the
                              // Q_PROPERTY(ChromaAnalyzer*) meta-type

#include <QColor>
#include <QPolygonF>
#include <QPointF>
#include <QQuickPaintedItem>
#include <QString>
#include <QVector>
#include <array>
#include <qqmlregistration.h>

class TonnetzView : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(ChromaAnalyzer* chromaSource READ chromaSource WRITE setChromaSource
               NOTIFY chromaSourceChanged)
    Q_PROPERTY(float litThreshold READ litThreshold WRITE setLitThreshold
               NOTIFY litThresholdChanged)
    Q_PROPERTY(int   topK READ topK WRITE setTopK NOTIFY topKChanged)
    Q_PROPERTY(QColor majorColor READ majorColor WRITE setMajorColor
               NOTIFY majorColorChanged)
    Q_PROPERTY(QColor minorColor READ minorColor WRITE setMinorColor
               NOTIFY minorColorChanged)
    Q_PROPERTY(QColor restColor  READ restColor  WRITE setRestColor
               NOTIFY restColorChanged)
    Q_PROPERTY(QColor lineColor  READ lineColor  WRITE setLineColor
               NOTIFY lineColorChanged)
    Q_PROPERTY(QColor labelColor READ labelColor WRITE setLabelColor
               NOTIFY labelColorChanged)
    Q_PROPERTY(bool   showLabels READ showLabels WRITE setShowLabels
               NOTIFY showLabelsChanged)
    Q_PROPERTY(bool   showAxes   READ showAxes   WRITE setShowAxes
               NOTIFY showAxesChanged)
    // Name of the currently brightest triad ("C", "Am", "F#", ...). Empty
    // when nothing is above the lit threshold.
    Q_PROPERTY(QString brightestTriadLabel READ brightestTriadLabel
               NOTIFY chromaTickComplete)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)

public:
    static constexpr int PITCH_CLASSES = 12;

    explicit TonnetzView(QQuickItem* parent = nullptr);
    ~TonnetzView() override;

    ChromaAnalyzer* chromaSource() const { return m_chromaSource; }
    void            setChromaSource(ChromaAnalyzer* s);

    float litThreshold() const { return m_litThreshold; }
    void  setLitThreshold(float v);

    int  topK() const { return m_topK; }
    void setTopK(int v);

    QColor majorColor() const { return m_majorColor; }
    void   setMajorColor(const QColor& c);
    QColor minorColor() const { return m_minorColor; }
    void   setMinorColor(const QColor& c);
    QColor restColor()  const { return m_restColor; }
    void   setRestColor(const QColor& c);
    QColor lineColor()  const { return m_lineColor; }
    void   setLineColor(const QColor& c);
    QColor labelColor() const { return m_labelColor; }
    void   setLabelColor(const QColor& c);

    bool showLabels() const { return m_showLabels; }
    void setShowLabels(bool v);
    bool showAxes()   const { return m_showAxes;   }
    void setShowAxes(bool v);

    QString brightestTriadLabel() const { return m_brightestLabel; }

    bool active() const { return m_active; }
    void setActive(bool a);

    void paint(QPainter* painter) override;

    // Number of triangles in the current lattice patch (visible for the
    // verification CLI to sanity-check the construction).
    int triadCount() const { return int(m_triads.size()); }

    // Score a single triad index post-hop. Exposed for the verification
    // harness; UI code reads m_triads directly via paint().
    float triadLit(int idx) const {
        return (idx >= 0 && idx < int(m_triads.size())) ? m_triads[idx].lit : 0.0f;
    }
    int  triadRoot(int idx) const {
        return (idx >= 0 && idx < int(m_triads.size())) ? m_triads[idx].rootPC : -1;
    }
    bool triadIsMajor(int idx) const {
        return (idx >= 0 && idx < int(m_triads.size())) && m_triads[idx].isMajor;
    }
    // Re-run the build + scoring with a synthetic chroma snapshot. Used
    // only by the verification CLI to drive the lattice deterministically
    // without spinning up a CqtAnalyzer + signal pipeline.
    void debugSetChroma(const float* c12);

signals:
    void chromaSourceChanged();
    void litThresholdChanged();
    void topKChanged();
    void majorColorChanged();
    void minorColorChanged();
    void restColorChanged();
    void lineColorChanged();
    void labelColorChanged();
    void showLabelsChanged();
    void showAxesChanged();
    // Fired after each triad-lit recomputation. Drives the brightest-
    // triad-label binding in QML so the corner readout updates live.
    void chromaTickComplete();
    void activeChanged();

protected:
    void geometryChange(const QRectF& newG, const QRectF& oldG) override;

private slots:
    void onChromaUpdated();

private:
    struct Triad {
        int       rootPC  = -1;     // root of the chord (lowest in stack)
        int       pcs[3]  = {-1,-1,-1};  // three pitch-class vertices
        bool      isMajor = true;        // upright = major, inverted = minor
        QPolygonF triangle;              // screen-space vertices
        QPointF   centroid;              // for label placement
        QString   label;                 // "C", "Am", "F#"
        float     lit = 0.0f;            // current lit value [0,1]
    };

    struct VertexNode {
        int       pc = -1;
        QPointF   pos;
    };

    struct EdgeKey {
        int a; int b;
        bool operator==(const EdgeKey& other) const {
            return a == other.a && b == other.b;
        }
    };

    void rebuildLattice();
    void recomputeLit();
    QString triadName(int rootPC, bool isMajor) const;

    ChromaAnalyzer* m_chromaSource = nullptr;

    // Geometry / appearance properties.
    float  m_litThreshold = 0.4f;
    int    m_topK         = 4;
    QColor m_majorColor   = QColor("#FFC433");
    QColor m_minorColor   = QColor("#33C8FF");
    QColor m_restColor    = QColor("#1A1A1F");
    QColor m_lineColor    = QColor(255, 255, 255, 31);   // ~0.12 alpha
    QColor m_labelColor   = QColor(255, 255, 255, 153);  // ~0.6 alpha
    bool   m_showLabels   = true;
    bool   m_showAxes     = false;
    bool   m_active       = true;

    // Lattice -- rebuilt on resize, sized for the current item geometry.
    QVector<Triad>      m_triads;
    QVector<VertexNode> m_vertices;
    // Deduplicated edges as flat (a, b) pairs into m_vertices, with a < b.
    QVector<QPair<int,int>> m_edges;

    // Last smoothed chroma snapshot (read inside paint as well as
    // recomputeLit). Pre-allocated; no per-frame alloc.
    std::array<float, PITCH_CLASSES> m_lastChroma {};

    // Cache of the active-PC set used for the "all three active" bonus
    // in recomputeLit. Sized to PITCH_CLASSES so no per-tick allocation.
    std::array<bool, PITCH_CLASSES> m_activeSet {};

    QString m_brightestLabel;
};
