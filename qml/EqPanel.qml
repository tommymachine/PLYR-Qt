// EqPanel — the main EQ UI. Expects the context property `eq`
// (EqController) to be available. Designed to sit inside a Popup or
// any other container; draws its own surface and chrome.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    signal closeRequested

    readonly property color bg:        Qt.rgba(0.08, 0.08, 0.10, 1.0)
    readonly property color primary:   "white"
    readonly property color muted:     Qt.rgba(1, 1, 1, 0.45)
    readonly property color veryMuted: Qt.rgba(1, 1, 1, 0.25)
    readonly property color subtle:    Qt.rgba(1, 1, 1, 0.10)
    readonly property color accent:    Qt.rgba(0.55, 0.82, 1.00, 1.0)

    // Helper: is a given preset id currently layered?
    function _layerIntensity(id) {
        for (let i = 0; i < eq.layers.length; i++)
            if (eq.layers[i].id === id) return eq.layers[i].intensity
        return -1   // not active
    }

    Rectangle {
        anchors.fill: parent
        color: root.bg
        radius: 12
        border.color: root.subtle
        border.width: 1

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 18
            spacing: 14

            // ---- Header ------------------------------------------------
            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                Text {
                    text: "EQ"
                    color: root.primary
                    font.pixelSize: 18
                    font.bold: true
                }

                Text {
                    text: eq.loadedName
                          + (eq.dirty ? "  •  unsaved changes" : "")
                    color: eq.dirty ? root.accent : root.muted
                    font.pixelSize: 11
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }

                Button {
                    text: "Close"
                    flat: true
                    onClicked: root.closeRequested()
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: root.subtle
            }

            // ---- Master controls row ----------------------------------
            RowLayout {
                Layout.fillWidth: true
                spacing: 22

                Switch {
                    text: "Enabled"
                    checked: eq.enabled
                    onToggled: eq.enabled = checked
                }

                // Preamp
                ColumnLayout {
                    spacing: 2
                    Text {
                        text: "Preamp"
                        color: root.muted
                        font.pixelSize: 10
                    }
                    RowLayout {
                        spacing: 8
                        Slider {
                            id: preampSlider
                            from: -12
                            to:    12
                            stepSize: 0.1
                            value: eq.preamp
                            implicitWidth: 140
                            onMoved: eq.preamp = value
                        }
                        Text {
                            text: (eq.preamp >= 0 ? "+" : "")
                                  + eq.preamp.toFixed(1) + " dB"
                            color: root.primary
                            font.pixelSize: 11
                            font.family: "Menlo"
                            Layout.preferredWidth: 60
                        }
                    }
                }

                // Width (global Q)
                ColumnLayout {
                    spacing: 2
                    Text {
                        text: "Width"
                        color: root.muted
                        font.pixelSize: 10
                    }
                    RowLayout {
                        spacing: 8
                        Slider {
                            from: 0.5
                            to:   2.5
                            stepSize: 0.01
                            value: eq.width
                            implicitWidth: 140
                            onMoved: eq.width = value
                        }
                        Text {
                            text: "Q " + eq.width.toFixed(2)
                            color: root.primary
                            font.pixelSize: 11
                            font.family: "Menlo"
                            Layout.preferredWidth: 60
                        }
                    }
                }

                Item { Layout.fillWidth: true }
            }

            // ---- Presets ----------------------------------------------
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 6

                Text {
                    text: "Presets  (tap to layer • press-and-hold user presets to delete)"
                    color: root.muted
                    font.pixelSize: 10
                }

                Flow {
                    Layout.fillWidth: true
                    spacing: 10

                    Repeater {
                        model: eq.presets
                        EqPresetChip {
                            presetId:    modelData.id
                            displayName: modelData.displayName
                            isBuiltin:   modelData.builtin
                            active:      root._layerIntensity(modelData.id) >= 0
                            intensity:   Math.max(0, root._layerIntensity(modelData.id))

                            onToggled: {
                                if (active) eq.deactivatePreset(modelData.id)
                                else         eq.activatePreset(modelData.id, 1.0)
                            }
                            onIntensityMoved: (v) => eq.setPresetIntensity(modelData.id, v)
                            onDeleteRequested: eq.deleteUserPreset(modelData.id)
                        }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: root.subtle
            }

            // ---- Band sliders + live viz + response curve -------------
            Item {
                id: bandArea
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: 260

                // Vertical extents of the bar region inside each slider.
                // Matches font.pixelSize:10 labels + spacing:4 in EqBandSlider
                // so the curve's ±12 dB maps onto the bar's vertical extent.
                readonly property real barTopInset:    18
                readonly property real barBottomInset: 18

                RowLayout {
                    id: sliderRow
                    anchors.fill: parent
                    spacing: 2

                    Repeater {
                        model: 10
                        EqBandSlider {
                            frequency: eq.bandFrequencies[index]
                            value:     eq.bandGains[index]
                            level:     (fft.eqBands.length > index ? fft.eqBands[index] : 0)
                            peak:      (fft.eqPeaks.length > index ? fft.eqPeaks[index] : 0)
                            active:    eq.enabled
                            Layout.fillHeight: true
                            Layout.fillWidth:  true
                            onMoved: (db) => eq.setBandGain(index, db)
                        }
                    }
                }

                // Response curve: floats over the bars, inset to the bar's
                // vertical range so the curve's 0 dB line is at bar-center.
                // No MouseArea → clicks pass through to the sliders.
                EqCurveOverlay {
                    anchors.left:   parent.left
                    anchors.right:  parent.right
                    anchors.top:    parent.top
                    anchors.bottom: parent.bottom
                    anchors.topMargin:    bandArea.barTopInset
                    anchors.bottomMargin: bandArea.barBottomInset
                    bandGains: eq.bandGains
                    bandCount: 10
                    active:    eq.enabled
                }
            }

            // ---- Footer: save / revert --------------------------------
            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Item { Layout.fillWidth: true }

                Button {
                    text: "Revert"
                    visible: eq.dirty
                    onClicked: eq.revert()
                }
                Button {
                    text: "Save"
                    visible: eq.dirty && eq.canOverwrite
                    highlighted: true
                    onClicked: eq.save()
                }
                Button {
                    text: eq.canOverwrite ? "Save As…" : "Save…"
                    visible: eq.dirty
                    highlighted: !eq.canOverwrite
                    onClicked: {
                        nameField.text = eq.suggestPresetName()
                        saveDialog.open()
                    }
                }
            }
        }
    }

    // ---- Save-As dialog --------------------------------------------------
    Dialog {
        id: saveDialog
        title: "Save EQ preset"
        standardButtons: Dialog.Ok | Dialog.Cancel
        modal: true
        // Anchor to the window's overlay so it draws on top of this Popup.
        parent: Overlay.overlay
        anchors.centerIn: parent

        ColumnLayout {
            spacing: 8
            Text {
                text: "Name this preset:"
                color: root.muted
                font.pixelSize: 11
            }
            TextField {
                id: nameField
                implicitWidth: 260
                selectByMouse: true
                onAccepted: saveDialog.accept()
            }
            Text {
                visible: saveDialog._lastError !== ""
                text: saveDialog._lastError
                color: Qt.rgba(1, 0.55, 0.55, 1)
                font.pixelSize: 10
            }
        }

        property string _lastError: ""

        onAccepted: {
            const name = nameField.text.trim()
            if (name === "") return
            if (!eq.saveAs(name)) {
                _lastError = "Name already in use — pick another."
                open()
            } else {
                _lastError = ""
            }
        }
        onRejected: _lastError = ""
    }
}
