// 3D MFCC trajectory visualizer (Layer 2c). The track's MFCC[1..13]
// vectors are projected to 3D via an online PCA on a 10-s sliding window
// and rendered as a fading polyline orbiting around the origin. The
// chorus of a song closes back on itself; a verse drifts to a new
// region; a bridge breaks the geometry.
//
// Drop into any container that gives it a size:
//   MfccTrajectoryView { anchors.fill: parent }
//
// `audioFeatures` is the global QML context property exposed from
// main.cpp; we forward it to the renderer's audioSource. On track
// changes (playlist.currentIndexChanged) we call resetPCA() so the new
// song's geometry isn't biased by the old one's basis.

import QtQuick
import QtQuick.Controls
import PLYR

Item {
    id: root

    property var   audioSource: audioFeatures
    property color headColor:   "#00E0FF"
    property color tailColor:   "#5B1E96"
    property real  lineWidth:   2.5
    // Backdrop. Transparent by default so callers can stack the view on
    // top of another visualizer; pass a real color to give it its own
    // panel context.
    property color backgroundColor: "#070A12"

    Rectangle {
        anchors.fill: parent
        color: root.backgroundColor
    }

    MfccTrajectory {
        id: traj
        anchors.fill: parent
        audioSource: root.audioSource
        headColor:   root.headColor
        tailColor:   root.tailColor
        lineWidth:   root.lineWidth
    }

    // Track-change reset: when the playlist's currentIndex changes we
    // wipe the PCA + trajectory so the new song's geometry isn't biased
    // by the previous song's principal axes. The `playlist` context
    // property is set in main.cpp; the binding is guarded so the view
    // also works in standalone test harnesses where playlist isn't
    // present.
    Connections {
        target: typeof playlist !== "undefined" ? playlist : null
        ignoreUnknownSignals: true
        function onCurrentIndexChanged() {
            traj.resetPCA()
        }
    }

    // Small overlay -- "TRAJECTORY * MFCC[1..13] -> PCA3" bottom-left,
    // plus an "ANALYZING..." chip while the PCA window is still filling
    // (under 600 rows of data).
    Text {
        anchors.bottom: parent.bottom
        anchors.left:   parent.left
        anchors.margins: 12
        text: "TRAJECTORY * MFCC[1..13] -> PCA3"
        color: Qt.rgba(1, 1, 1, 0.35)
        font.family: "Menlo"
        font.pixelSize: 10
    }

    Text {
        anchors.top:   parent.top
        anchors.right: parent.right
        anchors.margins: 12
        visible: traj.filledRows < 600
        text: "ANALYZING..."
        color: Qt.rgba(1, 1, 1, 0.55)
        font.family: "Menlo"
        font.pixelSize: 11
        font.bold: true
    }
}
