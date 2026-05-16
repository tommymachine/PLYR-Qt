// Live A/V sync calibration overlay.
//
// The CoreAudio + CADisplayLink analytic formula in AudioWorker accounts
// for everything the OS exposes: audio device latency, vsync target
// time, mach host-time anchor. But there are two residual latencies the
// OS doesn't expose at all:
//
//   * GPU pipeline depth — Qt's threaded render loop typically has
//     1–2 frames between FFT push and present. ~16–33 ms.
//   * LCD panel response — pixels start changing ~5–15 ms after the
//     compositor reports them presented.
//
// Combined residual is usually +20 to +50 ms of "visualizer is
// actually ahead of perceived audio." This panel exposes
// audio.syncCalibrationMs as a slider so the user can dial it in.
// Positive = nudge the visual further into the future (FFT taps the
// pipe further ahead). Negative = pull it back.
//
// Drop into Main.qml as a transient overlay:
//   SyncTuner { anchors.bottom: parent.bottom; anchors.right: parent.right }

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root

    property real flashOpacity: 0
    property real onsetMs: 0
    width: 280
    height: 120
    radius: 10
    color: Qt.rgba(0.04, 0.04, 0.06, 0.92)
    border.color: Qt.rgba(1, 1, 1, 0.18)
    border.width: 1

    // Onset flash: lights up a circle every time AudioFeatures fires an
    // onset() signal. Compare its timing to the audible beat — if the
    // flash leads the beat, raise the slider; if it trails, lower it.
    Connections {
        target: audioFeatures
        function onOnset() {
            root.flashOpacity = 1.0
            root.onsetMs = Date.now()
        }
    }
    Behavior on flashOpacity {
        NumberAnimation { duration: 220; easing.type: Easing.OutCubic }
    }
    Timer {
        interval: 60
        running: true; repeat: true
        onTriggered: if (root.flashOpacity > 0.0) root.flashOpacity *= 0.4
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            Rectangle {
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22
                radius: 11
                color: Qt.rgba(0.45, 0.78, 1.00,
                               Math.max(0.10, root.flashOpacity))
                border.color: Qt.rgba(1, 1, 1, 0.30)
                border.width: 1
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1
                Text {
                    text: "SYNC TUNER"
                    color: "white"
                    font.family: "Iosevka"
                    font.pixelSize: 10
                    font.bold: true
                }
                Text {
                    text: "calibration: "
                          + (audio.syncCalibrationMs >= 0 ? "+" : "")
                          + audio.syncCalibrationMs.toFixed(0) + " ms"
                          + (audio.outputIsBluetooth ? "  ·  BT" : "")
                    color: Qt.rgba(1, 1, 1, 0.55)
                    font.family: "Iosevka"
                    font.pixelSize: 9
                }
            }

            Button {
                text: "▶ Test"
                flat: true
                Layout.preferredWidth: 56
                Layout.preferredHeight: 22
                onClicked: audio.playTestSinePulses(30)
                ToolTip.text: "Play a 30 s click-track (1 Hz, 1 kHz pulse) — pure tone, no file written"
                ToolTip.visible: hovered
                ToolTip.delay: 500
            }

            Button {
                text: "0"
                flat: true
                Layout.preferredWidth: 26
                Layout.preferredHeight: 22
                onClicked: audio.syncCalibrationMs = 0
                ToolTip.text: "Reset calibration to 0"
                ToolTip.visible: hovered
                ToolTip.delay: 500
            }
        }

        Slider {
            id: slider
            Layout.fillWidth: true
            from: audio.outputIsBluetooth ? -300 : -150
            to:   audio.outputIsBluetooth ?  300 :  150
            stepSize: 1
            value: audio.syncCalibrationMs
            onMoved: audio.syncCalibrationMs = value
        }

        Text {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: "dot flashes on each beat — adjust until flash + drum hit feel simultaneous"
            color: Qt.rgba(1, 1, 1, 0.35)
            font.family: "Iosevka"
            font.pixelSize: 8
            wrapMode: Text.WordWrap
        }
    }
}
