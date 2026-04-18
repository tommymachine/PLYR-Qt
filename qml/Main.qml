import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

ApplicationWindow {
    id: root
    width: 900
    height: 600
    minimumWidth: 800
    minimumHeight: 560
    visible: true
    title: "PLYR Qt"
    color: "black"

    // PLYR palette (mirrors the Swift version).
    readonly property color bg:          "black"
    readonly property color ocean:       Qt.rgba(0.02, 0.18, 0.45, 1.0)
    readonly property color sky:         Qt.rgba(0.45, 0.78, 1.00, 1.0)
    readonly property color accent:      Qt.rgba(0.55, 0.82, 1.00, 1.0)
    readonly property color primary:     "white"
    readonly property color muted:       Qt.rgba(1.0, 1.0, 1.0, 0.40)
    readonly property color veryMuted:   Qt.rgba(1.0, 1.0, 1.0, 0.25)
    readonly property color subtleLine:  Qt.rgba(1.0, 1.0, 1.0, 0.10)

    FolderDialog {
        id: folderDialog
        title: "Choose a folder of FLAC files"
        onAccepted: playlist.openFolderUrl(selectedFolder)
    }

    // Playback: `audio` is a C++ AudioEngine instance (main.cpp ctx-prop).
    // It wraps QMediaPlayer + QAudioBufferOutput so the FFT visualizer
    // gets PCM samples while normal playback runs through QAudioOutput.
    readonly property bool isPlaying: audio.playing

    // Drives the metadata panel (opened via chevron, dismissed by click-out).
    property bool metadataExpanded: false

    function formatDuration(seconds) {
        if (!isFinite(seconds) || seconds < 0) return "0:00"
        const m = Math.floor(seconds / 60)
        const s = Math.floor(seconds) % 60
        return m + ":" + (s < 10 ? "0" + s : s)
    }

    // Full-window tap-catcher: any click while the overlay is open
    // dismisses. Sits below the overlay in z-order so clicks that land
    // on the overlay itself are handled by its own MouseArea first.
    MouseArea {
        z: 150
        anchors.fill: parent
        visible: root.metadataExpanded
        onClicked: root.metadataExpanded = false
    }

    // ------------------------------------------------------------------
    // Metadata overlay: sits directly above the now-playing card,
    // same width, extending upward. Taller because it includes the
    // metadata rows. Fades in instantly when metadataExpanded = true.
    // Positioned by absolute x/y because `transport` lives inside the
    // ColumnLayout and can't be anchor-targeted from this outer Item.
    // ------------------------------------------------------------------
    Rectangle {
        id: metadataOverlay
        z: 200
        visible: root.metadataExpanded && playlist.hasCurrent
        width: 260
        height: 260 + 48   // metadata rows + card-sized bottom strip
        // bottom-left corner of the overlay matches the now-playing card:
        //   x = transport-row left + padding
        //   y = window bottom - transport height + padding
        x: 12
        y: root.height - transport.height + 12 - height + 48
        color: Qt.rgba(0.08, 0.08, 0.10, 0.92)
        radius: 10
        border.color: Qt.rgba(1, 1, 1, 0.10)
        border.width: 1

        // Soak up clicks that land on the overlay itself — user wants
        // "any click dismisses", including on the overlay.
        MouseArea {
            anchors.fill: parent
            onClicked: root.metadataExpanded = false
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // Metadata rows
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.margins: 14
                spacing: 6

                Repeater {
                    model: [
                        { k: "ALBUM",      v: playlist.currentAlbum },
                        { k: "DISC",       v: String(playlist.currentDiscNumber) },
                        { k: "TRACK",      v: String(playlist.currentTrackNumber) },
                        { k: "COMPOSER",   v: playlist.currentComposer },
                        { k: "RECORDED",   v: playlist.currentRecordingDateDisplay },
                        { k: "RELEASED",   v: playlist.currentYear },
                        { k: "GENRE",      v: playlist.currentGenre }
                    ]

                    delegate: RowLayout {
                        Layout.fillWidth: true
                        visible: modelData.v !== ""
                        spacing: 8

                        Text {
                            text: modelData.k
                            color: Qt.rgba(1, 1, 1, 0.45)
                            font.family: "Menlo"
                            font.pixelSize: 9
                            font.bold: true
                            Layout.preferredWidth: 70
                        }
                        Text {
                            Layout.fillWidth: true
                            text: modelData.v
                            color: "white"
                            font.pixelSize: 11
                            wrapMode: Text.WordWrap
                        }
                    }
                }
                Item { Layout.fillHeight: true }  // soft bottom fill
            }

            // Faux-card bottom strip that visually merges with the real
            // now-playing card once the overlay is dismissed.
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 48
                color: Qt.rgba(1, 1, 1, 0.06)
                radius: 8

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 6
                    spacing: 6

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        Text {
                            Layout.fillWidth: true
                            text: playlist.currentTitle
                            color: root.primary
                            font.pixelSize: 13
                            font.bold: true
                            elide: Text.ElideRight
                        }
                        Text {
                            Layout.fillWidth: true
                            text: playlist.currentArtist
                            color: root.muted
                            font.pixelSize: 10
                            elide: Text.ElideRight
                        }
                    }

                    Rectangle {
                        Layout.preferredWidth: 20
                        Layout.preferredHeight: 20
                        color: "transparent"
                        Text {
                            anchors.centerIn: parent
                            text: "⌄"
                            color: root.muted
                            font.pixelSize: 13
                            rotation: 180      // flipped = collapse
                        }
                    }
                }
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ---- Header --------------------------------------------------
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
                    color: root.primary
                    font.family: "Menlo"
                    font.bold: true
                    font.pixelSize: 18
                }

                Button {
                    text: "Open Folder…"
                    onClicked: folderDialog.open()
                }

                Text {
                    visible: playlist.isScanning
                    text: "Scanning…"
                    color: root.muted
                    font.pixelSize: 11
                }

                Text {
                    visible: !playlist.isScanning && playlist.folderPath !== ""
                    text: playlist.folderPath
                    color: root.muted
                    font.pixelSize: 11
                    elide: Text.ElideMiddle
                    Layout.fillWidth: true
                }

                Item { Layout.fillWidth: true }

                Text {
                    text: playlist.count + " tracks"
                    color: root.muted
                    font.pixelSize: 11
                }
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: root.subtleLine }

        // ---- Split: playlist left, visualizer placeholder right -----
        SplitView {
            Layout.fillWidth: true
            Layout.fillHeight: true

            Rectangle {
                SplitView.preferredWidth: 420
                SplitView.minimumWidth: 320
                color: root.bg

                ListView {
                    id: listView
                    anchors.fill: parent
                    clip: true
                    model: playlist
                    spacing: 0

                    // Disc-based section headers.
                    section.property: "discFolder"
                    section.criteria: ViewSection.FullString
                    section.delegate: Rectangle {
                        width: ListView.view.width
                        height: 28
                        color: root.bg
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.leftMargin: 14
                            color: Qt.rgba(1, 1, 1, 0.55)
                            font.pixelSize: 11
                            font.bold: true
                            text: {
                                const m = /^CD(\d+)_(.*)$/.exec(section)
                                return m
                                    ? "Disc " + parseInt(m[1]) + " · " + m[2].replace(/_/g, " ")
                                    : section.replace(/_/g, " ")
                            }
                        }
                    }

                    delegate: Rectangle {
                        id: row
                        width: ListView.view.width
                        height: 48
                        readonly property bool current: (index === playlist.currentIndex)
                        color: current ? Qt.rgba(0.05, 0.10, 0.20, 1.0) : root.bg

                        MouseArea {
                            anchors.fill: parent
                            onDoubleClicked: playlist.currentIndex = index
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 12
                            anchors.topMargin: 4
                            anchors.bottomMargin: 4
                            spacing: 10

                            Item {
                                Layout.preferredWidth: 24
                                Layout.fillHeight: true
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: row.current
                                          ? "▶"
                                          : String(trackNumber).padStart(2, "0")
                                    color: row.current ? root.accent
                                                       : Qt.rgba(1, 1, 1, 0.28)
                                    font.pixelSize: row.current ? 11 : 10
                                    font.family: "Menlo"
                                }
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 8

                                    Text {
                                        Layout.fillWidth: true
                                        text: title
                                        color: row.current
                                               ? root.primary
                                               : Qt.rgba(1, 1, 1, 0.40)
                                        font.pixelSize: 13
                                        font.bold: row.current
                                        elide: Text.ElideRight
                                    }

                                    Text {
                                        text: formatDuration(duration)
                                        color: row.current
                                               ? Qt.rgba(1, 1, 1, 0.65)
                                               : Qt.rgba(1, 1, 1, 0.25)
                                        font.pixelSize: 10
                                        font.family: "Menlo"
                                    }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 8

                                    Text {
                                        Layout.fillWidth: true
                                        text: artist
                                        color: row.current
                                               ? Qt.rgba(root.accent.r,
                                                          root.accent.g,
                                                          root.accent.b, 0.85)
                                               : Qt.rgba(1, 1, 1, 0.25)
                                        font.pixelSize: 10
                                        elide: Text.ElideRight
                                    }

                                    Text {
                                        visible: composer !== ""
                                        text: composer
                                        color: row.current
                                               ? Qt.rgba(root.accent.r,
                                                          root.accent.g,
                                                          root.accent.b, 0.85)
                                               : Qt.rgba(1, 1, 1, 0.25)
                                        font.pixelSize: 10
                                        elide: Text.ElideRight
                                    }
                                }
                            }
                        }
                    }

                    ScrollBar.vertical: ScrollBar { }
                }
            }

            // Right: visualizer (Canvas-based initial pass).
            Rectangle {
                SplitView.minimumWidth: 280
                color: root.bg

                Canvas {
                    id: viz
                    anchors.fill: parent

                    readonly property int bandCount: 16
                    readonly property int segCount:  110
                    readonly property real segDecay: 0.98
                    readonly property real bandDecay: 0.95
                    readonly property real barGap: 0.18
                    readonly property real segGap: 0.28
                    readonly property real taperK: 0.7

                    readonly property color ocean: root.ocean
                    readonly property color sky:   root.sky
                    readonly property color peak:  Qt.rgba(0.87, 0.87, 0.87, 1.0)

                    Timer {
                        interval: 16   // ~60 Hz
                        running: true
                        repeat: true
                        onTriggered: viz.requestPaint()
                    }

                    onPaint: {
                        const ctx = getContext("2d")
                        const W = width, H = height
                        if (W <= 0 || H <= 0) return

                        ctx.fillStyle = "black"
                        ctx.fillRect(0, 0, W, H)

                        const data = fft.bandsAndPeaks()
                        if (!data || data.length < 2 * bandCount) return

                        // Pre-compute band slot widths (geometric decay).
                        const bandDenom = 1.0 - Math.pow(bandDecay, bandCount)

                        // Pre-compute segment heights (normalized sum = 1).
                        const segTotal = (1.0 - Math.pow(segDecay, segCount)) /
                                         (1.0 - segDecay)

                        for (let bi = 0; bi < bandCount; ++bi) {
                            const mag  = data[bi]
                            const pk   = data[bandCount + bi]
                            const slotCum = (1.0 - Math.pow(bandDecay, bi)) / bandDenom
                            const slotW   = Math.pow(bandDecay, bi) * (1.0 - bandDecay) / bandDenom
                            const slotCenter = (slotCum + slotW * 0.5) * W

                            let cumH = 0
                            for (let si = 0; si < segCount; ++si) {
                                const segH  = Math.pow(segDecay, si) / segTotal
                                const segFill = segH * (1 - segGap)
                                const yBot = cumH * H
                                const yTop = (cumH + segFill) * H
                                const segMid = cumH + segH * 0.5
                                cumH += segH

                                const lit    = segMid <= mag
                                const isPeak = (pk > 0.001) && (segMid - segH*0.5 <= pk && pk < segMid + segH*0.5)

                                if (!lit && !isPeak) continue

                                // Taper the bar width at this y.
                                const yNorm = 1.0 - (cumH - segH * 0.5)   // y=0 at top, 1 at bottom in UV; invert
                                const taper = Math.exp(-taperK * (1.0 - yNorm))
                                const halfW = slotW * W * (1.0 - barGap) * 0.5 * taper

                                // Map yBot / yTop so that y grows downward on canvas.
                                const canvasYTop = H - yTop
                                const canvasYBot = H - yBot

                                // Gradient color (ocean → sky) for this segment's t value.
                                const t = si / (segCount - 1)
                                const r = ocean.r + t * (sky.r - ocean.r)
                                const g = ocean.g + t * (sky.g - ocean.g)
                                const b = ocean.b + t * (sky.b - ocean.b)

                                if (isPeak) {
                                    // Grayed-out version for peak.
                                    const Y = 0.2126 * r + 0.7152 * g + 0.0722 * b
                                    ctx.fillStyle = Qt.rgba(Y, Y, Y, 1.0)
                                } else {
                                    ctx.fillStyle = Qt.rgba(r, g, b, 1.0)
                                }

                                ctx.fillRect(
                                    slotCenter - halfW,
                                    canvasYTop,
                                    halfW * 2,
                                    canvasYBot - canvasYTop
                                )
                            }
                        }
                    }
                }
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: root.subtleLine }

        // ---- Transport ------------------------------------------------
        Rectangle {
            id: transport
            Layout.fillWidth: true
            Layout.preferredHeight: 96
            color: root.bg

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 6

                // Seek bar — custom tapered slider.
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Text {
                        text: formatDuration(
                                  seekSlider.editing
                                  ? seekSlider.value
                                  : audio.position / 1000.0)
                        color: root.muted
                        font.family: "Menlo"
                        font.pixelSize: 10
                        Layout.preferredWidth: 48
                        horizontalAlignment: Text.AlignRight
                    }

                    PLYRSeekSlider {
                        id: seekSlider
                        Layout.fillWidth: true
                        Layout.preferredHeight: 28
                        enabled: playlist.hasCurrent
                        property bool editing: false
                        from: 0
                        to:   Math.max(1, audio.duration / 1000.0)
                        value: editing
                               ? value    // preserve preview
                               : audio.position / 1000.0
                        onEditingStarted: editing = true
                        onEditingEnded:   editing = false
                        onMoved: (newValue) => {
                            audio.position = newValue * 1000
                        }
                    }

                    Text {
                        text: formatDuration(audio.duration / 1000.0)
                        color: root.muted
                        font.family: "Menlo"
                        font.pixelSize: 10
                        Layout.preferredWidth: 48
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 16

                    // Now-playing card — marquee title + artist + chevron toggle.
                    Rectangle {
                        id: nowCard
                        Layout.preferredWidth: 260
                        Layout.preferredHeight: 48
                        color: Qt.rgba(1, 1, 1, 0.06)
                        radius: 8

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 6
                            spacing: 6

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2

                                MarqueeText {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 18
                                    text:  playlist.hasCurrent ? playlist.currentTitle : "nothing playing"
                                    color: playlist.hasCurrent ? root.primary : root.muted
                                    font.pixelSize: 13
                                    font.bold: playlist.hasCurrent
                                }

                                Text {
                                    Layout.fillWidth: true
                                    text: playlist.currentArtist
                                    color: root.muted
                                    font.pixelSize: 10
                                    elide: Text.ElideRight
                                    visible: text !== ""
                                }
                            }

                            // Chevron — click-to-expand (overlay wired below).
                            Rectangle {
                                id: chevBtn
                                Layout.preferredWidth: 20
                                Layout.preferredHeight: 20
                                color: "transparent"
                                radius: 10
                                enabled: playlist.hasCurrent

                                Text {
                                    anchors.centerIn: parent
                                    text: "⌄"
                                    color: root.muted
                                    font.pixelSize: 13
                                    rotation: root.metadataExpanded ? 180 : 0
                                    Behavior on rotation {
                                        NumberAnimation { duration: 180 }
                                    }
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    onClicked: root.metadataExpanded =
                                               !root.metadataExpanded
                                }
                            }
                        }
                    }

                    Item { Layout.fillWidth: true }

                    Button {
                        text: "⏮"
                        flat: true
                        enabled: playlist.hasCurrent
                        onClicked: playlist.previous()
                    }
                    Button {
                        text: root.isPlaying ? "⏸" : "▶"
                        flat: true
                        font.pixelSize: 20
                        enabled: playlist.hasCurrent
                        onClicked: {
                            if (root.isPlaying) audio.pause()
                            else audio.play()
                        }
                    }
                    Button {
                        text: "⏭"
                        flat: true
                        enabled: playlist.hasCurrent
                        onClicked: playlist.next()
                    }

                    Item { Layout.fillWidth: true }

                    RowLayout {
                        Layout.preferredWidth: 140
                        spacing: 4

                        Text { text: "🔈"; color: root.muted; font.pixelSize: 11 }
                        PLYRSeekSlider {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 24
                            from: 0; to: 1
                            commitOnDrag: true
                            value: audio.volume
                            onMoved: (v) => audio.volume = v
                        }
                        Text { text: "🔊"; color: root.muted; font.pixelSize: 11 }
                    }
                }
            }
        }
    }
}
