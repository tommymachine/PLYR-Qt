// Live Tonnetz hexagonal lattice (Layer 2a). Triangles light up as
// triads are detected in the live chromagram. Drop into a parent:
//
//     TonnetzOverlay { anchors.fill: parent }
//
// Wires the CqtAnalyzer + ChromaAnalyzer pipeline behind a TonnetzView.
// All audio binding goes through `audioSource` (default: the global
// audioFeatures context property). The brightest-triad-name readout
// lives in the bottom-right corner as a small Menlo label.

import QtQuick
import QtQuick.Controls
import PLYR

Item {
    id: root

    property var   audioSource: audioFeatures
    property real  litThreshold: 0.4
    property bool  showLabels:   true
    property bool  showAxes:     false
    property color majorColor:   "#FFC433"
    property color minorColor:   "#33C8FF"

    // Optional dark backdrop so the lattice has its own panel context.
    // Transparent by default -- callers usually parent the overlay over
    // another visualizer (e.g. the CQT waterfall) and want to see
    // through to that.
    property color backgroundColor: "transparent"

    Rectangle {
        anchors.fill: parent
        color: root.backgroundColor
    }

    // CQT engine (the chromagram is folded from CQT magnitudes).
    CqtAnalyzer {
        id: cqt
        audioSource: root.audioSource
    }

    // PC folding + envelope smoothing.
    ChromaAnalyzer {
        id: chroma
        cqtSource: cqt
    }

    // The lattice itself.
    TonnetzView {
        id: tonnetz
        anchors.fill: parent
        chromaSource:  chroma
        litThreshold:  root.litThreshold
        showLabels:    root.showLabels
        showAxes:      root.showAxes
        majorColor:    root.majorColor
        minorColor:    root.minorColor
    }

    // Live chord-name readout. Updates whenever the brightest triad
    // changes; empty when nothing is above the threshold.
    Text {
        anchors.bottom: parent.bottom
        anchors.right:  parent.right
        anchors.margins: 12
        text:  tonnetz.brightestTriadLabel
        color: "white"
        font.family: "Menlo"
        font.pixelSize: 11
        font.bold: true
    }
}
