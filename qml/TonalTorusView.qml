// Janata-style 3D tonal torus (Layer 2b). The current key estimate's
// toroidal position glows on the surface; secondary glows show
// neighbouring-key likelihoods (e.g. relative minor, dominant). Camera
// slowly orbits. Drop into a parent:
//
//     TonalTorusView { anchors.fill: parent }
//
// Wires the CqtAnalyzer + ChromaAnalyzer + KeyEstimator pipeline behind
// a fullscreen raymarched-SDF ShaderEffect. The audio backbone goes
// through `audioSource` (default: the global audioFeatures context
// property). All the chord-recognition math lives in KeyEstimator; the
// shader only renders.

import QtQuick
import QtQuick.Controls
import PLYR

Item {
    id: root

    property var   audioSource: audioFeatures
    property color keyColor:    "#FFD966"     // warm amber primary glow
    property color backColor:   "#0C2640"     // deep teal torus base
    property color ringColor:   Qt.rgba(1, 1, 1, 0.08)

    // Backdrop. Transparent by default so callers can stack the view
    // on top of another visualizer; pass a real color via property
    // binding to give it its own panel context.
    property color backgroundColor: "#070A12"

    Rectangle {
        anchors.fill: parent
        color: root.backgroundColor
    }

    // CQT engine -- the chromagram is folded from CQT magnitudes.
    CqtAnalyzer {
        id: cqt
        audioSource: root.audioSource
    }

    // PC folding + envelope smoothing.
    ChromaAnalyzer {
        id: chroma
        cqtSource: cqt
    }

    // K-K key estimation + Janata toroidal projection.
    KeyEstimator {
        id: est
        chromaSource: chroma
    }

    // ------------------------------------------------------------------
    //  Time driver. We need a smoothly-incrementing `time` uniform for
    //  the camera orbit. NumberAnimation gives us a continuously-
    //  evaluated property without burning a QTimer slot. Duration is
    //  arbitrarily large; we only read the value, never let it finish.
    // ------------------------------------------------------------------
    property real elapsed: 0
    NumberAnimation on elapsed {
        from: 0
        to: 1e9                                // arbitrarily huge endpoint
        duration: 1e12                         // ms -- effectively forever
        running: true
        loops: Animation.Infinite
    }

    // ------------------------------------------------------------------
    //  Top-N key uniforms. KeyEstimator exposes `topKeys` as a
    //  QVariantList of {name, u, v, weight, isMajor, root} maps; the
    //  shader wants four vec4(u, v, weight, 0). Build them with a tiny
    //  function so the shader binding stays declarative.
    // ------------------------------------------------------------------
    function topKeyAt(i) {
        const arr = est.topKeys
        if (!arr || arr.length <= i) return Qt.vector4d(0, 0, 0, 0)
        const k = arr[i]
        return Qt.vector4d(k.u, k.v, k.weight, 0)
    }

    ShaderEffect {
        id: torus
        anchors.fill: parent

        // Camera-driver + estimator-driven uniforms. We bind the
        // estimator's properties directly; QML keeps them current via
        // the estimateUpdated NOTIFY signal on KeyEstimator.
        property real time: root.elapsed
        property real centroidU: est.torusU
        property real centroidV: est.torusV

        property vector4d topKey0: root.topKeyAt(0)
        property vector4d topKey1: root.topKeyAt(1)
        property vector4d topKey2: root.topKeyAt(2)
        property vector4d topKey3: root.topKeyAt(3)
        property vector2d viewportPx: Qt.vector2d(width, height)

        property color keyColor:  root.keyColor
        property color backColor: root.backColor
        property color ringColor: root.ringColor

        vertexShader:   "qrc:/shaders/tonal_torus.vert.qsb"
        fragmentShader: "qrc:/shaders/tonal_torus.frag.qsb"

        // Re-fetch top-N uniforms on every estimator tick. Without this
        // hook the four topKeyN vec4 bindings would only re-evaluate
        // when est.topKeys was accessed elsewhere; QML's property system
        // doesn't see them as dependencies of `topKeys` because the
        // function call is opaque to the binding analyzer.
        Connections {
            target: est
            function onEstimateUpdated() {
                torus.topKey0 = root.topKeyAt(0)
                torus.topKey1 = root.topKeyAt(1)
                torus.topKey2 = root.topKeyAt(2)
                torus.topKey3 = root.topKeyAt(3)
            }
        }
    }

    // ------------------------------------------------------------------
    //  Top-right readout. Key name in bold Menlo; tiny confidence
    //  percentage subtitle. Matches the chrome style other visualizers
    //  use (TonnetzOverlay, SpectrumView).
    // ------------------------------------------------------------------
    Column {
        anchors.top:    parent.top
        anchors.right:  parent.right
        anchors.margins: 14
        spacing: 2

        Text {
            text: est.keyName.length > 0 ? est.keyName : "—"
            color: "white"
            font.family: "Menlo"
            font.pixelSize: 14
            font.bold: true
            horizontalAlignment: Text.AlignRight
            width: 110
        }

        Text {
            text: est.keyConfidence > 0
                  ? Math.round(est.keyConfidence * 100) + "%"
                  : ""
            color: Qt.rgba(1, 1, 1, 0.45)
            font.family: "Menlo"
            font.pixelSize: 10
            horizontalAlignment: Text.AlignRight
            width: 110
        }
    }
}
