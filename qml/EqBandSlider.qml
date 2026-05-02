// EqBandSlider — viz-bar-style vertical slider.
//
// The slider's "track" IS a visualizer bar: segmented, tapered, ocean→sky
// gradient. Live audio amplitude fills segments from the bottom, peak-hold
// is a bright outlined segment, and the user's gain setting sits on top as
// a bright horizontal marker. Drag anywhere in the bar to set gain.
//
// This widget therefore displays three independent things on one bar:
//   - level (0..1) — live post-EQ audio amplitude in this band
//   - peak  (0..1) — peak-hold of that amplitude, decaying
//   - value (±12) — user's current gain setting (independent of level)

import QtQuick
import QtQuick.Layouts

Item {
    id: root

    property real frequency: 1000
    property real value:     0       // dB, ±12
    property real level:     0       // 0..1
    property real peak:      0       // 0..1
    property bool active:    true

    signal moved(real dB)

    readonly property color ocean:     Qt.rgba(0.02, 0.18, 0.45, 1.0)
    readonly property color sky:       Qt.rgba(0.45, 0.78, 1.00, 1.0)
    readonly property color oceanDim:  Qt.rgba(0.08, 0.085, 0.10, 1.0)
    readonly property color skyDim:    Qt.rgba(0.19, 0.20, 0.22, 1.0)
    readonly property color primary:   "white"
    readonly property color muted:     Qt.rgba(1, 1, 1, 0.45)
    readonly property color markerCol: Qt.rgba(1, 1, 1, 0.95)

    readonly property int  segments: 16
    readonly property real segGap:   2.0

    implicitWidth: 56

    function _formatFreq(hz) {
        if (hz >= 1000) {
            const k = hz / 1000
            return (k === Math.round(k) ? k.toFixed(0) : k.toFixed(1)) + "k"
        }
        return Math.round(hz).toString()
    }
    function _formatDb(db) {
        const sign = db >= 0.05 ? "+" : (db <= -0.05 ? "" : " ")
        return sign + db.toFixed(1)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 4

        Text {
            Layout.alignment: Qt.AlignHCenter
            text: root._formatFreq(root.frequency)
            color: root.active ? root.primary : root.muted
            font.pixelSize: 10
        }

        // -------- The bar (Canvas-rendered) -----------------------------
        Item {
            id: barArea
            Layout.alignment: Qt.AlignHCenter
            Layout.fillHeight: true
            Layout.fillWidth: true
            implicitWidth: 24

            Canvas {
                id: canvas
                anchors.fill: parent
                antialiasing: true

                Connections {
                    target: root
                    function onValueChanged()  { canvas.requestPaint() }
                    function onLevelChanged()  { canvas.requestPaint() }
                    function onPeakChanged()   { canvas.requestPaint() }
                    function onActiveChanged() { canvas.requestPaint() }
                }
                onWidthChanged:  requestPaint()
                onHeightChanged: requestPaint()

                onPaint: {
                    const ctx = getContext("2d")
                    const W = width, H = height
                    if (W <= 0 || H <= 0) return
                    ctx.clearRect(0, 0, W, H)

                    const N       = root.segments
                    const gap     = root.segGap
                    const totalG  = gap * (N - 1)
                    const segH    = (H - totalG) / N
                    const centerX = W / 2
                    const baseW   = Math.min(W * 0.85, 20)

                    // Subtle bottom-heavy taper, like the main viz bars.
                    function segW(i) {
                        const t = i / (N - 1)   // 0 = bottom, 1 = top
                        return baseW * (1.0 - 0.18 * t)
                    }

                    function lerp(a, b, t) { return a * (1 - t) + b * t }
                    function segColor(i, lit) {
                        const t = i / (N - 1)
                        const c = lit
                            ? Qt.rgba(lerp(root.ocean.r, root.sky.r, t),
                                      lerp(root.ocean.g, root.sky.g, t),
                                      lerp(root.ocean.b, root.sky.b, t), 1)
                            : Qt.rgba(lerp(root.oceanDim.r, root.skyDim.r, t),
                                      lerp(root.oceanDim.g, root.skyDim.g, t),
                                      lerp(root.oceanDim.b, root.skyDim.b, t), 1)
                        return c
                    }

                    const litCount = Math.round(root.level * N)
                    const peakSeg  = Math.round(root.peak  * N)

                    for (let i = 0; i < N; i++) {
                        const y = H - (i + 1) * segH - i * gap
                        const w = segW(i)
                        const x = centerX - w / 2

                        const lit = (i < litCount) && root.active
                        ctx.fillStyle = segColor(i, lit)
                        ctx.fillRect(x, y, w, segH)

                        // Peak-hold: outlined bright segment.
                        if (i === peakSeg - 1 && root.peak > 0.01 && root.active) {
                            ctx.strokeStyle = Qt.rgba(1, 1, 1, 0.85)
                            ctx.lineWidth   = 1.2
                            ctx.strokeRect(x + 0.6, y + 0.6, w - 1.2, segH - 1.2)
                        }
                    }

                    // Gain marker: bright horizontal bar at the user's set
                    // value. Width matches the bar's tapered width at this
                    // Y so the marker sits flush with the segment edges.
                    // value=+12 → y=0 (top); value=-12 → y=H (bottom).
                    const gainY = H * (1 - (root.value + 12) / 24)
                    const tAtY  = 1 - (gainY / H)             // 0 bottom, 1 top
                    const markW = baseW * (1.0 - 0.18 * tAtY) // same taper as segW

                    ctx.fillStyle = root.markerCol
                    ctx.fillRect(centerX - markW/2, gainY - 1.5, markW, 3)
                }
            }

            MouseArea {
                anchors.fill: parent
                hoverEnabled: false
                function _gainFromY(y) {
                    const t = Math.max(0, Math.min(1, y / height))
                    return +12 - 24 * t   // y=0 top → +12, y=H bottom → -12
                }
                onPressed: (m) => root.moved(_gainFromY(m.y))
                onPositionChanged: (m) => {
                    if (pressed) root.moved(_gainFromY(m.y))
                }
                onReleased: {
                    if (Math.abs(root.value) < 0.5) root.moved(0)
                }
                onDoubleClicked: root.moved(0)
            }
        }

        Text {
            Layout.alignment: Qt.AlignHCenter
            text: root._formatDb(root.value)
            color: Math.abs(root.value) < 0.1 ? root.muted : root.primary
            font.pixelSize: 10
            font.family: "Menlo"
        }
    }
}
