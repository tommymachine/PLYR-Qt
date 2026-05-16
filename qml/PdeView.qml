// PdeView -- Layer 4d visualizer wrapper. Two modes:
//
//   * Chladni    -- stateless vibrating-plate eigenmodes. Audio
//                   centroid drifts (m, n); bass adds a breathing
//                   perturbation; rms scales brightness.
//   * GrayScott  -- a real reaction-diffusion PDE solved on the GPU
//                   via ping-pong textures. Audio modulates (F, k);
//                   spectral-flux onsets fire a v-bump at a fresh
//                   site each beat.
//
// Drop into a sized container:
//   PdeView { anchors.fill: parent }
//
// `audioFeatures` is the QML context property set up by main.cpp.

import QtQuick
import QtQuick.Controls
import PLYR

Item {
    id: root

    property var  audioSource: audioFeatures
    // Default to Gray-Scott because it's the more spectacular of the
    // two -- a fresh seed mitotic spots is more visually striking
    // than the still-image-with-breathing of Chladni at first sight.
    property int  mode: PdeView.GrayScott

    Rectangle {
        anchors.fill: parent
        color: "#070A12"
    }

    PdeView {
        id: pde
        anchors.fill: parent
        audioSource: root.audioSource
        mode:        root.mode
        // Tunables. Default Gray-Scott parameters land in the "mitosis
        // spots" basin (Pearson 1993); the audio modulation shifts F
        // by up to +0.020 and k by up to +0.005 from the bass / treb
        // attack envelopes.
        gsFeedBase: 0.035
        gsKillBase: 0.062
        gsDu:       0.16
        gsDv:       0.08
        gsDt:       1.0
        gsStepsPerFrame: 8
    }

    // -------------------- Mode-cycle chip (top-right) ----------------
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
            onClicked: {
                root.mode = (root.mode === PdeView.GrayScott)
                    ? PdeView.Chladni
                    : PdeView.GrayScott
            }
        }
        Text {
            id: nameLabel
            anchors.centerIn: parent
            text: (root.mode === PdeView.GrayScott
                   ? "Gray-Scott"
                   : "Chladni") + "  ›"
            color: "white"
            font.family: "Iosevka"
            font.pixelSize: 10
        }
    }

    // -------------------- Reset button (bottom-right) ----------------
    // Only visible in GrayScott mode -- Chladni is stateless.
    Rectangle {
        id: resetBtn
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        anchors.margins: 12
        height: 28
        width: 64
        visible: root.mode === PdeView.GrayScott
        color: Qt.rgba(0, 0, 0, 0.5)
        border.color: Qt.rgba(1, 1, 1, 0.2)
        radius: 6

        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: pde.resetGrayScott()
        }
        Text {
            anchors.centerIn: parent
            text: "Reset"
            color: "white"
            font.family: "Iosevka"
            font.pixelSize: 10
        }
    }

    // -------------------- (F, k) chip (bottom-left, GS only) ---------
    Rectangle {
        id: paramChip
        anchors.bottom: parent.bottom
        anchors.left:   parent.left
        anchors.margins: 12
        height: 22
        width:  fkText.width + 16
        visible: root.mode === PdeView.GrayScott
        color: Qt.rgba(0, 0, 0, 0.45)
        border.color: Qt.rgba(1, 1, 1, 0.15)
        radius: 4
        Text {
            id: fkText
            anchors.centerIn: parent
            text: "F=" + pde.gsLastFeed.toFixed(4)
                + "  k=" + pde.gsLastKill.toFixed(4)
            color: Qt.rgba(1, 1, 1, 0.5)
            font.family: "Iosevka"
            font.pixelSize: 9
        }
    }

    // -------------------- Subtitle (bottom-left, Chladni) ------------
    Text {
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.margins: 12
        visible: root.mode === PdeView.Chladni
        text: "CHLADNI PLATE * (m, n) = "
              + pde.chladniM.toFixed(1) + ", " + pde.chladniN.toFixed(1)
        color: Qt.rgba(1, 1, 1, 0.4)
        font.family: "Iosevka"
        font.pixelSize: 9
    }
}
