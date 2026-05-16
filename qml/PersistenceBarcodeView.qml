// Persistent-homology barcode visualizer (Layer 4a).
//
// We embed the audio's sliding-window MFCC trajectory as a point cloud
// in 13-D space and compute its Vietoris-Rips persistent homology with
// Ripser (Ulrich Bauer, MIT) on a worker thread every ~0.5 s. The
// resulting (birth, death, dim) bars are rendered as horizontal stripes
// -- gray for H0 (connected components), bright cyan for H1 (1-D
// loops). When the music has a repeating chorus, you literally see the
// loop's birth and death in the barcode.
//
// Math reference: Tralie & Perea 2017, "Sliding windows and persistence:
// An application of topological methods to signal analysis"
// (arxiv.org/abs/1703.04127). The technique has been demonstrated in
// JS (Tralie's LoopDitty) but, to the team's knowledge, has not
// shipped in a consumer music player.
//
// Drop into any container that gives it a size:
//   PersistenceBarcodeView { anchors.fill: parent }

import QtQuick
import QtQuick.Controls
import PLYR

Item {
    id: root

    property var  audioSource: audioFeatures
    property real maxRadius: 2.0          // honored when autoScaleRadius=false
    property int  windowSize: 64           // sliding-window point cloud size
    property int  hopsPerCompute: 30       // ~0.5 s between recomputes
    property real minPersistence: 0.05     // drop bars shorter than this
    property bool autoScaleRadius: true    // fit x-axis to last threshold
    property color backgroundColor: "#070A12"
    property color h0Color: "#88888888"
    property color h1Color: "#33C8FFFF"

    Rectangle {
        anchors.fill: parent
        color: root.backgroundColor
    }

    MfccAnalyzer {
        id: mfcc
        audioSource: root.audioSource
    }

    PersistentHomologyAnalyzer {
        id: ph
        mfccSource: mfcc
        windowSize: root.windowSize
        hopsPerCompute: root.hopsPerCompute
    }

    PersistenceBarcode {
        id: barcode
        anchors.fill: parent
        anchors.margins: 24
        analyzer: ph
        maxRadius: root.maxRadius
        h0Color: root.h0Color
        h1Color: root.h1Color
        minPersistence: root.minPersistence
        autoScaleRadius: root.autoScaleRadius
    }

    // Track-change hook: wipe the analyzer's hop counter + last-pairs
    // snapshot so the previous song's barcode doesn't bleed into the
    // new one's first compute. The `playlist` context property is set
    // in main.cpp; guard so the view also runs in test harnesses where
    // playlist isn't bound.
    Connections {
        target: typeof playlist !== "undefined" ? playlist : null
        ignoreUnknownSignals: true
        function onCurrentIndexChanged() {
            ph.reset()
        }
    }

    // --- Header overlay (title + cadence chip) ----------------------------
    Column {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.margins: 12
        spacing: 2
        Text {
            text: "H0 / H1 PERSISTENCE BARCODE"
            color: Qt.rgba(1, 1, 1, 0.55)
            font.family: "Menlo"
            font.pixelSize: 9
            font.bold: true
        }
        Text {
            text: "WINDOW " + root.windowSize
                  + " * REFRESH " + (root.hopsPerCompute * 16) + "MS"
                  + " * COMPUTE " + (ph.lastComputeUsec / 1000.0).toFixed(1) + "MS"
            color: Qt.rgba(1, 1, 1, 0.3)
            font.family: "Menlo"
            font.pixelSize: 8
        }
    }

    // --- Bottom-right: H1 loop counter ------------------------------------
    Text {
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        anchors.margins: 12
        text: {
            const pairs = ph.latestPairs() || []
            let count = 0
            for (let i = 0; i < pairs.length; ++i) {
                const p = pairs[i]
                if (p.dim === 1
                    && isFinite(p.death)
                    && (p.death - p.birth) > Math.max(0.1, root.minPersistence)) {
                    count++
                }
            }
            return "H1 LOOPS: " + count
        }
        color: Qt.rgba(0.2, 0.78, 1.0, 0.75)
        font.family: "Menlo"
        font.pixelSize: 11
        font.bold: true
    }

    // --- ANALYZING... chip ------------------------------------------------
    // Visible while the very first compute is in flight. The first compute
    // can't fire until ~windowSize hops have accumulated (~1 s at 60 Hz
    // hop rate) AND the chosen hopsPerCompute counter has elapsed once.
    Text {
        anchors.centerIn: parent
        visible: !ph.hasData
        text: "ANALYZING..."
        color: Qt.rgba(0.2, 0.78, 1.0, 0.85)
        font.family: "Menlo"
        font.pixelSize: 16
        font.bold: true
    }
}
