// Hilbert-pair rosette visualizer (Layer 4c).
//
// 8 dots orbit a common center; each dot's radius is its log-spaced
// subband's analytic envelope, each dot's angular position is the
// instantaneous phase of the band's analytic signal. Result: a beating
// rosette/Lissajous bouquet whose closed-orbit moments correspond to
// periodic musical structure and whose open trails correspond to
// aperiodic content. Mathematically free CPU; nice ear-flattering eye
// candy.
//
// Drop into any container that gives it a size:
//   HilbertRosetteView { anchors.fill: parent }
//
// `audioFeatures` is the global QML context property exposed from
// main.cpp; we forward it to the renderer's audioSource.

import QtQuick
import QtQuick.Controls
import PLYR

Item {
    id: root

    property var   audioSource: audioFeatures
    property real  trailTau: 0.15
    property real  dotRadius: 8.0
    property real  ringRadius: 0.40
    property bool  showBaseRing: true
    property bool  showBandLabels: false
    property color backgroundColor: "#070A12"

    Rectangle {
        anchors.fill: parent
        color: root.backgroundColor
    }

    // Optional guide ring -- a faint static circle at the dots' base
    // radius. Drawn as a QML Shape so we don't need a dedicated GPU
    // pipeline just for the overlay; the rosette itself is RHI-backed.
    Canvas {
        id: baseRing
        anchors.fill: parent
        visible: root.showBaseRing
        antialiasing: true

        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            ctx.strokeStyle = Qt.rgba(1, 1, 1, 0.16)
            ctx.lineWidth   = 1.0
            const cx = width  * 0.5
            const cy = height * 0.5
            // The dots' base radius mirrors HilbertRosette::render(): the
            // baseline orbit is 0.3 * rMax where rMax = 0.5*min(w,h)*ringR.
            const r  = 0.5 * Math.min(width, height) * root.ringRadius * 0.3
            ctx.beginPath()
            ctx.arc(cx, cy, r, 0, Math.PI * 2)
            ctx.stroke()
        }

        // Repaint when the size or ringRadius changes.
        Connections {
            target: root
            function onRingRadiusChanged() { baseRing.requestPaint() }
        }
        onWidthChanged:  requestPaint()
        onHeightChanged: requestPaint()
    }

    HilbertRosette {
        id: rosette
        anchors.fill: parent
        audioSource:    root.audioSource
        trailTau:       root.trailTau
        dotRadius:      root.dotRadius
        ringRadius:     root.ringRadius
        showBaseRing:   false   // overlay handled by Canvas above
        showBandLabels: root.showBandLabels
    }

    // Optional band-Hz labels around the orbit. Mirrors the dot's base
    // angle layout (band b at angle -pi/2 + 2pi*b/N). Toggled via the
    // showBandLabels property.
    Repeater {
        model: root.showBandLabels ? rosette.bandCenters() : []
        Item {
            x: parent.width  * 0.5 + Math.cos(angle) * radius - width  * 0.5
            y: parent.height * 0.5 + Math.sin(angle) * radius - height * 0.5
            width: lbl.implicitWidth + 8
            height: lbl.implicitHeight + 4

            // angle = -pi/2 + 2*pi*index/N  -- band 0 at the top.
            readonly property int    nBands: rosette.bandCenters().length
            readonly property real   angle: -Math.PI * 0.5
                + 2.0 * Math.PI * index / nBands
            // Position labels just outside the dot's maximum orbital
            // reach (envelope=1 -> r=0.8 * rMax; label sits at ~0.95).
            readonly property real   radius: 0.5
                * Math.min(parent.width, parent.height)
                * root.ringRadius * 0.95

            Text {
                id: lbl
                anchors.centerIn: parent
                text: {
                    // modelData is the band center in Hz; format with
                    // automatic unit (Hz / kHz).
                    const hz = modelData
                    return hz >= 1000
                        ? (hz / 1000).toFixed(hz >= 10000 ? 0 : 1) + " kHz"
                        : hz.toFixed(0) + " Hz"
                }
                color: Qt.rgba(1, 1, 1, 0.55)
                font.family: "Menlo"
                font.pixelSize: 9
            }
        }
    }

    // Small subtitle overlay matching the other Layer N views.
    Text {
        anchors.bottom: parent.bottom
        anchors.left:   parent.left
        anchors.margins: 12
        text: "HILBERT ROSETTE * 8 BANDS"
        color: Qt.rgba(1, 1, 1, 0.35)
        font.family: "Menlo"
        font.pixelSize: 10
    }
}
