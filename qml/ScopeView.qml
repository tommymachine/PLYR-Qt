// Combined oscilloscope + vectorscope. Wraps ScopeRenderer (Qt RHI
// item) with a phase-correlation bar along the bottom and a small
// mode-toggle chip in the top-right corner.
//
// Drop into any container that gives it a size:
//   ScopeView { anchors.fill: parent; mode: ScopeView.Vectorscope }
//
// `audioFeatures` is the global QML context property exposed from
// main.cpp; we forward it to the renderer's audioSource property.

import QtQuick
import QtQuick.Controls
import PLYR

Item {
    id: root

    enum Mode { Oscilloscope, Vectorscope }
    property int mode: ScopeView.Vectorscope

    property color beamColor: "#34ff34"
    property real  sigma:     1.5
    property real  decay:     0.080
    property real  audioGain: 1.0
    property real  beamIntensity: 1.0
    property bool  stereoSeparated: true
    property real  axisRotation:  45.0

    readonly property int _corrBarH: 18
    readonly property color _panelBg: "#0a0a0a"

    ScopeRenderer {
        id: scope
        anchors.fill: parent
        anchors.bottomMargin: root._corrBarH
        audioSource: audioFeatures
        mode: root.mode === ScopeView.Vectorscope
              ? ScopeRenderer.Vectorscope
              : ScopeRenderer.Oscilloscope
        beamColor:       root.beamColor
        sigma:           root.sigma
        decay:           root.decay
        beamIntensity:   root.beamIntensity
        audioGain:       root.audioGain
        stereoSeparated: root.stereoSeparated
        axisRotation:    root.axisRotation
    }

    // Mode toggle. Top-right corner so it doesn't interfere with the
    // beam trace. Plain Text + MouseArea — matches the codebase's
    // existing chip-button style.
    Rectangle {
        id: modeChip
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 6
        width:  modeChipText.implicitWidth + 14
        height: 20
        color: Qt.rgba(0, 0, 0, 0.55)
        border.color: Qt.rgba(1, 1, 1, 0.20)
        border.width: 1
        radius: 4

        Text {
            id: modeChipText
            anchors.centerIn: parent
            text: root.mode === ScopeView.Vectorscope ? "VECTOR" : "SCOPE"
            color: "white"
            font.family: "Menlo"
            font.pixelSize: 9
            font.bold: true
        }

        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: {
                root.mode = root.mode === ScopeView.Vectorscope
                          ? ScopeView.Oscilloscope
                          : ScopeView.Vectorscope
            }
        }
    }

    // -----------------------------------------------------------------
    //  Phase-correlation bar
    // -----------------------------------------------------------------
    //  -1 ← antiphase  ·  0 (mono-uncorrelated)  ·  +1 → mono-correlated
    //  Reads audioFeatures.phase_corr live. Color of the indicator goes
    //  red below 0 (antiphase, dangerous on mono playback) and green
    //  above 0 (mono-correlated, the "safe" half of the range).
    Rectangle {
        id: corrBar
        anchors.bottom: parent.bottom
        anchors.left:   parent.left
        anchors.right:  parent.right
        height: root._corrBarH
        color:  root._panelBg
        border.color: Qt.rgba(1, 1, 1, 0.10)
        border.width: 1

        // Center tick.
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            anchors.horizontalCenter: parent.horizontalCenter
            width: 1
            height: parent.height - 6
            color: Qt.rgba(1, 1, 1, 0.18)
        }

        // -1 label (left).
        Text {
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            anchors.leftMargin: 4
            text: "-1"
            color: Qt.rgba(1, 0.4, 0.4, 0.65)
            font.family: "Menlo"
            font.pixelSize: 9
        }

        // -S annotation, just right of "-1".
        Text {
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            anchors.leftMargin: 22
            text: "-S"
            color: Qt.rgba(1, 1, 1, 0.30)
            font.family: "Menlo"
            font.pixelSize: 9
        }

        // 0 label centered.
        Text {
            anchors.verticalCenter: parent.verticalCenter
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.horizontalCenterOffset: 6
            text: "0"
            color: Qt.rgba(1, 1, 1, 0.30)
            font.family: "Menlo"
            font.pixelSize: 9
        }

        // +M annotation.
        Text {
            anchors.verticalCenter: parent.verticalCenter
            anchors.right: parent.right
            anchors.rightMargin: 22
            text: "+M"
            color: Qt.rgba(1, 1, 1, 0.30)
            font.family: "Menlo"
            font.pixelSize: 9
        }

        // +1 label (right).
        Text {
            anchors.verticalCenter: parent.verticalCenter
            anchors.right: parent.right
            anchors.rightMargin: 4
            text: "+1"
            color: Qt.rgba(0.4, 1, 0.4, 0.65)
            font.family: "Menlo"
            font.pixelSize: 9
        }

        // Indicator triangle. x maps phase_corr ∈ [-1, +1] across the bar.
        Item {
            id: indicator
            width: 8
            height: parent.height
            // Center of the bar is at width/2; corr ∈ [-1,1] maps to the
            // full width minus 12 px of side margin for the labels.
            x: {
                const w = parent.width
                const usable = w - 24
                const cx = 12 + usable * 0.5 + (audioFeatures.phase_corr * 0.5 * usable)
                return cx - 4
            }

            Canvas {
                id: indicatorCanvas
                anchors.fill: parent
                Connections {
                    target: audioFeatures
                    function onFeaturesUpdated() { indicatorCanvas.requestPaint() }
                }
                onPaint: {
                    const ctx = getContext("2d")
                    ctx.clearRect(0, 0, width, height)
                    const corr = audioFeatures.phase_corr
                    // Color: red below 0, green above 0; intensity scales
                    // with magnitude so a near-zero correlation reads as
                    // muted, not loud.
                    const hue = corr > 0 ? 0.33 : 0.0
                    const sat = Math.min(1, 0.4 + Math.abs(corr) * 0.6)
                    ctx.fillStyle = Qt.hsva(hue, sat, 1, 1)
                    ctx.beginPath()
                    ctx.moveTo(4, 2)
                    ctx.lineTo(0, height - 2)
                    ctx.lineTo(8, height - 2)
                    ctx.closePath()
                    ctx.fill()
                }
            }
        }
    }
}
