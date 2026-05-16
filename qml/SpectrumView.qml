// SPAN-style log-frequency spectrum analyzer with optional peak-hold,
// infinite-peak, and fractional-octave smoothing. Drop into a parent:
//
//     SpectrumView { anchors.fill: parent }
//
// All knobs default to a sensible mastering-display configuration:
//   - 20 Hz .. 20 kHz log frequency axis
//   - -90 .. 0 dB dynamic range
//   - +4.5 dB/oct display slope (pink noise reads flat)
//   - peak-hold on, infinite-peak off, no fractional-octave smoothing
//   - viridis colormap with a faint vertical brightness ramp
//
// `audioFeatures` is the global QML context property set up in main.cpp;
// override `audioSource` if you want to feed a different DSP backbone.

import QtQuick
import QtQuick.Controls
import PLYR

Item {
    id: root

    property var  audioSource: audioFeatures
    property real displaySlope: 4.5
    property real smoothingOctaves: 0
    property real freqMin: 20
    property real freqMax: 20000
    property real dBMin: -90
    property real dBMax: 0
    property bool showPeakHold: true
    property bool showInfinitePeak: false
    property bool showGrid: true
    property bool showLabels: true
    property color fillTint: "white"

    // Palette. Viridis is the analytic default in the shader; setting
    // useViridis=0 from QML would route through fillColorBottom/Top
    // instead — we expose those for future user-themeable colormaps.
    property color fillColorBottom: "#460B5C"
    property color fillColorTop:    "#FCE61F"
    property color peakColor:       Qt.rgba(1, 1, 1, 0.6)
    property color infPeakColor:    "#FF9A33"

    readonly property color _panelBg: "#0a0a0a"
    readonly property color _gridFaint:  Qt.rgba(1, 1, 1, 0.06)
    readonly property color _gridBright: Qt.rgba(1, 1, 1, 0.12)
    readonly property color _labelMuted: Qt.rgba(1, 1, 1, 0.35)

    // -----------------------------------------------------------------
    //  Background panel + analyzer
    // -----------------------------------------------------------------

    Rectangle {
        anchors.fill: parent
        color: root._panelBg
    }

    // The DSP item. Its CPU pipeline runs per featuresUpdated; the bound
    // ShaderEffect samples its output texture below.
    SpectrumAnalyzer {
        id: analyzer
        audioSource:      root.audioSource
        displaySlope:     root.displaySlope
        smoothingOctaves: root.smoothingOctaves
        freqMin:          root.freqMin
        freqMax:          root.freqMax
        dBMin:            root.dBMin
        dBMax:            root.dBMax
        showPeakHold:     root.showPeakHold
        showInfinitePeak: root.showInfinitePeak
        fillTint:         root.fillTint
        // No anchors / position — the analyzer is a logical 1×DISPLAY_BINS
        // pump only; the ShaderEffect sampling it does the visual work.
        // Force a renderable footprint of 0×0 at the origin so it never
        // captures input, never paints over the analyzer canvas.
        width: 0
        height: 0
    }

    ShaderEffect {
        id: spectrumPaint
        anchors.fill: parent

        // The three texture sources. `source` is the special property
        // ShaderEffect reads to find its primary sampler; the other two
        // bind to extra sampler2D's via property-name → binding match.
        property var source:        analyzer
        property var peakSource:    analyzer.peakProvider
        property var infPeakSource: analyzer.infPeakProvider

        property color fillColorBottom: root.fillColorBottom
        property color fillColorTop:    root.fillColorTop
        property color peakColor:       root.peakColor
        property color infPeakColor:    root.infPeakColor
        property color fillTint:        root.fillTint
        property vector2d viewportPx:   Qt.vector2d(width, height)
        property real dBMin:            root.dBMin
        property real dBMax:            root.dBMax
        property real showPeakHoldF:    root.showPeakHold    ? 1.0 : 0.0
        property real showInfinitePeakF: root.showInfinitePeak ? 1.0 : 0.0
        // 1 = use the analytic viridis polynomial (mastering default).
        property real useViridis: 1.0

        vertexShader:   "qrc:/shaders/spectrum.vert.qsb"
        fragmentShader: "qrc:/shaders/spectrum.frag.qsb"
    }

    // -----------------------------------------------------------------
    //  Grid overlay
    // -----------------------------------------------------------------
    //
    //  Verticals on each octave 31.25 — 16k Hz; thicker decade lines
    //  at 100/1k/10k. Horizontals every 10 dB; thicker at 0 / -60 dB.
    //  Frequency labels under the bottom edge, dB labels along the
    //  right edge. Canvas2D so we only repaint on geometry change.

    Canvas {
        id: gridCanvas
        anchors.fill: parent
        antialiasing: false
        visible: root.showGrid
        renderStrategy: Canvas.Cooperative

        // Geometry- and parameter-driven repaint. requestPaint is cheap
        // enough that we redraw on every resize without throttling.
        onWidthChanged:  requestPaint()
        onHeightChanged: requestPaint()
        Connections {
            target: root
            function onFreqMinChanged()   { gridCanvas.requestPaint() }
            function onFreqMaxChanged()   { gridCanvas.requestPaint() }
            function onDBMinChanged()     { gridCanvas.requestPaint() }
            function onDBMaxChanged()     { gridCanvas.requestPaint() }
            function onShowLabelsChanged(){ gridCanvas.requestPaint() }
        }

        // Map frequency f (Hz) → display x (px). Matches the analyzer's
        // log-x mapping so the grid lines sit exactly under the data.
        function xForFreq(f) {
            const lo = Math.log(root.freqMin)
            const hi = Math.log(root.freqMax)
            return (Math.log(f) - lo) / (hi - lo) * width
        }
        function yForDb(db) {
            // dBMin at the bottom, dBMax at the top.
            const t = (db - root.dBMin) / (root.dBMax - root.dBMin)
            return (1.0 - t) * height
        }

        onPaint: {
            const ctx = getContext("2d")
            ctx.clearRect(0, 0, width, height)

            // ---- Horizontal dB lines ---------------------------------
            const dbStep = 10
            const dbStart = Math.ceil(root.dBMin / dbStep) * dbStep
            for (let db = dbStart; db <= root.dBMax; db += dbStep) {
                const y = Math.floor(yForDb(db)) + 0.5
                ctx.beginPath()
                ctx.moveTo(0, y); ctx.lineTo(width, y)
                ctx.lineWidth = 1
                // Bright reference lines: 0 dB and -60 dB. The rest is faint.
                ctx.strokeStyle = (db === 0 || db === -60)
                                ? root._gridBright
                                : root._gridFaint
                ctx.stroke()
            }

            // ---- Vertical octave lines -------------------------------
            const octaves = [31.25, 62.5, 125, 250, 500, 1000, 2000, 4000, 8000, 16000]
            const decades = [100, 1000, 10000]
            for (const f of octaves) {
                if (f < root.freqMin || f > root.freqMax) continue
                const x = Math.floor(xForFreq(f)) + 0.5
                ctx.beginPath()
                ctx.moveTo(x, 0); ctx.lineTo(x, height)
                ctx.lineWidth = 1
                ctx.strokeStyle = root._gridFaint
                ctx.stroke()
            }
            for (const f of decades) {
                if (f < root.freqMin || f > root.freqMax) continue
                const x = Math.floor(xForFreq(f)) + 0.5
                ctx.beginPath()
                ctx.moveTo(x, 0); ctx.lineTo(x, height)
                ctx.lineWidth = 1
                ctx.strokeStyle = root._gridBright
                ctx.stroke()
            }

            // ---- Labels ----------------------------------------------
            if (!root.showLabels) return

            ctx.fillStyle = root._labelMuted
            ctx.font = "9px Menlo"

            // Frequency labels: anchored to the bottom edge.
            const fLabels = [
                { f: 20,    t: "20"  },
                { f: 100,   t: "100" },
                { f: 1000,  t: "1k"  },
                { f: 10000, t: "10k" },
                { f: 20000, t: "20k" }
            ]
            ctx.textAlign = "center"
            ctx.textBaseline = "bottom"
            for (const lbl of fLabels) {
                if (lbl.f < root.freqMin || lbl.f > root.freqMax) continue
                const x = xForFreq(lbl.f)
                ctx.fillText(lbl.t, x, height - 2)
            }

            // dB labels: anchored to the right edge.
            const dbLabels = [-60, -30, 0]
            ctx.textAlign = "right"
            ctx.textBaseline = "middle"
            for (const db of dbLabels) {
                if (db < root.dBMin || db > root.dBMax) continue
                const y = yForDb(db)
                ctx.fillText(db.toString(), width - 3, y)
            }
        }
    }

    // -----------------------------------------------------------------
    //  Top-right chip row: slope (dB/oct) + smoothing (1/N oct)
    // -----------------------------------------------------------------

    Row {
        id: chipRow
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 6
        spacing: 4

        // Cycle slope through 0 → +3 → +4.5 → 0. Three values is enough
        // to differentiate flat from "mastering-flat-on-pink" without
        // ballooning the UI.
        Rectangle {
            id: slopeChip
            width:  slopeText.implicitWidth + 14
            height: 20
            color:  Qt.rgba(0, 0, 0, 0.55)
            border.color: Qt.rgba(1, 1, 1, 0.20)
            border.width: 1
            radius: 4

            Text {
                id: slopeText
                anchors.centerIn: parent
                text: root.displaySlope === 0
                      ? "0 dB/oct"
                      : (root.displaySlope === 3
                         ? "+3 dB/oct"
                         : "+4.5 dB/oct")
                color: "white"
                font.family: "Menlo"
                font.pixelSize: 9
                font.bold: true
            }

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    if (root.displaySlope === 0)        root.displaySlope = 3
                    else if (root.displaySlope === 3)   root.displaySlope = 4.5
                    else                                root.displaySlope = 0
                }
            }
        }

        // Cycle smoothing: off → 1/24 → 1/12 → 1/6 → 1/3 → off.
        Rectangle {
            id: smoothChip
            width:  smoothText.implicitWidth + 14
            height: 20
            color:  Qt.rgba(0, 0, 0, 0.55)
            border.color: Qt.rgba(1, 1, 1, 0.20)
            border.width: 1
            radius: 4

            Text {
                id: smoothText
                anchors.centerIn: parent
                text: {
                    if (root.smoothingOctaves <= 0) return "RAW"
                    // Display as a 1/N fraction. Round to nearest of our
                    // four "musical" values so a slight float wobble doesn't
                    // turn "1/24" into "1/23.999".
                    const inv = Math.round(1 / root.smoothingOctaves)
                    return "1/" + inv + " OCT"
                }
                color: "white"
                font.family: "Menlo"
                font.pixelSize: 9
                font.bold: true
            }

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    const v = root.smoothingOctaves
                    if      (v <= 0)              root.smoothingOctaves = 1/24
                    else if (Math.abs(v - 1/24) < 1e-4) root.smoothingOctaves = 1/12
                    else if (Math.abs(v - 1/12) < 1e-4) root.smoothingOctaves = 1/6
                    else if (Math.abs(v - 1/6)  < 1e-4) root.smoothingOctaves = 1/3
                    else                          root.smoothingOctaves = 0
                }
            }
        }

        // INF-peak reset chip — visible only when infinite-peak is on.
        Rectangle {
            id: resetChip
            visible: root.showInfinitePeak
            width:  resetText.implicitWidth + 14
            height: 20
            color:  Qt.rgba(0.35, 0.05, 0.05, 0.55)
            border.color: Qt.rgba(1, 0.6, 0.2, 0.50)
            border.width: 1
            radius: 4

            Text {
                id: resetText
                anchors.centerIn: parent
                text: "RESET ∞"
                color: "#FFCC88"
                font.family: "Menlo"
                font.pixelSize: 9
                font.bold: true
            }

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: analyzer.resetPeakHold()
            }
        }
    }
}
