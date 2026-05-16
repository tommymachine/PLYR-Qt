// MilkdropView wrapper — Layer 3b. Hosts the C++ MilkdropView (a
// QQuickRhiItem registered as a QML element via QML_ELEMENT) plus
// chrome: a top-right preset cycle chip and a bottom-left error
// overlay if a preset failed to compile.
//
// New presets: drop a `.milk` text file into presets/, list it in
// CMakeLists.txt under the `milkdrop_presets` resource bundle, and
// add a `qrc:/presets/...` entry to the `bundledPresets` array
// below. Rebuild and the cycle button will pick it up.

import QtQuick
import QtQuick.Controls
import PLYR

Item {
    id: root

    // Source of audio scalars (bass/mid/treb/_att). Default is the
    // global context property set by main.cpp.
    property var audioSource: audioFeatures

    // The bundled preset library. Keep in sync with CMakeLists.txt's
    // milkdrop_presets resource block.
    readonly property var bundledPresets: [
        { name: "Simple Zoom",   path: "qrc:/presets/01_simple_zoom.milk",
          desc: "Minimal — global zoom + slow rotation" },
        { name: "Swirl",         path: "qrc:/presets/02_swirl.milk",
          desc: "Bass-driven angular swirl" },
        { name: "Kaleidoscope",  path: "qrc:/presets/03_kaleidoscope.milk",
          desc: "Radial pulse + accumulated q1 phase" }
    ]

    // Start on Swirl — it shows the most preset features at idle.
    property int currentIndex: 1

    MilkdropView {
        id: viz
        anchors.fill: parent
        audioSource: root.audioSource
        presetPath: root.bundledPresets[root.currentIndex].path
    }

    // ---------------- Cycle button (top-right) -------------------------
    Rectangle {
        id: cycleBtn
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 12
        height: 28
        width: nameLabel.width + 28
        color: Qt.rgba(0, 0, 0, 0.5)
        border.color: Qt.rgba(1, 1, 1, 0.2)
        radius: 6

        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: root.currentIndex =
                (root.currentIndex + 1) % root.bundledPresets.length
        }
        Text {
            id: nameLabel
            anchors.centerIn: parent
            text: root.bundledPresets[root.currentIndex].name + "  ›"
            color: "white"
            font.family: "Iosevka"
            font.pixelSize: 10
        }
    }

    // ---------------- Description tag (bottom-left) --------------------
    Text {
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.margins: 12
        text: root.bundledPresets[root.currentIndex].desc
        color: Qt.rgba(1, 1, 1, 0.4)
        font.family: "Iosevka"
        font.pixelSize: 9
    }

    // ---------------- Error overlay (bottom, only if set) --------------
    Rectangle {
        visible: viz.lastError !== undefined && viz.lastError.length > 0
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottomMargin: 32
        width: Math.min(parent.width - 40, errLabel.implicitWidth + 24)
        height: errLabel.implicitHeight + 12
        color: Qt.rgba(0.4, 0.05, 0.05, 0.85)
        border.color: Qt.rgba(1, 0.3, 0.3, 0.4)
        radius: 4

        Text {
            id: errLabel
            anchors.centerIn: parent
            width: parent.width - 16
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
            text: viz.lastError
            color: "white"
            font.family: "Iosevka"
            font.pixelSize: 10
        }
    }

    // ---------------- Keyboard cycling ---------------------------------
    Keys.onRightPressed: currentIndex =
        (currentIndex + 1) % bundledPresets.length
    Keys.onLeftPressed: currentIndex =
        (currentIndex - 1 + bundledPresets.length) % bundledPresets.length
    focus: true
}
