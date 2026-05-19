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
    // Qt 6.9+ flags. Numeric values from qnamespace.h —
    //   NoTitleBarBackgroundHint = 0x00800000  (titlebarAppearsTransparent;
    //   the title text is hidden separately via macos_titlebar.mm)
    flags: Qt.Window | 0x00800000

    // PLYR palette (mirrors the Swift version).
    readonly property color bg:          "black"
    readonly property color ocean:       Qt.rgba(0.02, 0.18, 0.45, 1.0)
    readonly property color sky:         Qt.rgba(0.45, 0.78, 1.00, 1.0)
    readonly property color accent:      Qt.rgba(0.55, 0.82, 1.00, 1.0)
    readonly property color primary:     "white"
    readonly property color muted:       Qt.rgba(1.0, 1.0, 1.0, 0.40)
    readonly property color veryMuted:   Qt.rgba(1.0, 1.0, 1.0, 0.25)
    readonly property color subtleLine:  Qt.rgba(1.0, 1.0, 1.0, 0.10)

    // Hoist the `fft` context property to a named root reference so the
    // ListView delegate can pass it down to PlayingBars without bare-
    // name lookup (which the delegate's model scope can shadow).
    readonly property var fftRef: fft

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

    // CD-rip view. Non-modal so disc 1 of a batch can play back while
    // disc 2 is reading. `playlist.recentFolders` doubles as a sensible
    // initial location for the "Save to…" picker at the end of a rip.
    RipView {
        id: ripView
        x: (root.width  - width)  / 2
        y: (root.height - height) / 2
        width:  Math.min(root.width  - 60, 880)
        height: Math.min(root.height - 60, 600)
        saveRecents: playlist.recentFolders
    }

    // Open the rip view, mediating through the batch / resume UX:
    //   no resumable batches            → straight to a fresh session
    //   exactly one resumable batch     → "Resume X?" dialog
    //   ≥ 2 resumable batches           → batch picker
    function openRipView(resumeBatchId) {
        ripView.saveVolumes = systemPaths.mountedVolumes()
        ripper.startSession(resumeBatchId || "")
        ripView.open()
    }

    function startRipFlow() {
        ripper.refreshResumableBatches()
        const batches = ripper.resumableBatches
        if (batches.length === 0) {
            root.openRipView()
        } else if (batches.length === 1) {
            resumePrompt.batch = batches[0]
            resumePrompt.open()
        } else {
            batchPicker.open()
        }
    }

    // "Resume <album>?" — single-batch shortcut.
    Dialog {
        id: resumePrompt
        modal: true
        x: (root.width  - width)  / 2
        y: (root.height - height) / 2
        title: "Resume previous rip?"
        standardButtons: Dialog.NoButton

        property var batch: ({})

        background: Rectangle {
            color: Qt.rgba(0.08, 0.08, 0.10, 0.98)
            border.color: root.subtleLine
            border.width: 1
            radius: 10
        }

        contentItem: ColumnLayout {
            spacing: 14
            Text {
                Layout.preferredWidth: 400
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: "white"
                font.pixelSize: 12
                text: {
                    const b = resumePrompt.batch || {}
                    const remaining = (b.totalDiscs || 0) - (b.doneCount || 0)
                    return "You started ripping \"" + (b.albumTitle || "")
                        + "\" earlier — " + remaining + " of " + (b.totalDiscs || 0)
                        + " disc" + ((b.totalDiscs || 0) === 1 ? "" : "s") + " left."
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Button {
                    text: "Delete batch"
                    onClicked: {
                        ripper.deleteResumableBatch(resumePrompt.batch.id)
                        resumePrompt.close()
                    }
                }
                Item { Layout.fillWidth: true }
                Button {
                    text: "Start new"
                    onClicked: {
                        resumePrompt.close()
                        root.openRipView()
                    }
                }
                Button {
                    text: "Resume"
                    highlighted: true
                    onClicked: {
                        const id = resumePrompt.batch.id
                        resumePrompt.close()
                        root.openRipView(id)
                    }
                }
            }
        }
    }

    // Multi-batch resume picker.
    Dialog {
        id: batchPicker
        modal: true
        x: (root.width  - width)  / 2
        y: (root.height - height) / 2
        width: 480
        title: "Resume which rip?"
        standardButtons: Dialog.NoButton

        background: Rectangle {
            color: Qt.rgba(0.08, 0.08, 0.10, 0.98)
            border.color: root.subtleLine
            border.width: 1
            radius: 10
        }

        contentItem: ColumnLayout {
            spacing: 8
            ListView {
                Layout.fillWidth: true
                Layout.preferredHeight: 240
                clip: true
                model: ripper.resumableBatches
                spacing: 0

                delegate: Rectangle {
                    width: ListView.view.width
                    height: 48
                    color: rowMouse.containsMouse
                           ? Qt.rgba(1, 1, 1, 0.06)
                           : "transparent"
                    radius: 6

                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            batchPicker.close()
                            root.openRipView(modelData.id)
                        }
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        spacing: 8

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Text {
                                Layout.fillWidth: true
                                text: modelData.albumTitle
                                color: "white"
                                font.pixelSize: 12
                                font.bold: true
                                elide: Text.ElideRight
                            }
                            Text {
                                Layout.fillWidth: true
                                text: modelData.artist + "  ·  "
                                    + modelData.doneCount + "/"
                                    + modelData.totalDiscs + " discs done"
                                color: root.muted
                                font.pixelSize: 10
                                elide: Text.ElideRight
                            }
                        }
                        Button {
                            text: "Delete"
                            flat: true
                            onClicked: {
                                ripper.deleteResumableBatch(modelData.id)
                                // List is bound to ripper.resumableBatches and
                                // refreshes automatically.
                            }
                        }
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Item { Layout.fillWidth: true }
                Button {
                    text: "Start a new rip"
                    onClicked: {
                        batchPicker.close()
                        root.openRipView()
                    }
                }
                Button {
                    text: "Cancel"
                    onClicked: batchPicker.close()
                }
            }
        }
    }

    function togglePlayPause() {
        if (!playlist.hasCurrent) return
        if (root.isPlaying) audio.pause()
        else audio.play()
    }

    // Transport key shortcuts. ApplicationShortcut context so they fire
    // regardless of which item currently holds keyboard focus (the
    // default WindowShortcut needs an item in the window to have focus,
    // which isn't guaranteed at startup). All gated while any popup is
    // open so the keys remain free for the popup itself (Enter to
    // confirm a folder pick, arrow keys to navigate its list, etc.).
    readonly property bool _transportKeysActive:
        !folderDialog.opened && !ripView.opened && !eqPopup.opened

    // Space / Return / Enter = play-pause toggle.
    Shortcut {
        sequences: ["Space", "Return", "Enter"]
        context: Qt.ApplicationShortcut
        enabled: root._transportKeysActive
        onActivated: root.togglePlayPause()
    }

    // ← / → = previous / next track.
    Shortcut {
        sequence: "Left"
        context: Qt.ApplicationShortcut
        enabled: root._transportKeysActive && playlist.hasCurrent
        onActivated: playlist.previous()
    }
    Shortcut {
        sequence: "Right"
        context: Qt.ApplicationShortcut
        enabled: root._transportKeysActive && playlist.hasCurrent
        onActivated: playlist.next()
    }

    // Shift+← / Shift+→ = seek 5 s backward / forward, clamped.
    Shortcut {
        sequence: "Shift+Left"
        context: Qt.ApplicationShortcut
        enabled: root._transportKeysActive && playlist.hasCurrent
        onActivated: audio.seek(Math.max(0, audio.position - 5000))
    }
    Shortcut {
        sequence: "Shift+Right"
        context: Qt.ApplicationShortcut
        enabled: root._transportKeysActive && playlist.hasCurrent
        onActivated: audio.seek(Math.min(audio.duration, audio.position + 5000))
    }

    // Visualizer hotkeys. Cmd+0..9 selects entries 0..9 of
    // `visualizers.available`; Cmd+Shift+0..3 selects entries 10..13. Qt's
    // shortcut grammar uses "Ctrl+" for the platform's primary modifier,
    // which on macOS is the Cmd key — see QKeySequence::PortableText.
    // Bounds-guarded so unconfigured slots are silent no-ops. Written
    // long-hand rather than via Repeater because Shortcut is a
    // QQuickAttachedObject, not an Item — Repeater only instantiates Items.
    function _selectVisualizer(idx) {
        const list = visualizers.available
        if (idx < 0 || idx >= list.length) return
        visualizers.currentVisualizerId = list[idx].id
    }
    Shortcut { sequence: "Ctrl+0"; context: Qt.ApplicationShortcut; onActivated: root._selectVisualizer(0) }
    Shortcut { sequence: "Ctrl+1"; context: Qt.ApplicationShortcut; onActivated: root._selectVisualizer(1) }
    Shortcut { sequence: "Ctrl+2"; context: Qt.ApplicationShortcut; onActivated: root._selectVisualizer(2) }
    Shortcut { sequence: "Ctrl+3"; context: Qt.ApplicationShortcut; onActivated: root._selectVisualizer(3) }
    Shortcut { sequence: "Ctrl+4"; context: Qt.ApplicationShortcut; onActivated: root._selectVisualizer(4) }
    Shortcut { sequence: "Ctrl+5"; context: Qt.ApplicationShortcut; onActivated: root._selectVisualizer(5) }
    Shortcut { sequence: "Ctrl+6"; context: Qt.ApplicationShortcut; onActivated: root._selectVisualizer(6) }
    Shortcut { sequence: "Ctrl+7"; context: Qt.ApplicationShortcut; onActivated: root._selectVisualizer(7) }
    Shortcut { sequence: "Ctrl+8"; context: Qt.ApplicationShortcut; onActivated: root._selectVisualizer(8) }
    Shortcut { sequence: "Ctrl+9"; context: Qt.ApplicationShortcut; onActivated: root._selectVisualizer(9) }
    Shortcut { sequence: "Ctrl+Shift+0"; context: Qt.ApplicationShortcut; onActivated: root._selectVisualizer(10) }
    Shortcut { sequence: "Ctrl+Shift+1"; context: Qt.ApplicationShortcut; onActivated: root._selectVisualizer(11) }
    Shortcut { sequence: "Ctrl+Shift+2"; context: Qt.ApplicationShortcut; onActivated: root._selectVisualizer(12) }
    Shortcut { sequence: "Ctrl+Shift+3"; context: Qt.ApplicationShortcut; onActivated: root._selectVisualizer(13) }

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
                            font.family: "Iosevka"
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

                // Rip CD button. Opens the rip view; the view itself
                // handles disc detection, identification, and the rest of
                // the pipeline. While a rip is active and the view is
                // dismissed, this button is replaced by a progress pill
                // showing the live state.
                Button {
                    id: ripCdBtn
                    visible: ripper.state === 0 /*Idle*/
                    text: "💿"
                    font.pixelSize: 14
                    Layout.fillHeight: true
                    Layout.preferredWidth: height
                    padding: 0
                    background: Rectangle { color: "transparent" }
                    onClicked: root.startRipFlow()
                    ToolTip.text: "Rip a CD"
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                }

                // Header pill — visible only when a rip is in progress
                // AND the rip view is dismissed. Click reopens the view.
                Rectangle {
                    visible: ripper.state !== 0 /*Idle*/
                             && !ripView.opened
                    Layout.preferredHeight: 26
                    Layout.preferredWidth: pillRow.implicitWidth + 22
                    radius: 13
                    color: Qt.rgba(0.05, 0.10, 0.20, 1.0)
                    border.color: Qt.rgba(0.55, 0.82, 1.00, 0.30)
                    border.width: 1

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: ripView.open()
                    }

                    RowLayout {
                        id: pillRow
                        anchors.centerIn: parent
                        spacing: 8
                        Text {
                            text: "💿"
                            font.pixelSize: 12
                        }
                        Text {
                            text: {
                                switch (ripper.state) {
                                case 1: return ripper.inBatch
                                    ? "Insert disc " + ripper.batchExpectedDisc
                                      + "/" + ripper.batchTotalCount
                                    : "Waiting for disc"
                                case 2: return "Identifying…"
                                case 3: return "Ripping · "
                                    + Math.round(ripper.readProgress * 100) + "%"
                                case 4: return "Encoding · "
                                    + Math.round(ripper.encodeProgress * 100) + "%"
                                case 5: return "Verifying · "
                                    + Math.round(ripper.verifyProgress * 100) + "%"
                                case 6: return "Ready to save"
                                case 7: return "Saving…"
                                case 8: return "Done"
                                case 9: return "Stopping…"
                                case 10: return "Failed"
                                }
                                return ""
                            }
                            color: root.primary
                            font.pixelSize: 11
                            font.family: "Iosevka"
                        }
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

                    // Safari-feel scroll tuning (mirrors PureBibleQt). Native
                    // Qt Flickable wheel handling is step-based and coarse;
                    // we replace it with a WheelHandler that applies pixel-
                    // precise trackpad deltas directly and animates mouse-
                    // wheel ticks with an OutQuint ease so rapid clicks
                    // compound instead of cancelling.
                    flickDeceleration: 350
                    maximumFlickVelocity: 15000
                    pixelAligned: true

                    property real wheelTargetY: 0

                    function clampY(y) {
                        return Math.max(0, Math.min(listView.contentHeight - listView.height, y))
                    }

                    onMovingChanged:   if (moving)   wheelAnim.stop()
                    onFlickingChanged: if (flicking) wheelAnim.stop()

                    NumberAnimation {
                        id: wheelAnim
                        target: listView
                        property: "contentY"
                        duration: 220
                        easing.type: Easing.OutQuint
                    }

                    WheelHandler {
                        target: null
                        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                        onWheel: (event) => {
                            // Trackpads / "smooth scrolling" mice deliver
                            // pixelDelta — already OS-smoothed, apply directly.
                            if (event.pixelDelta.y !== 0) {
                                wheelAnim.stop()
                                centerAnim.stop()
                                listView.contentY = listView.clampY(
                                    listView.contentY - event.pixelDelta.y)
                                listView.wheelTargetY = listView.contentY
                                event.accepted = true
                                return
                            }
                            // Traditional wheel ticks — accumulate into a
                            // target so successive clicks compound.
                            const step = event.angleDelta.y
                            const base = wheelAnim.running
                                ? listView.wheelTargetY
                                : listView.contentY
                            listView.wheelTargetY = listView.clampY(base - step)
                            centerAnim.stop()
                            wheelAnim.stop()
                            wheelAnim.from = listView.contentY
                            wheelAnim.to   = listView.wheelTargetY
                            wheelAnim.start()
                            event.accepted = true
                        }
                    }

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

                                // Active + playing → live FFT bars
                                // (concerto's ocean→sky palette). Active +
                                // paused → plain ▶. Inactive → 2-digit
                                // track number.
                                PlayingBars {
                                    anchors.centerIn: parent
                                    visible: row.current && root.isPlaying
                                    fft: root.fftRef
                                    height: 14
                                    oceanColor: root.ocean
                                    skyColor:   root.sky
                                }
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    visible: !(row.current && root.isPlaying)
                                    text: row.current
                                          ? "▶"
                                          : String(trackNumber).padStart(2, "0")
                                    color: row.current ? root.accent
                                                       : Qt.rgba(1, 1, 1, 0.28)
                                    font.pixelSize: row.current ? 11 : 10
                                    font.family: "Iosevka"
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
                                        font.family: "Iosevka"
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

            // Right: visualizer pane. Hosts either the built-in 16-band
            // ShaderEffect (id "viz16band") or an "advanced" visualizer
            // QML loaded by URL from the VisualizerRegistry. The active
            // viz is `visualizers.currentVisualizerId`; Cmd+0..9 / Cmd+
            // Shift+0..3 cycle through `visualizers.available`.
            Rectangle {
                SplitView.minimumWidth: 280
                color: root.bg

                readonly property bool _is16Band:
                    visualizers.currentVisualizerId === "viz16band"

                // ---- 16-band default path ---------------------------
                // Preserved exactly as-is; only `visible` is added so it
                // hides when an advanced viz takes over. All per-pixel
                // band/segment work is done in shaders/viz.frag; this
                // QML just pumps fresh FFT data and binds uniforms.
                ShaderEffect {
                    id: viz
                    anchors.fill: parent
                    visible: parent._is16Band

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
                // re-evaluate and trigger a re-render. Only ticks while
                // the 16-band shader is the active viz; other visualizers
                // don't read fft.bX/pX and would just burn CPU.
                Timer {
                    interval: 16
                    running:  parent._is16Band
                    repeat:   true
                    onTriggered: fft.refresh()
                }

                // ---- Advanced visualizer path -----------------------
                // For every registry entry whose id != "viz16band" we
                // load the corresponding QML by URL. The loaded item
                // inherits the `audioFeatures`, `fft`, `audio`, `visualizers`
                // context properties from the root context — see
                // ScopeView.qml / SpectrumView.qml for the bare-name lookup
                // pattern. Synchronous load so Cmd+N swaps feel instant.
                Loader {
                    id: advancedViz
                    anchors.fill: parent
                    visible: !parent._is16Band
                    active:  !parent._is16Band
                    asynchronous: false
                    source: {
                        if (parent._is16Band) return ""
                        const info = visualizers.visualizerInfo(
                            visualizers.currentVisualizerId)
                        return (info && info.qmlSource) ? info.qmlSource : ""
                    }
                    onStatusChanged: {
                        if (status === Loader.Error) {
                            console.warn("Visualizer Loader error for",
                                         visualizers.currentVisualizerId,
                                         "source:", source)
                        }
                    }
                }

                // Top-right toast confirming a viz swap. Triggered on
                // every `currentVisualizerIdChanged` (including the one
                // that fires at app startup from QSettings restore — fine,
                // it just labels the current viz briefly).
                Rectangle {
                    id: vizToast
                    anchors.top: parent.top
                    anchors.right: parent.right
                    anchors.margins: 12
                    color: Qt.rgba(0, 0, 0, 0.55)
                    radius: height / 2
                    height: vizToastLabel.implicitHeight + 10
                    width:  vizToastLabel.implicitWidth  + 22
                    opacity: 0
                    visible: opacity > 0.001
                    Behavior on opacity { NumberAnimation { duration: 250 } }

                    Text {
                        id: vizToastLabel
                        anchors.centerIn: parent
                        color: "white"
                        font.pixelSize: 14
                        text: {
                            const info = visualizers.visualizerInfo(
                                visualizers.currentVisualizerId)
                            const name = (info && info.displayName)
                                ? info.displayName
                                : visualizers.currentVisualizerId
                            return "Visualizer: " + name
                        }
                    }

                    Timer {
                        id: vizToastHide
                        interval: 1500
                        repeat: false
                        onTriggered: vizToast.opacity = 0
                    }

                    Connections {
                        target: visualizers
                        function onCurrentVisualizerIdChanged() {
                            vizToast.opacity = 1
                            vizToastHide.restart()
                        }
                    }
                }

                // A/V sync calibration overlay — dot flashes on each
                // detected onset; slider biases the analytic lookahead.
                // Uncomment to bring back the tuner panel.
                // SyncTuner {
                //     anchors.bottom: parent.bottom
                //     anchors.right:  parent.right
                //     anchors.margins: 12
                // }

                // Drives the higher-level AudioFeatures pipeline at the
                // same 60 Hz cadence: per-band envelope followers,
                // centroid, flux, onset detection, phase correlation, and
                // the 512×2 spectrum/waveform texture rows. Independent
                // from fft.refresh() — both consume the same PCM stream
                // but maintain their own ring + smoothing state.
                Timer {
                    interval: 16
                    running:  true
                    repeat:   true
                    onTriggered: audioFeatures.refresh()
                }

                // Layer-0 debug HUD is at qml/AudioFeaturesHud.qml; drop
                // an `AudioFeaturesHud { ... }` here during dev to verify
                // the DSP. See that file's header for the invocation.
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
                        font.family: "Iosevka"
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
                        font.family: "Iosevka"
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
                        onClicked: root.togglePlayPause()
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
