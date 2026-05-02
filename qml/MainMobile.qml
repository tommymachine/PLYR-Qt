// Mobile (phone-portrait) layout for PLYR.
//
// Two-state UI:
//   - "viz"    : visualizer fills the screen, transport docked at bottom,
//                a screen-wide pull-tab above the transport. The viz
//                ShaderEffect + FFT refresh Timer only run here, so
//                when the user is browsing the battery is idle.
//   - "browse" : the transport slides to the TOP of the screen and the
//                playlist fills the rest. The viz is fully unmounted
//                (`visible: false`) so it doesn't eat GPU cycles.
//
// Tapping the pull-tab (or the currently-playing card in the transport)
// switches to browse. Tapping a track commits it and slides back to viz.
// Tapping the title while in viz shows the metadata sheet.

import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

ApplicationWindow {
    id: root
    visible: true
    width:  390                // iPhone-ish default; a real device resizes us
    height: 844
    color:  "black"
    title:  "Concerto"

    // ----- Palette (mirrors desktop) ---------------------------------
    readonly property color bg:         "black"
    readonly property color ocean:      Qt.rgba(0.02, 0.18, 0.45, 1.0)
    readonly property color sky:        Qt.rgba(0.45, 0.78, 1.00, 1.0)
    readonly property color accent:     Qt.rgba(0.55, 0.82, 1.00, 1.0)
    readonly property color primary:    "white"
    readonly property color muted:      Qt.rgba(1.0, 1.0, 1.0, 0.40)
    readonly property color subtleLine: Qt.rgba(1.0, 1.0, 1.0, 0.10)

    // ----- State -----------------------------------------------------
    // "viz" (default) or "browse".
    property string mode: "viz"
    readonly property bool isBrowse: mode === "browse"
    readonly property int  slideMs:  300

    // Mini-player: transport collapses to a single row of (play/pause +
    // title + artist). Toggled by dragging the pull-tab down / up.
    property bool transportMinimized: false

    // Tap-expanded metadata sheet (the tap-analogue of desktop hover).
    property bool metadataExpanded: false

    // Sizes — touch-friendly.
    readonly property int transportH:    180  // full: title + seek + buttons
    readonly property int transportMiniH: 56  // minimized: play/pause + info
    readonly property int pullTabH:       28  // grip strip (viz mode only)
    readonly property int bottomBarH:     40  // persistent toggle strip

    readonly property int currentTransportH:
        root.transportMinimized ? root.transportMiniH : root.transportH

    // Entering browse always shows the full transport up top — the mini
    // is only useful while the visualizer is on screen.
    onModeChanged: if (mode === "browse") transportMinimized = false

    function formatDuration(seconds) {
        if (!isFinite(seconds) || seconds < 0) return "0:00"
        const m = Math.floor(seconds / 60)
        const s = Math.floor(seconds) % 60
        return m + ":" + (s < 10 ? "0" + s : s)
    }

    FolderDialog {
        id: folderDialog
        title: "Choose a folder of FLAC files"
        onAccepted: playlist.openFolderUrl(selectedFolder)
    }

    // Recent-folders dropdown — opened by long-press on the "Open…" button.
    // Structure mirrors Main.qml's recentsMenu.
    Menu {
        id: recentsMenu

        MenuItem {
            text: "Open Folder…"
            onTriggered: folderDialog.open()
        }

        MenuSeparator {
            visible: playlist.recentFolders.length > 0
        }

        Instantiator {
            model: playlist.recentFolders
            delegate: MenuItem {
                required property var modelData
                required property int index
                text: {
                    const parts = modelData.split("/").filter(p => p.length > 0)
                    const name = parts[parts.length - 1] || modelData
                    return "/" + name
                }
                onTriggered: playlist.openFolder(modelData)
            }
            onObjectAdded: (index, object) => recentsMenu.insertItem(2 + index, object)
            onObjectRemoved: (index, object) => recentsMenu.removeItem(object)
        }

        MenuSeparator {
            visible: playlist.recentFolders.length > 0
        }

        MenuItem {
            text: "Clear Recents"
            enabled: playlist.recentFolders.length > 0
            onTriggered: playlist.clearRecents()
        }
    }

    // ----- Visualizer (viz mode only) --------------------------------
    ShaderEffect {
        id: viz
        visible: !root.isBrowse
        anchors.left:   parent.left
        anchors.right:  parent.right
        anchors.top:    parent.top
        anchors.bottom: pullTab.top   // stops just above the pull-tab

        property vector4d b0: fft.b0
        property vector4d b1: fft.b1
        property vector4d b2: fft.b2
        property vector4d b3: fft.b3
        property vector4d p0: fft.p0
        property vector4d p1: fft.p1
        property vector4d p2: fft.p2
        property vector4d p3: fft.p3

        property color oceanColor: root.ocean
        property color skyColor:   root.sky
        property real  bandDecay:  0.95
        property real  segDecay:   0.98
        property real  barGap:     0.18
        property real  segGap:     0.28
        property real  taperK:     0.7
        property real  segCountF:  110.0
        property real  bandCountF: 16.0

        vertexShader:   "qrc:/shaders/viz.vert.qsb"
        fragmentShader: "qrc:/shaders/viz.frag.qsb"
    }

    // Only pump FFT when viz is actually visible — saves the whole
    // render+kiss_fft loop on mobile when the user is browsing.
    Timer {
        interval: 16
        running:  !root.isBrowse && Qt.application.state === Qt.ApplicationActive
        repeat:   true
        onTriggered: fft.refresh()
    }

    // ----- Pull tab (viz mode only, sits above transport) ------------
    // Anchored to transport.top so it follows as transport animates.
    // Fades out in browse mode so it doesn't steal touches from the list.
    Rectangle {
        id: pullTab
        color: Qt.rgba(0, 0, 0, 0.92)
        height: root.pullTabH
        width:  parent.width
        anchors.bottom: transport.top
        opacity: root.isBrowse ? 0 : 1
        Behavior on opacity { NumberAnimation { duration: root.slideMs / 2 } }

        Rectangle {                     // grip pill
            width: 48; height: 4
            radius: 2
            color: Qt.rgba(1, 1, 1, 0.55)
            anchors.centerIn: parent
        }

        // Vertical gesture detector:
        //   drag down → minimize transport (if currently full)
        //   drag up   → either restore (if minimized) or open browse
        //   simple tap → open browse (matches the old behaviour)
        MouseArea {
            anchors.fill: parent
            enabled: !root.isBrowse
            property int _pressY: 0
            readonly property int _thresh: 12

            onPressed: (m) => _pressY = m.y
            onReleased: (m) => {
                const dy = m.y - _pressY
                if (dy > _thresh) {
                    // pulled down
                    if (!root.transportMinimized) root.transportMinimized = true
                } else if (dy < -_thresh) {
                    // pulled up
                    if (root.transportMinimized) root.transportMinimized = false
                    else                         root.mode = "browse"
                } else {
                    // treated as tap
                    if (root.transportMinimized) root.transportMinimized = false
                    else                         root.mode = "browse"
                }
            }
        }
    }

    // ----- Persistent bottom bar (both modes) ------------------------
    // Always-visible strip at the very bottom of the screen. Two filled
    // triangles at width/3 and 2*width/3 point up in viz mode (= "open
    // the browser") and down in browse mode (= "close, back to viz").
    // Both arrows toggle, giving thumbs-on-either-hand a target.
    Rectangle {
        id: bottomBar
        // Deep-blue-tinted dark, slightly lifted off the black so the
        // toggle strip reads as a distinct control surface.
        color: Qt.rgba(0.05, 0.08, 0.14, 1.0)
        height: root.bottomBarH
        width:  parent.width
        anchors.bottom: parent.bottom

        // Entire strip is one tap target — the two triangles are purely
        // visual, centered at 1/3 and 2/3 so either thumb lands on one
        // naturally, but anywhere on the bar activates the toggle.
        MouseArea {
            anchors.fill: parent
            onClicked: root.mode = root.isBrowse ? "viz" : "browse"
        }

        // Single centred triangle — stretched wide and dimmed so the
        // bar reads as "there is a control here" without shouting.
        Text {
            id: toggleIcon
            text:  root.isBrowse ? "▼" : "▲"
            color: Qt.rgba(1, 1, 1, 0.42)
            font.pixelSize: 18
            font.bold: true
            anchors.centerIn: parent
            transform: Scale {
                xScale: 20
                origin.x: toggleIcon.implicitWidth  / 2
                origin.y: toggleIcon.implicitHeight / 2
            }
        }
    }

    // ----- Transport (animates between bottom and top) ---------------
    // y is the ONLY property that changes when mode switches; all content
    // inside is stable so nothing tears during the slide.
    Rectangle {
        id: transport
        color:  Qt.rgba(0, 0, 0, 0.92)
        width:  parent.width
        height: root.currentTransportH
        Behavior on height { NumberAnimation { duration: root.slideMs; easing.type: Easing.OutCubic } }

        // In viz mode, transport parks directly above the bottom bar.
        // The pull-tab is anchored to transport.top so it rides along
        // automatically — no extra gap needed below the transport.
        y: root.isBrowse
            ? 0
            : parent.height - root.currentTransportH - root.bottomBarH
        Behavior on y { NumberAnimation { duration: root.slideMs; easing.type: Easing.OutCubic } }

        // ---- Minimized layout: play/pause + title/artist ------------
        // Visible when root.transportMinimized is true; fades through
        // as height animates.
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin:  12
            anchors.rightMargin: 12
            anchors.topMargin:    8
            anchors.bottomMargin: 8
            spacing: 10
            opacity: root.transportMinimized ? 1 : 0
            visible: opacity > 0.01
            Behavior on opacity { NumberAnimation { duration: root.slideMs / 2 } }

            Button {
                Layout.preferredWidth:  38
                Layout.preferredHeight: 38
                flat: true
                font.pixelSize: 20
                text: audio.playing ? "⏸" : "▶"
                enabled: playlist.hasCurrent
                onClicked: {
                    if (audio.playing) audio.pause()
                    else               audio.play()
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1

                MarqueeText {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 18
                    text:  playlist.hasCurrent ? playlist.currentTitle : "nothing playing"
                    color: playlist.hasCurrent ? root.primary : root.muted
                    font.pixelSize: 13
                    font.bold: true
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
        }

        // ---- Full layout: title + seek + buttons --------------------
        // Visible when NOT minimized (and always while in browse mode,
        // where there's no mini player). Fades out as the mini appears.
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 14
            spacing: 10
            opacity: root.transportMinimized ? 0 : 1
            visible: opacity > 0.01
            Behavior on opacity { NumberAnimation { duration: root.slideMs / 2 } }

            // Title / artist row — tapping title toggles metadata sheet.
            // In browse mode, the chevron on the right closes browse.
            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2

                    MarqueeText {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 22
                        text:  playlist.hasCurrent ? playlist.currentTitle : "nothing playing"
                        color: playlist.hasCurrent ? root.primary : root.muted
                        font.pixelSize: 15
                        font.bold: true
                    }
                    Text {
                        Layout.fillWidth: true
                        text: playlist.currentArtist
                        color: root.muted
                        font.pixelSize: 11
                        elide: Text.ElideRight
                        visible: text !== ""
                    }
                }

                // Right-hand icon: "ℹ︎" while in viz (tap → metadata),
                // "▼" while in browse (tap → back to viz).
                Rectangle {
                    Layout.preferredWidth: 40
                    Layout.preferredHeight: 40
                    color: "transparent"
                    Text {
                        anchors.centerIn: parent
                        text: root.isBrowse ? "▾" : "ⓘ"
                        color: root.muted
                        font.pixelSize: 18
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            if (root.isBrowse)       root.mode = "viz"
                            else                     root.metadataExpanded = !root.metadataExpanded
                        }
                    }
                }
            }

            // Seek row.
            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Text {
                    text: formatDuration(mobileSeek.displayValue)
                    color: root.muted
                    font.pixelSize: 10
                    Layout.preferredWidth: 40
                    horizontalAlignment: Text.AlignRight
                }
                PLYRSeekSlider {
                    id: mobileSeek
                    Layout.fillWidth: true
                    Layout.preferredHeight: 28
                    enabled: playlist.hasCurrent
                    from: 0
                    to:   Math.max(1, audio.duration / 1000.0)
                    value: audio.position / 1000.0
                    onMoved: (v) => audio.seek(v * 1000)
                }
                Text {
                    text: formatDuration(audio.duration / 1000.0)
                    color: root.muted
                    font.pixelSize: 10
                    Layout.preferredWidth: 40
                }
            }

            // Buttons row.
            RowLayout {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignHCenter
                spacing: 36

                Button {
                    text: "⏮"
                    flat: true
                    font.pixelSize: 22
                    enabled: playlist.hasCurrent
                    onClicked: playlist.previous()
                }
                Button {
                    text: audio.playing ? "⏸" : "▶"
                    flat: true
                    font.pixelSize: 30
                    enabled: playlist.hasCurrent
                    onClicked: {
                        if (audio.playing) audio.pause()
                        else               audio.play()
                    }
                }
                Button {
                    text: "⏭"
                    flat: true
                    font.pixelSize: 22
                    enabled: playlist.hasCurrent
                    onClicked: playlist.next()
                }
            }
        }
    }

    // ----- Playlist (browse mode only) -------------------------------
    // Fills the area between the transport (below) and the persistent
    // bottom toggle bar. Hidden while in viz mode so it doesn't steal
    // touches from the viz, and kept above the bottom bar so the bottom
    // chevron is always tappable.
    Rectangle {
        id: browsePane
        color: root.bg
        anchors.left:   parent.left
        anchors.right:  parent.right
        anchors.top:    transport.bottom
        anchors.bottom: bottomBar.top
        visible: root.isBrowse
        opacity: root.isBrowse ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: root.slideMs } }

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // Small header row — folder + open-folder button.
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 44
                color: Qt.rgba(0, 0, 0, 0.92)

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    spacing: 8

                    Text {
                        Layout.fillWidth: true
                        text: playlist.folderPath === "" ? "No folder" : playlist.folderPath
                        color: root.muted
                        font.pixelSize: 11
                        elide: Text.ElideMiddle
                    }
                    // Split button: tap "Open…" for the folder picker,
                    // tap the chevron (or long-press "Open…") for recents.
                    RowLayout {
                        spacing: 0

                        Button {
                            id: openBtn
                            text: "Open…"
                            flat: true
                            onClicked: folderDialog.open()
                            onPressAndHold: recentsMenu.popup(openBtn, 0, openBtn.height)
                        }

                        Button {
                            id: openChevron
                            text: "\u25BE"
                            flat: true
                            implicitWidth: 28
                            padding: 4
                            onClicked: recentsMenu.popup(openChevron, 0, openChevron.height)
                        }
                    }
                }
            }

            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: root.subtleLine }

            ListView {
                id: listView
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: playlist
                spacing: 0

                // Matches the Swift pre-port behaviour: smooth-center on
                // every currentIndex change (startup, user pick, engine
                // advancing). `positionViewAtIndex` is instant, so we
                // probe for the target contentY and animate there.
                SmoothedAnimation {
                    id: mobileCenterAnim
                    target:   listView
                    property: "contentY"
                    duration: 260
                    velocity: -1
                }
                function centerCurrent() {
                    if (playlist.currentIndex < 0 || count === 0) return
                    Qt.callLater(function() {
                        mobileCenterAnim.stop()
                        const oldY = listView.contentY
                        listView.positionViewAtIndex(
                            playlist.currentIndex, ListView.Center)
                        const targetY = listView.contentY
                        listView.contentY = oldY
                        mobileCenterAnim.to = targetY
                        mobileCenterAnim.start()
                    })
                }
                Connections {
                    target: playlist
                    function onCurrentIndexChanged() { listView.centerCurrent() }
                }
                onCountChanged:        if (count > 0) centerCurrent()
                Component.onCompleted: centerCurrent()

                section.property: "discFolder"
                section.criteria: ViewSection.FullString
                section.delegate: Rectangle {
                    width: ListView.view.width
                    height: 32
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
                    height: 60
                    readonly property bool current: (index === playlist.currentIndex)
                    color: current ? Qt.rgba(0.05, 0.10, 0.20, 1.0) : root.bg

                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            playlist.currentIndex = index
                            root.mode = "viz"        // commit → pop back to viz
                        }
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 14
                        anchors.rightMargin: 14
                        anchors.topMargin: 6
                        anchors.bottomMargin: 6
                        spacing: 10

                        Item {
                            Layout.preferredWidth: 26
                            Layout.fillHeight: true
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: row.current
                                      ? "▶"
                                      : String(trackNumber).padStart(2, "0")
                                color: row.current ? root.accent : Qt.rgba(1, 1, 1, 0.30)
                                font.pixelSize: row.current ? 12 : 11
                                font.family: "Menlo"
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 3

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8

                                Text {
                                    Layout.fillWidth: true
                                    text: title
                                    color: row.current ? root.primary : Qt.rgba(1, 1, 1, 0.45)
                                    font.pixelSize: 14
                                    font.bold: row.current
                                    elide: Text.ElideRight
                                }
                                Text {
                                    text: formatDuration(duration)
                                    color: row.current ? Qt.rgba(1, 1, 1, 0.65) : Qt.rgba(1, 1, 1, 0.28)
                                    font.pixelSize: 11
                                    font.family: "Menlo"
                                }
                            }

                            Text {
                                Layout.fillWidth: true
                                text: composer !== "" ? composer : artist
                                color: row.current ? Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.85)
                                                   : Qt.rgba(1, 1, 1, 0.28)
                                font.pixelSize: 11
                                elide: Text.ElideRight
                            }
                        }
                    }
                }

                ScrollBar.vertical: ScrollBar { }
            }
        }
    }

    // ----- Metadata sheet (tap analogue of desktop hover) -------------
    // Slides up over everything when `metadataExpanded` is true. Taps
    // on the backdrop dismiss.
    Rectangle {
        id: metadataSheet
        z: 500
        anchors.fill: parent
        color: Qt.rgba(0, 0, 0, 0.65)
        visible: opacity > 0.01
        opacity: root.metadataExpanded ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 180 } }

        MouseArea {
            anchors.fill: parent
            onClicked: root.metadataExpanded = false
        }

        Rectangle {
            width: parent.width - 32
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 40
            height: metaContent.implicitHeight + 28
            color: Qt.rgba(0.08, 0.08, 0.10, 0.98)
            radius: 14
            border.color: Qt.rgba(1, 1, 1, 0.10)
            border.width: 1

            ColumnLayout {
                id: metaContent
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top:   parent.top
                anchors.margins: 14
                spacing: 6

                Repeater {
                    model: [
                        { k: "TITLE",      v: playlist.currentTitle },
                        { k: "ARTIST",     v: playlist.currentArtist },
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
                        visible: modelData.v !== "" && modelData.v !== "0"
                        spacing: 8

                        Text {
                            text: modelData.k
                            color: Qt.rgba(1, 1, 1, 0.45)
                            font.family: "Menlo"
                            font.pixelSize: 10
                            font.bold: true
                            Layout.preferredWidth: 80
                        }
                        Text {
                            Layout.fillWidth: true
                            text: modelData.v
                            color: "white"
                            font.pixelSize: 12
                            wrapMode: Text.WordWrap
                        }
                    }
                }
            }
        }
    }
}
