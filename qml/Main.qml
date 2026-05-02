import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtQuick.Layouts
import Qt.labs.platform as Platform

ApplicationWindow {
    id: root
    width: 900
    height: 600
    minimumWidth: 800
    minimumHeight: 560
    visible: true
    title: "Concerto"
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

    // Custom in-process folder picker. Replaces Qt's FolderDialog, which
    // was using its QML fallback implementation and was visibly slow to
    // open on macOS (the prewarm of NSOpenPanel didn't help — Qt wasn't
    // routing through it). This picker is FolderListModel-backed, so
    // opening it is essentially instant.
    FolderPicker {
        id: folderDialog
        x: (root.width  - width)  / 2
        y: (root.height - height) / 2
        width:  Math.min(root.width  - 80, 720)
        height: Math.min(root.height - 80, 520)
        recents: playlist.recentFolders
        onAccepted: (url) => playlist.openFolderUrl(url)
    }

    // Aim the picker at the parent of the currently-loaded folder so the
    // user lands on its siblings (the common case: switch to another album
    // in the same artist directory). On first launch, fall back to ~/Music.
    // Computed once per click rather than bound, so the dialog's own
    // navigation while open isn't fought by the binding.
    function _pathToFileUrl(path) {
        // Percent-encode each segment so spaces (and other URL-special
        // chars) don't make the URL invalid — otherwise Qt silently drops
        // it and macOS's NSOpenPanel reuses its last-shown folder.
        return "file://" + path.split("/").map(encodeURIComponent).join("/")
    }
    function openFolderPicker() {
        var url
        var p = playlist.folderPath
        // Strip a trailing slash so lastIndexOf finds the *parent's* slash
        // rather than the path's own trailing one.
        if (p.length > 1 && p.endsWith("/")) p = p.substring(0, p.length - 1)
        if (p !== "") {
            const i = p.lastIndexOf("/")
            if (i > 0) url = Qt.url(_pathToFileUrl(p.substring(0, i)))
        }
        if (!url) {
            url = Platform.StandardPaths.writableLocation(
                Platform.StandardPaths.MusicLocation)
        }
        // Re-query volumes per open so newly mounted/ejected drives are
        // reflected immediately. Recents are already reactive (the
        // playlist signals recentFoldersChanged).
        folderDialog.volumes = systemPaths.mountedVolumes()
        folderDialog.openAt(url)
    }

    // Shared dropdown menu for the split-button's chevron, right-click on
    // the main button, and long-press on mobile. Static "Open Folder…" on
    // top, dynamic recents in the middle (filled via Instantiator from
    // playlist.recentFolders), "Clear Recents" at the bottom.
    Menu {
        id: recentsMenu

        MenuItem {
            text: "Open Folder…"
            onTriggered: root.openFolderPicker()
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
                // Full path in the Tool Tip on hover. Qt Controls Basic
                // doesn't show tooltips inside Menu natively everywhere,
                // but where it does, the full path is there.
                ToolTip.text: modelData
                ToolTip.visible: hovered
                ToolTip.delay: 500
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

    // EQ overlay. Modal popup with backdrop dismiss; the panel itself is
    // EqPanel.qml and gets its data from the `eq` context property.
    Popup {
        id: eqPopup
        x: (root.width  - width)  / 2
        y: (root.height - height) / 2
        width:  Math.min(root.width  - 60, 860)
        height: Math.min(root.height - 60, 540)
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        padding: 0

        background: Rectangle { color: "transparent" }  // EqPanel draws its own

        contentItem: EqPanel {
            onCloseRequested: eqPopup.close()
        }

        Overlay.modal: Rectangle {
            color: Qt.rgba(0, 0, 0, 0.55)
        }
    }

    // Playback: `audio` is a C++ AudioEngine instance (main.cpp ctx-prop).
    // It wraps QMediaPlayer + QAudioBufferOutput so the FFT visualizer
    // gets PCM samples while normal playback runs through QAudioOutput.
    readonly property bool isPlaying: audio.playing

    // Metadata overlay open/close — hover driven. `_hoveringCard` and
    // `_hoveringOverlay` are updated by HoverHandlers below. While either
    // is true the overlay stays open; when both go false, a short grace
    // timer gives the pointer time to travel between them without flicker.
    property bool _hoveringCard:    false
    property bool _hoveringOverlay: false
    readonly property bool _anyHover: _hoveringCard || _hoveringOverlay
    property bool metadataExpanded: false

    on_AnyHoverChanged: {
        if (_anyHover) {
            hideOverlayTimer.stop()
            metadataExpanded = true
        } else if (metadataExpanded) {
            hideOverlayTimer.restart()
        }
    }

    Timer {
        id: hideOverlayTimer
        interval: 160
        onTriggered: root.metadataExpanded = false
    }

    function formatDuration(seconds) {
        if (!isFinite(seconds) || seconds < 0) return "0:00"
        const m = Math.floor(seconds / 60)
        const s = Math.floor(seconds) % 60
        return m + ":" + (s < 10 ? "0" + s : s)
    }

    // ------------------------------------------------------------------
    // Metadata overlay: parented to the now-playing card so its bottom
    // always lines up with the card's bottom — the overlay extends
    // upward from there, revealing metadata rows above a faux-card strip
    // that sits exactly on top of (and visually replaces) the real card.
    // ------------------------------------------------------------------
    Rectangle {
        id: metadataOverlay
        parent: nowCard
        z: 200
        visible: root.metadataExpanded && playlist.hasCurrent
        width: nowCard.width
        // Auto-fit: the inner ColumnLayout's implicitHeight tracks the
        // actual metadata rows + the bottom faux-card strip, so the
        // overlay is exactly as tall as it needs to be.
        height: overlayContent.implicitHeight
        x: 0
        y: nowCard.height - height  // overlay.bottom == card.bottom
        color: Qt.rgba(0.08, 0.08, 0.10, 0.92)
        radius: 10
        border.color: Qt.rgba(1, 1, 1, 0.10)
        border.width: 1

        // Hover on the overlay itself keeps it open. Combined with the
        // card's HoverHandler, the panel stays up while the pointer is
        // over either zone; the hide timer covers the transition gap.
        HoverHandler {
            onHoveredChanged: root._hoveringOverlay = hovered
        }

        ColumnLayout {
            id: overlayContent
            anchors.fill: parent
            spacing: 0

            // Metadata rows
            ColumnLayout {
                Layout.fillWidth: true
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

                        // Mirrors the card's marquee exactly (same text,
                        // same style, same scroll offset). Because the
                        // overlay's bottom strip sits on top of the real
                        // card, this reads as one continuous label.
                        MarqueeText {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 18
                            text:  playlist.currentTitle
                            color: root.primary
                            font.pixelSize: 13
                            font.bold: true
                            followExternal: true
                            externalX: cardTitleMarquee.currentX
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

                // Brand wordmark — replaces the old "CONCERTO" text.
                // Native 6688×1024 (~6.53:1), downscaled to 22 pt tall.
                // `sourceSize.height` at 2× ensures a crisp raster on
                // retina without decoding the full 6688 every frame.
                Image {
                    source: "qrc:/qt/qml/PLYR/resources/images/concerto-logo.png"
                    fillMode: Image.PreserveAspectFit
                    Layout.preferredHeight: 22
                    Layout.preferredWidth: 22 * 6688 / 1024   // ≈ 144
                    sourceSize.height: 44
                    smooth: true
                }

                // Split button: left half opens the folder picker; right
                // half (or right-click anywhere) opens the recents menu.
                RowLayout {
                    spacing: 0

                    Button {
                        id: openFolderBtn
                        text: "📁"
                        font.pixelSize: 16
                        Layout.fillHeight: true
                        Layout.preferredWidth: height
                        padding: 0
                        background: Rectangle { color: "transparent" }
                        onClicked: root.openFolderPicker()
                        ToolTip.text: "Open folder · right-click for recents"
                        ToolTip.visible: hovered
                        ToolTip.delay: 600

                        // Right-click anywhere on the main button also opens
                        // the recents menu. Left-clicks pass through to the
                        // Button because this MouseArea only accepts Right.
                        MouseArea {
                            anchors.fill: parent
                            acceptedButtons: Qt.RightButton
                            onClicked: recentsMenu.popup(openFolderBtn, 0, openFolderBtn.height)
                        }
                    }

                    Button {
                        id: recentsChevron
                        text: "\u25BE"    // ▾
                        implicitWidth: 22
                        padding: 4
                        onClicked: recentsMenu.popup(recentsChevron, 0, recentsChevron.height)
                    }
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

                    // Matches the Swift pre-port behaviour: every time
                    // currentIndex changes — startup, user pick, engine
                    // advancing to the next track — smooth-scroll so the
                    // current row lands in the middle of the viewport.
                    // `positionViewAtIndex` alone is instant, so we use
                    // it as a probe for the target contentY and then
                    // animate from the current contentY to that target.
                    SmoothedAnimation {
                        id: centerAnim
                        target:   listView
                        property: "contentY"
                        duration: 260
                        velocity: -1
                    }
                    function centerCurrent() {
                        if (playlist.currentIndex < 0 || count === 0) return
                        Qt.callLater(function() {
                            centerAnim.stop()
                            const oldY = listView.contentY
                            listView.positionViewAtIndex(
                                playlist.currentIndex, ListView.Center)
                            const targetY = listView.contentY
                            listView.contentY = oldY
                            centerAnim.to = targetY
                            centerAnim.start()
                        })
                    }
                    Connections {
                        target: playlist
                        function onCurrentIndexChanged() { listView.centerCurrent() }
                    }
                    onCountChanged:        if (count > 0) centerCurrent()
                    Component.onCompleted: centerCurrent()

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

            // Right: visualizer — GPU shader (Qt RHI ShaderEffect).
            // All per-pixel band/segment work is done in shaders/viz.frag;
            // this QML just pumps fresh FFT data and binds uniforms.
            Rectangle {
                SplitView.minimumWidth: 280
                color: root.bg

                ShaderEffect {
                    id: viz
                    anchors.fill: parent

                    // FFT uniforms — re-evaluated whenever fft.updated() fires.
                    property vector4d b0: fft.b0
                    property vector4d b1: fft.b1
                    property vector4d b2: fft.b2
                    property vector4d b3: fft.b3
                    property vector4d p0: fft.p0
                    property vector4d p1: fft.p1
                    property vector4d p2: fft.p2
                    property vector4d p3: fft.p3

                    // Geometry + style uniforms.
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

                // Drives the FFT refresh at ~60 Hz. `refresh()` runs the
                // FFT, updates bands/peaks, and emits updated() — which
                // causes the ShaderEffect's property bindings above to
                // re-evaluate and trigger a re-render.
                Timer {
                    interval: 16
                    running:  true
                    repeat:   true
                    onTriggered: fft.refresh()
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
                        text: formatDuration(seekSlider.displayValue)
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
                        from: 0
                        to:   Math.max(1, audio.duration / 1000.0)
                        // `value` stays bound to live playback progress.
                        // Drag state is tracked inside the slider and
                        // exposed via `displayValue` for preview rendering.
                        value: audio.position / 1000.0
                        onMoved: (newValue) => {
                            audio.seek(newValue * 1000)
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

                    // Now-playing card — marquee title + artist + chevron.
                    // Hovering the card (or the overlay it reveals) expands
                    // the metadata panel; moving off collapses it after a
                    // short grace period (see root._anyHover).
                    Rectangle {
                        id: nowCard
                        Layout.preferredWidth: 260
                        Layout.preferredHeight: 48
                        color: Qt.rgba(1, 1, 1, 0.06)
                        radius: 8

                        HoverHandler {
                            onHoveredChanged: root._hoveringCard = hovered
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 6
                            spacing: 6

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2

                                MarqueeText {
                                    id: cardTitleMarquee
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

                            // Chevron — visual indicator that rotates with
                            // the hover-driven overlay state. No MouseArea:
                            // hovering anywhere on the card does the work.
                            Rectangle {
                                Layout.preferredWidth: 20
                                Layout.preferredHeight: 20
                                color: "transparent"
                                radius: 10

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

                    Button {
                        text: "EQ"
                        flat: true
                        highlighted: eqPopup.opened
                        onClicked: eqPopup.opened ? eqPopup.close() : eqPopup.open()
                    }

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
