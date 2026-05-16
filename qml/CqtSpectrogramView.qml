// CQT spectrogram waterfall (Layer 1c). Drop into a parent:
//
//     CqtSpectrogramView { anchors.fill: parent }
//
// Defaults: 24 bins/octave, 8 octaves (C1 -> just below C9), magma
// colormap, gamma 0.5 (sqrt mapping), 1024 horizontal columns. The
// spectrogram, scroll, and colormap live in shaders/cqt_spectrogram.*;
// this file adds the surrounding chrome (panel background, pitch /
// octave grid, optional axis labels).

import QtQuick
import QtQuick.Controls
import PLYR

Item {
    id: root

    property var  audioSource: audioFeatures
    property real dbMin: -80
    property real dbMax: 0
    property real gamma: 0.5
    property bool showGrid: true
    property bool showLabels: true

    // Colors used when useMagma=0 (off). Magma is the default mastering
    // colormap; leave low/high here so a themer can flip the chip and
    // get a clean two-color spectrogram instead.
    property color lowColor:  "#000000"
    property color highColor: "#FCEC9D"

    readonly property color _panelBg:    "#0a0a0a"
    readonly property color _gridFaint:  Qt.rgba(1, 1, 1, 0.06)
    readonly property color _gridBright: Qt.rgba(1, 1, 1, 0.18)
    readonly property color _labelMuted: Qt.rgba(1, 1, 1, 0.45)

    Rectangle {
        anchors.fill: parent
        color: root._panelBg
    }

    // The DSP/texture item. The ring + analyzer live in here; the
    // ShaderEffect below samples the texture this exposes.
    CqtSpectrogram {
        id: spectrogram
        audioSource: root.audioSource
        dbMin: root.dbMin
        dbMax: root.dbMax
        // No anchors -- this item only needs to live in the QML tree
        // long enough to provide a texture; the visible footprint is
        // the ShaderEffect's. width/height kept tiny so it doesn't
        // capture input or paint the raw R8 texture under the shader.
        width: 0
        height: 0
    }

    ShaderEffect {
        id: spectrogramPaint
        anchors.fill: parent

        property var source: spectrogram
        // scrollOffset is the texel column the writer will use NEXT;
        // forwarded raw so the shader can wrap with mod(u + off, 1).
        property real scrollOffset: spectrogram.scrollOffset
        property real columns:      spectrogram.columns
        property real gamma:        root.gamma
        property color lowColor:    root.lowColor
        property color highColor:   root.highColor
        property real useMagma:     1.0

        vertexShader:   "qrc:/shaders/cqt_spectrogram.vert.qsb"
        fragmentShader: "qrc:/shaders/cqt_spectrogram.frag.qsb"
    }

    // -----------------------------------------------------------------
    //  Grid overlay: octave horizontals (C0..C9 boundaries) + time
    //  marks along the bottom.
    // -----------------------------------------------------------------

    Canvas {
        id: gridCanvas
        anchors.fill: parent
        antialiasing: false
        visible: root.showGrid
        renderStrategy: Canvas.Cooperative

        onWidthChanged:  requestPaint()
        onHeightChanged: requestPaint()
        Connections {
            target: root
            function onShowLabelsChanged() { gridCanvas.requestPaint() }
        }

        // Pitch index p (0 = lowest, bins-1 = highest) -> display y (px).
        // Display has the lowest pitch at the bottom, so flip.
        function yForPitch(p) {
            const t = (p + 0.5) / spectrogram.bins
            return (1.0 - t) * height
        }

        onPaint: {
            const ctx = getContext("2d")
            ctx.clearRect(0, 0, width, height)

            const bpo = spectrogram.binsPerOctave    // 24
            const nO  = spectrogram.nOctaves         // 8

            // --- Octave horizontals + labels ----------------------------
            // The lowest pitch is C1 by default (32.7 Hz). Each successive
            // octave starts bpo bins higher. Draw a faint line every
            // octave, brighter at C-octaves (every nth octave is a power
            // of two of f_min, all of which are C-pitches when f_min is
            // 32.70).
            ctx.font = "9px Menlo"
            ctx.textBaseline = "middle"
            ctx.textAlign = "left"
            ctx.fillStyle = root._labelMuted

            for (let oct = 0; oct < nO; ++oct) {
                const p = oct * bpo
                const y = Math.floor(yForPitch(p)) + 0.5
                ctx.beginPath()
                ctx.moveTo(0, y); ctx.lineTo(width, y)
                ctx.strokeStyle = root._gridBright
                ctx.lineWidth = 1
                ctx.stroke()

                if (root.showLabels) {
                    // f_min is C1 = 32.70 Hz; octave n starts at C(n+1).
                    const noteName = "C" + (oct + 1)
                    ctx.fillText(noteName, 4, y - 6)
                }
            }
            // Cap: line for the top of the highest octave.
            {
                const y = Math.floor(yForPitch(nO * bpo - 1)) - 1.5
                ctx.beginPath()
                ctx.moveTo(0, y + 0.5); ctx.lineTo(width, y + 0.5)
                ctx.strokeStyle = root._gridBright
                ctx.lineWidth = 1
                ctx.stroke()
                if (root.showLabels) {
                    ctx.fillText("C" + (nO + 1), 4, y - 6)
                }
            }

            // Sub-octave halfway markers (pitch-class A inside each
            // octave: bpo*9/12 above C). Skipped when labels are off so
            // a chartless render stays clean.
            if (root.showLabels) {
                ctx.strokeStyle = root._gridFaint
                ctx.lineWidth = 1
                for (let oct = 0; oct < nO; ++oct) {
                    // A is 9 semitones above C; with bpo=24 that's 18
                    // bins. For B != 24 this is approximately right
                    // (the grid is decorative).
                    const aOffset = Math.round(bpo * 9 / 12)
                    const p = oct * bpo + aOffset
                    const y = Math.floor(yForPitch(p)) + 0.5
                    ctx.beginPath()
                    ctx.moveTo(0, y); ctx.lineTo(width, y)
                    ctx.stroke()
                    ctx.fillStyle = Qt.rgba(1, 1, 1, 0.22)
                    ctx.fillText("A" + (oct + 1), 4, y - 6)
                }
            }

            // --- Time labels along the bottom --------------------------
            // The hop rate is roughly 60 Hz (driven by AudioFeatures'
            // featuresUpdated). columns/60 seconds of history fit in the
            // ring. Mark every full second from "now" back.
            if (root.showLabels) {
                ctx.fillStyle = root._labelMuted
                ctx.textAlign = "center"
                ctx.textBaseline = "bottom"
                const cols = spectrogram.columns
                const hopHz = 60.0  // AudioFeatures' refresh rate
                const totalSec = cols / hopHz
                ctx.fillText("NOW", width - 16, height - 2)

                for (let s = 1; s <= 5; ++s) {
                    if (s > totalSec) break
                    const x = (1.0 - s / totalSec) * width
                    if (x < 18) break
                    ctx.beginPath()
                    ctx.moveTo(x + 0.5, height - 6)
                    ctx.lineTo(x + 0.5, height - 1)
                    ctx.strokeStyle = root._gridBright
                    ctx.stroke()
                    ctx.fillText("-" + s + "s", x, height - 7)
                }
            }
        }
    }
}
