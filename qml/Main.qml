import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root
    width: 900
    height: 600
    minimumWidth: 800
    minimumHeight: 560
    visible: true
    title: "PLYR Qt"
    color: "black"

    // PLYR palette (matches the Swift project for visual continuity)
    readonly property color bg:        "black"
    readonly property color ocean:     Qt.rgba(0.02, 0.18, 0.45, 1.0)
    readonly property color sky:       Qt.rgba(0.45, 0.78, 1.00, 1.0)
    readonly property color muted:     Qt.rgba(1.0, 1.0, 1.0, 0.40)
    readonly property color veryMuted: Qt.rgba(1.0, 1.0, 1.0, 0.25)

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ---- Header ---------------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 42
            color: root.bg

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                spacing: 8

                Text {
                    text: "PLYR"
                    color: "white"
                    font.family: "Menlo"
                    font.bold: true
                    font.pixelSize: 18
                }

                Button {
                    text: "Open Folder…"
                    // TODO: hook up a native file dialog
                }

                Item { Layout.fillWidth: true }  // spacer

                Text {
                    text: "0 tracks"
                    color: root.muted
                    font.pixelSize: 11
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: Qt.rgba(1, 1, 1, 0.1)
        }

        // ---- Split: playlist left, visualizer-placeholder right -------
        SplitView {
            Layout.fillWidth: true
            Layout.fillHeight: true

            // Left: playlist placeholder
            Rectangle {
                SplitView.preferredWidth: 420
                SplitView.minimumWidth: 320
                color: root.bg

                Text {
                    anchors.centerIn: parent
                    color: root.muted
                    text: "playlist goes here"
                }
            }

            // Right: visualizer placeholder
            Rectangle {
                SplitView.minimumWidth: 280
                color: root.bg

                Text {
                    anchors.centerIn: parent
                    color: root.muted
                    text: "visualizer goes here"
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: Qt.rgba(1, 1, 1, 0.1)
        }

        // ---- Transport (slider + controls) ----------------------------
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 96
            color: root.bg

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 6

                // Seek slider placeholder
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 28
                    color: Qt.rgba(1, 1, 1, 0.04)
                    radius: 2
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 16

                    // Now-playing card placeholder
                    Rectangle {
                        Layout.preferredWidth: 260
                        Layout.preferredHeight: 48
                        color: Qt.rgba(1, 1, 1, 0.06)
                        radius: 8

                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.leftMargin: 10
                            color: root.muted
                            text: "nothing playing"
                            font.pixelSize: 13
                        }
                    }

                    Item { Layout.fillWidth: true }

                    Button { text: "⏮"; flat: true }
                    Button { text: "▶"; flat: true; font.pixelSize: 20 }
                    Button { text: "⏭"; flat: true }

                    Item { Layout.fillWidth: true }

                    // Volume placeholder
                    Rectangle {
                        Layout.preferredWidth: 100
                        Layout.preferredHeight: 28
                        color: Qt.rgba(1, 1, 1, 0.04)
                        radius: 2
                    }
                }
            }
        }
    }
}
