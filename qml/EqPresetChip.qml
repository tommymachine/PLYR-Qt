// EqPresetChip — toggleable pill for layering a preset, with an
// intensity slider that appears when the chip is active. Press-and-hold
// on a user preset (non-builtin) triggers deleteRequested.

import QtQuick
import QtQuick.Controls

Item {
    id: root

    property string presetId:    ""
    property string displayName: ""
    property bool   active:      false
    property real   intensity:   1.0
    property bool   isBuiltin:   true

    signal toggled
    signal intensityMoved(real value)
    signal deleteRequested

    readonly property color accent: Qt.rgba(0.45, 0.78, 1.00, 1.0)

    implicitWidth:  chip.width
    implicitHeight: chip.height + (root.active ? 4 + slider.height : 0)

    Rectangle {
        id: chip
        width: Math.max(110, label.paintedWidth + 24)
        height: 28
        radius: 14
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter

        color: root.active ? Qt.rgba(0.45, 0.78, 1.00, 0.20)
                           : Qt.rgba(1, 1, 1, 0.06)
        border.color: root.active ? root.accent : Qt.rgba(1, 1, 1, 0.16)
        border.width: 1

        Text {
            id: label
            anchors.centerIn: parent
            text: root.displayName
            color: root.active ? "white" : Qt.rgba(1, 1, 1, 0.72)
            font.pixelSize: 12
            font.bold: root.active
        }

        MouseArea {
            anchors.fill: parent
            onClicked: root.toggled()
            onPressAndHold: if (!root.isBuiltin) root.deleteRequested()
        }
    }

    Slider {
        id: slider
        visible: root.active
        anchors.top: chip.bottom
        anchors.topMargin: 4
        anchors.horizontalCenter: parent.horizontalCenter
        width: chip.width
        height: visible ? 14 : 0
        from: 0
        to:   1
        value: root.intensity
        onMoved: root.intensityMoved(value)
    }
}
