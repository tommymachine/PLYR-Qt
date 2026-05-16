import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qt.labs.platform as Platform

// The CD rip view. Non-modal Popup so audio (disc 1 already saved + playing)
// continues to be interactive underneath; the rip continues even when the
// popup is closed, replaced by a header pill in Main.qml.
//
// State-driven layout — the central message, sidebar contents, and bottom
// bar buttons all switch on ripper.state. The CD canvas is the constant
// visual anchor across every phase.
Popup {
    id: ripView

    // Modal would block audio playback of the already-saved disc 1 in a
    // multi-disc set. We use a backdrop dim instead, but events fall
    // through so the user can interact with the player.
    modal: false
    focus: true
    closePolicy: Popup.NoAutoClose   // closing is explicit (X / done)
    padding: 0

    // Palette pulled in from the parent (Main.qml's root.* properties).
    property color bg:          "black"
    property color ocean:       Qt.rgba(0.02, 0.18, 0.45, 1.0)
    property color sky:         Qt.rgba(0.45, 0.78, 1.00, 1.0)
    property color accent:      Qt.rgba(0.55, 0.82, 1.00, 1.0)
    property color primary:     "white"
    property color muted:       Qt.rgba(1.0, 1.0, 1.0, 0.40)
    property color veryMuted:   Qt.rgba(1.0, 1.0, 1.0, 0.25)
    property color subtleLine:  Qt.rgba(1.0, 1.0, 1.0, 0.10)

    // Hoist the `fft` context property so the track-list delegate can
    // pass it down to PlayingBars unambiguously (bare-name `fft`
    // lookup in a delegate scope is unreliable).
    readonly property var fftRef: fft

    // Recents + volumes list passed through to the save picker. Host
    // (Main.qml) supplies these so they stay reactive.
    property var saveRecents: []
    property var saveVolumes: []

    // "Preview is the active audio source" — true while the streaming
    // preview (raw CDDA → pipe → sink) is feeding the engine. The
    // streaming source URL is the synthetic `preview://cd-rip` rather
    // than a file://, so the predicate is just a URL-scheme check.
    // Lifted to the Popup so the track-list delegate can read it
    // without crossing scopes.
    readonly property bool _previewActive:
        audio.source !== undefined
        && audio.source.toString().startsWith("preview://")

    // "Engine has something to play / pause / seek through". True for
    // either FLAC playback (playlist has a current row) or the
    // streaming preview. Also lifted to the Popup so the transport
    // bindings further down (which reference `ripView._audioReady`)
    // resolve in scope.
    readonly property bool _audioReady:
        playlist.hasCurrent || _previewActive

    // During streaming preview the AudioEngine treats the whole disc as
    // one synthetic segment: `audio.duration` is the disc total and
    // `audio.position` is time-since-stream-start. The mini-transport
    // below would otherwise show "0:23 / 73:18" of the disc; we want
    // "0:23 / 4:32" of the current track. We already know per-track
    // durations + startFraction/endFraction from the TOC the moment
    // streaming kicks off, so we can derive a per-track view here.
    //
    // When streaming is NOT active (post-save FLAC playback), the
    // engine itself exposes per-track values and these fall through
    // to the raw audio.* numbers via the -1 / fallback branches.
    readonly property int _streamTrackIndex: {
        if (!_previewActive || audio.duration <= 0) return -1
        const rows = ripper.tracks
        if (!rows || rows.length === 0) return -1
        const frac = audio.position / audio.duration
        for (let i = 0; i < rows.length; ++i) {
            const sf = rows[i].startFraction
            const ef = rows[i].endFraction
            if (sf === undefined || ef === undefined || ef <= sf) continue
            if (frac >= sf && frac < ef) return i
        }
        // Past the last endFraction (race at disc end) — clamp.
        return rows.length - 1
    }
    readonly property real _streamTrackStartMs:
        _streamTrackIndex >= 0 && audio.duration > 0
            ? ripper.tracks[_streamTrackIndex].startFraction * audio.duration
            : 0
    readonly property real _trackDurationMs:
        _streamTrackIndex >= 0
            ? (ripper.tracks[_streamTrackIndex].durationSec || 0) * 1000
            : audio.duration
    readonly property real _trackPositionMs:
        _streamTrackIndex >= 0
            ? Math.max(0, audio.position - _streamTrackStartMs)
            : audio.position

    background: Rectangle {
        color: "black"
        border.color: ripView.subtleLine
        border.width: 1
        radius: 12
    }

    // Translate the Ripper.State enum into a string the canvas reads.
    function modeString(s) {
        switch (s) {
        case 0: return "idle"
        case 1: return "waiting"
        case 2: return "identifying"
        case 3: return "reading"
        case 4: return "encoding"
        case 5: return "verifying"
        case 6: return "savePending"
        case 7: return "saving"
        case 8: return "done"
        case 9: return "cancelling"
        case 10: return "failed"
        }
        return "idle"
    }

    // Top-bar title — primary text only (album / state-noun). The
    // disc-of-disc position is shown separately in muted gray to its
    // right. Always shows position info (including "1 of 1" for
    // singles) when a disc is identified.
    function titleText() {
        switch (ripper.state) {
        case 1: return ripper.inBatch
            ? "Insert next disc"
            : (ripper.discPresent ? "Audio CD" : "Insert a CD")
        case 2: return "Identifying disc"
        case 3:
        case 4:
        case 5:
        case 6:
        case 7:
        case 8:
            return ripper.hasMatch ? ripper.albumTitle
                 : (ripper.inBatch ? ripper.batchAlbumTitle : "Audio CD")
        case 9: return "Stopping"
        case 10: return "Rip failed"
        }
        return ""
    }

    // Position string shown in gray next to the title. Always present
    // when we know the disc identity — "Disc 2 of 14" for sets, "Disc 1
    // of 1" for singles.
    function positionText() {
        if (ripper.inBatch && ripper.state === 1 /*WaitingForDisc*/) {
            return "Disc " + ripper.batchExpectedDisc
                   + " of " + ripper.batchTotalCount
                   + "  ·  " + ripper.batchAlbumTitle
        }
        if (!ripper.discPresent && !ripper.hasMatch) return ""
        const pos   = ripper.discPosition
        const total = Math.max(ripper.discTotalCount, 1)
        return "Disc " + pos + " of " + total
    }

    function statusLine() {
        switch (ripper.state) {
        case 1:  // WaitingForDisc — the big "Insert CD to rip" panel
            return ""        // on the right is the only prompt now
        case 2:  // Identifying
            return "Reading TOC · looking up MusicBrainz · checking drive offset"
        case 3: {  // Reading
            const pct = Math.round(ripper.readProgress * 100)
            const mins = Math.floor(ripper.etaSec / 60)
            const secs = ripper.etaSec % 60
            const eta = mins + "m " + (secs < 10 ? "0" : "") + secs + "s remaining"
            return pct + "%  ·  "
                 + Math.round(ripper.currentSpeedSecPerSec) + " sec/s · "
                 + ripper.currentMultiplier.toFixed(1) + "×  ·  "
                 + eta
        }
        case 4:
            return "Encoding " + Math.round(ripper.encodeProgress * 100) + "%"
        case 5:
            return "Verifying " + Math.round(ripper.verifyProgress * 100) + "%"
        case 6:
            return ripper.verifySummary
        case 7:
            return "Moving tracks into place…"
        case 8:
            return "Tracks saved and playing now."
        case 9:
            return "Cancelling — discarding the in-progress disc."
        case 10:
            return ripper.errorMessage
        }
        return ""
    }

    function fmtTime(s) {
        if (!isFinite(s) || s < 0) return "0:00"
        const m = Math.floor(s / 60)
        const r = Math.floor(s) % 60
        return m + ":" + (r < 10 ? "0" : "") + r
    }

    // When the rip view opens, swing the disc's light in — the
    // CdDiscCanvas runs its `_swingOffset` from −75° → 0 with ease-out
    // so the iridescent shimmer rolls in across the disc.
    onOpened: disc.swingIn()

    // Auto-open the save picker once the rip enters SavePending. Guarded
    // by _savePickerArmed so re-entering the state (e.g. user cancelled
    // the picker and stepped back-then-forward) doesn't fire it twice on
    // the same entry — the Save button stays visible for that re-trigger
    // path.
    property bool _savePickerArmed: false
    Connections {
        target: ripper
        function onStateChanged() {
            if (ripper.state === 6 /*SavePending*/ && !ripView._savePickerArmed) {
                ripView._savePickerArmed = true
                savePicker.volumes = ripView.saveVolumes
                const initial = Platform.StandardPaths.writableLocation(
                    Platform.StandardPaths.MusicLocation)
                savePicker.openAt(initial)
            } else if (ripper.state !== 6) {
                ripView._savePickerArmed = false
            }
        }
    }

    contentItem: Item {

        // ---- Top bar: close + title ----
        Rectangle {
            id: topBar
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: 44
            color: "transparent"

            // macOS-style close stoplight, top-left. Behavior:
            //   * during a read/encode/verify -> confirm stop (Resume later
            //     vs Delete batch) via the confirmDialog.
            //   * in WaitingForDisc / Done -> just close the popup.
            Rectangle {
                id: closeBtn
                width: 12; height: 12; radius: 6
                anchors.left: parent.left
                anchors.leftMargin: 14
                anchors.verticalCenter: parent.verticalCenter
                color: closeMouse.pressed ? "#BF4942" : "#ED6A5F"
                border.color: "#E24B41"
                border.width: 1
                antialiasing: true

                MouseArea {
                    id: closeMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: ripView.requestClose()
                }

                Item {
                    anchors.centerIn: parent
                    width: 8; height: 8
                    visible: closeMouse.containsMouse
                    Rectangle {
                        anchors.centerIn: parent
                        width: parent.width; height: 1.5
                        color: "#B30000"; rotation: 45
                        antialiasing: true
                    }
                    Rectangle {
                        anchors.centerIn: parent
                        width: parent.width; height: 1.5
                        color: "#B30000"; rotation: -45
                        antialiasing: true
                    }
                }
            }

            // SCANCERTO wordmark — the rip flow's sub-brand, sits to
            // the left of the disc title in muted gray so it reads as a
            // section header rather than competing with the album name.
            Text {
                id: scancertoMark
                anchors.left: closeBtn.right
                anchors.leftMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                text: "SCANCERTO"
                color: ripView.muted
                font.pixelSize: 12
                font.bold: true
                font.letterSpacing: 1.2
            }

            Row {
                anchors.left: scancertoMark.right
                anchors.leftMargin: 14
                anchors.right: stepIndicator.left
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                spacing: 12
                clip: true

                Text {
                    text: ripView.titleText()
                    color: ripView.primary
                    font.pixelSize: 13
                    font.bold: true
                    anchors.verticalCenter: parent.verticalCenter
                    elide: Text.ElideRight
                }
                Text {
                    text: ripView.positionText()
                    visible: text !== ""
                    color: ripView.muted
                    font.pixelSize: 12
                    anchors.verticalCenter: parent.verticalCenter
                    elide: Text.ElideRight
                }
            }

            // Top-right indicator. Estimated time remaining during the
            // read phase (the long phase that benefits from one); empty
            // otherwise.
            Text {
                id: stepIndicator
                anchors.right: parent.right
                anchors.rightMargin: 14
                anchors.verticalCenter: parent.verticalCenter
                text: {
                    if (ripper.state === 3 /*Reading*/ && ripper.etaSec > 0) {
                        const m = Math.floor(ripper.etaSec / 60)
                        const s = ripper.etaSec % 60
                        return m + ":" + (s < 10 ? "0" : "") + s + " remaining"
                    }
                    return ""
                }
                color: ripView.muted
                font.family: "Menlo"
                font.pixelSize: 10
            }
        }

        Rectangle {
            id: topDivider
            anchors.top: topBar.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            height: 1
            color: ripView.subtleLine
        }

        // ---- Body: CD canvas on the left, info panel on the right ----
        RowLayout {
            anchors.top: topDivider.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 18
            spacing: 18

            // Left column — CD canvas + status caption. Fixed width so
            // the right column's geometry stays constant across stages.
            ColumnLayout {
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredWidth: 380
                Layout.minimumWidth: 380
                Layout.maximumWidth: 380
                spacing: 12

                CdDiscCanvas {
                    id: disc
                    // Fixed size so the left column doesn't reflow between
                    // stages — keeps the disc anchored at the same place
                    // and the right column at the same width.
                    Layout.alignment: Qt.AlignHCenter
                    Layout.preferredWidth:  360
                    Layout.preferredHeight: 360
                    Layout.minimumWidth:    360
                    Layout.minimumHeight:   360

                    mode: ripView.modeString(ripper.state)
                    readProgress: ripper.readProgress
                    encodeProgress: ripper.encodeProgress
                    verifyProgress: ripper.verifyProgress
                    tracks: ripper.tracks
                    zeroFilledRanges: ripper.zeroFilledRanges
                }

                // Fixed-height caption so the column's total height is
                // constant across states — the disc above stays pegged
                // at the same vertical position regardless of whether
                // statusLine() wraps to 1, 2, or 3 lines.
                Text {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.fillWidth: true
                    Layout.preferredHeight: 36
                    Layout.minimumHeight: 36
                    Layout.maximumHeight: 36
                    text: ripView.statusLine()
                    color: ripView.muted
                    font.pixelSize: 11
                    font.family: "Menlo"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignTop
                    wrapMode: Text.WordWrap
                    clip: true
                }
            }

            // Right column "insert" placeholder — replaces the data
            // panel when we're idling at WaitingForDisc with nothing in
            // the drive. A single big prompt on the right is much more
            // useful than empty DRIVE / MATCH / OFFSET rows.
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumWidth: 280
                visible: ripper.state === 1 /*WaitingForDisc*/
                         && !ripper.discPresent

                ColumnLayout {
                    anchors.centerIn: parent
                    width: parent.width - 32
                    spacing: 6

                    Text {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        text: ripper.inBatch
                              ? "Insert disc " + ripper.batchExpectedDisc
                                + " to continue"
                              : "Insert a CD to rip"
                        color: ripView.primary
                        font.pixelSize: 26
                        font.bold: true
                        wrapMode: Text.WordWrap
                    }
                    Text {
                        visible: ripper.inBatch
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        text: "(or any other disc from this set)"
                        color: ripView.muted
                        font.pixelSize: 11
                        wrapMode: Text.WordWrap
                    }
                    // Skip this disc: mark the current expected disc
                    // as `skipped` in the batch JSON and advance to
                    // the next pending one. parent_folder memory
                    // persists, so subsequent discs auto-save into
                    // the same place.
                    Text {
                        visible: ripper.inBatch
                        Layout.alignment: Qt.AlignHCenter
                        text: "Skip this one"
                        color: skipMouse.containsMouse
                               ? ripView.accent
                               : ripView.veryMuted
                        font.pixelSize: 11
                        font.underline: skipMouse.containsMouse

                        MouseArea {
                            id: skipMouse
                            anchors.fill: parent
                            anchors.margins: -6
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: ripper.skipCurrentDisc()
                        }
                    }

                    // Breathing room between the call-to-action and the
                    // set-context recap.
                    Item {
                        visible: ripper.inBatch
                        Layout.preferredHeight: 28
                    }

                    // Set context: album / artist / disc-N-of-M /
                    // save folder. Lets the user confirm which batch
                    // they're being asked to feed before swapping
                    // discs. Visible only inside a batch — out-of-set
                    // discs go through the full disc-identify flow
                    // and don't need the recap.
                    ColumnLayout {
                        visible: ripper.inBatch
                        Layout.alignment: Qt.AlignHCenter
                        Layout.preferredWidth: Math.min(parent.width - 64, 520)
                        spacing: 2

                        Text {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                            visible: ripper.batchAlbumTitle !== ""
                            text: ripper.batchAlbumTitle
                            color: ripView.primary
                            font.pixelSize: 14
                            font.bold: true
                            wrapMode: Text.WordWrap
                            maximumLineCount: 2
                            elide: Text.ElideRight
                        }
                        Text {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                            visible: ripper.batchArtist !== ""
                            text: ripper.batchArtist
                            color: ripView.accent
                            font.pixelSize: 12
                            wrapMode: Text.WordWrap
                            elide: Text.ElideRight
                        }
                        Text {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                            text: "Disc " + ripper.batchExpectedDisc
                                + " of " + ripper.batchTotalCount
                                + "  ·  " + ripper.batchDoneCount
                                + " saved"
                            color: ripView.muted
                            font.pixelSize: 10
                            font.family: "Menlo"
                        }
                        Text {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                            visible: ripper.batchParentFolder !== ""
                            text: "→ " + ripper.batchParentFolder
                            color: ripView.veryMuted
                            font.pixelSize: 9
                            font.family: "Menlo"
                            elide: Text.ElideMiddle
                        }
                    }
                }
            }

            // Right column — info panel (drive, match, offset) above the
            // track list. Visible from Identifying onward; collapses when
            // we're at WaitingForDisc with no disc (the placeholder above
            // takes over).
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumWidth: 280
                spacing: 14
                visible: !(ripper.state === 1 && !ripper.discPresent)

                // ---- DRIVE / MATCH / OFFSET grid ----
                GridLayout {
                    Layout.fillWidth: true
                    columns: 2
                    columnSpacing: 16
                    rowSpacing: 10

                    // DRIVE
                    Text {
                        text: "DRIVE"
                        color: ripView.muted
                        font.family: "Menlo"; font.pixelSize: 9; font.bold: true
                        Layout.preferredWidth: 64
                    }
                    Text {
                        Layout.fillWidth: true
                        text: ripper.driveName === ""
                              ? "— waiting on disc"
                              : ripper.driveName
                        color: ripper.driveName === ""
                               ? ripView.muted : ripView.primary
                        font.pixelSize: 11
                        elide: Text.ElideRight
                    }

                    // MATCH
                    Text {
                        text: "MATCH"
                        color: ripView.muted
                        font.family: "Menlo"; font.pixelSize: 9; font.bold: true
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Text {
                            Layout.fillWidth: true
                            text: ripper.hasMatch
                                  ? ripper.artist + " — " + ripper.albumTitle
                                  : (ripper.discPresent
                                     ? "Looking up…"
                                     : "— no disc")
                            color: ripper.hasMatch
                                   ? ripView.primary : ripView.muted
                            font.pixelSize: 11
                            elide: Text.ElideRight
                        }
                        Text {
                            Layout.fillWidth: true
                            visible: ripper.hasMatch
                            text: {
                                let s = ripper.date
                                if (ripper.discTotalCount > 1) {
                                    if (s.length > 0) s += "  ·  "
                                    s += "Disc " + ripper.discPosition
                                       + " of " + ripper.discTotalCount
                                }
                                return s
                            }
                            color: ripView.muted
                            font.pixelSize: 10
                            elide: Text.ElideRight
                        }
                    }

                    // OFFSET
                    Text {
                        text: "OFFSET"
                        color: ripView.muted
                        font.family: "Menlo"; font.pixelSize: 9; font.bold: true
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Text {
                            Layout.fillWidth: true
                            text: ripper.discPresent
                                ? (ripper.driveOffsetSamples >= 0 ? "+" : "")
                                  + ripper.driveOffsetSamples + " samples"
                                : "—"
                            color: ripper.discPresent
                                   ? ripView.primary : ripView.muted
                            font.pixelSize: 11
                            font.family: "Menlo"
                        }
                        Text {
                            Layout.fillWidth: true
                            visible: ripper.discPresent
                            text: ripper.driveOffsetFromDb
                                ? "from AccurateRip drive DB"
                                : "drive not in DB · ripping at 0, verifier scans"
                            color: ripView.muted
                            font.pixelSize: 10
                            elide: Text.ElideRight
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: ripView.subtleLine
                    visible: ripper.discPresent
                }

                // ---- Track list (the scrollable part) ----
                Text {
                    text: "TRACKS"
                    color: ripView.muted
                    font.family: "Menlo"; font.pixelSize: 9; font.bold: true
                    visible: ripper.discPresent
                }

                // Mini transport above the track list. Wired to the
                // same AudioEngine the main player uses — during a
                // batch rip this controls the streaming preview's
                // playback through the same QAudioSink. Duration +
                // position come from the synthetic preview segment
                // that the worker populated with the TOC-derived
                // disc duration when previewStreamStart fired.
                //
                // `ripView._previewActive` and `ripView._audioReady`
                // are lifted to the Popup so the track-list delegate
                // and these transport bindings share the same predicate.

                RowLayout {
                    Layout.fillWidth: true
                    visible: ripper.discPresent
                    spacing: 6

                    Button {
                        text: "⏮"
                        flat: true
                        Layout.preferredHeight: 24
                        Layout.preferredWidth: 28
                        font.pixelSize: 12
                        padding: 0
                        enabled: playlist.hasCurrent
                        onClicked: playlist.previous()
                    }
                    Button {
                        text: audio.playing ? "⏸" : "▶"
                        flat: true
                        Layout.preferredHeight: 24
                        Layout.preferredWidth: 30
                        font.pixelSize: 14
                        padding: 0
                        enabled: ripView._audioReady
                        onClicked: {
                            if (audio.playing) audio.pause()
                            else audio.play()
                        }
                    }
                    Button {
                        text: "⏭"
                        flat: true
                        Layout.preferredHeight: 24
                        Layout.preferredWidth: 28
                        font.pixelSize: 12
                        padding: 0
                        enabled: playlist.hasCurrent
                        onClicked: playlist.next()
                    }

                    PLYRSeekSlider {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 20
                        enabled: ripView._audioReady && ripView._trackDurationMs > 0
                        from: 0
                        to:   Math.max(1, ripView._trackDurationMs / 1000.0)
                        value: ripView._trackPositionMs / 1000.0
                        // Seek targets the disc-absolute position: offset the
                        // per-track value by the current track's start.
                        onMoved: (newValue) =>
                            audio.seek(ripView._streamTrackStartMs + newValue * 1000)
                    }

                    Text {
                        text: ripView.fmtTime(ripView._trackPositionMs / 1000.0)
                             + " / "
                             + ripView.fmtTime(ripView._trackDurationMs / 1000.0)
                        color: ripView.muted
                        font.family: "Menlo"
                        font.pixelSize: 9
                        Layout.preferredWidth: 78
                        horizontalAlignment: Text.AlignRight
                    }
                }

                ListView {
                    id: trackList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    visible: ripper.discPresent
                    model: ripper.tracks
                    spacing: 0
                    boundsBehavior: Flickable.StopAtBounds

                    // Auto-scroll: only when the currently-scanning track
                    // *number* changes (not on every progress tick). Keeps
                    // the active track visible across track boundaries
                    // while leaving the user free to scroll within a
                    // single track. `currentTrackNumber` shares the
                    // `progressChanged` NOTIFY with the per-tick progress
                    // scalars, so we throttle by binding through a local
                    // int property — the binding re-evaluates on every
                    // progress tick but the `onChanged` signal only fires
                    // when the integer value actually differs.
                    SmoothedAnimation {
                        id: trackScroll
                        target: trackList
                        property: "contentY"
                        duration: 220
                        velocity: -1
                    }
                    function centerOnCurrent() {
                        const n = ripper.currentTrackNumber
                        if (n <= 0 || count === 0) return
                        Qt.callLater(function() {
                            trackScroll.stop()
                            const oldY = trackList.contentY
                            trackList.positionViewAtIndex(n - 1, ListView.Center)
                            const targetY = trackList.contentY
                            trackList.contentY = oldY
                            trackScroll.to = targetY
                            trackScroll.start()
                        })
                    }
                    property int scanningTrack: ripper.currentTrackNumber
                    onScanningTrackChanged: centerOnCurrent()

                    delegate: Rectangle {
                        id: trackRow
                        width: ListView.view.width
                        height: 26

                        // CD-icon highlight: this is the row the worker
                        // is currently scanning off the disc. Wins over
                        // play-icon when both would be true.
                        readonly property bool isReadingThis:
                            modelData.number === ripper.currentTrackNumber

                        // "Currently playing" splits into two cases:
                        //
                        //  (a) Streaming preview is the active audio
                        //      source. The streaming source URL is
                        //      `preview://cd-rip`, NOT a folder path —
                        //      so we can't use playlist.folderPath here.
                        //      Instead, compute a 0..1 fraction from
                        //      audio.position / audio.duration and
                        //      check whether it lands in this row's
                        //      [startFraction, endFraction) window.
                        //      Guarded with `_previewActive` so during
                        //      normal FLAC playback this branch never
                        //      lights up rows on neighbouring discs.
                        //
                        //  (b) The user has clicked into one of the
                        //      disc's saved FLAC tracks — playlist
                        //      drives playback now, audio.source is a
                        //      file:// URL, and we match by track
                        //      number AND folder-rooted-in-this-disc.
                        //      `currentDiscPlaybackPath` is the temp
                        //      dir during the rip and the saved path
                        //      after; it's the right anchor either way.
                        readonly property bool _previewPlayingHere: {
                            if (!ripView._previewActive) return false
                            if (audio.duration <= 0) return false
                            const sf = (modelData.startFraction !== undefined)
                                       ? modelData.startFraction : -1
                            const ef = (modelData.endFraction !== undefined)
                                       ? modelData.endFraction : -1
                            if (sf < 0 || ef <= sf) return false
                            const frac = audio.position / audio.duration
                            return frac >= sf && frac < ef
                        }
                        // File-playback case: the user has clicked a
                        // saved FLAC; audio.source is a file:// URL
                        // pointing into the disc's folder. We check the
                        // URL itself rather than playlist.folderPath
                        // because during a batch rip the playlist is
                        // rooted at the batch *parent*, not the per-
                        // disc folder. Match by track number + the
                        // source being inside the disc folder.
                        readonly property bool _filePlayingHere: {
                            if (ripView._previewActive) return false
                            if (!playlist.hasCurrent) return false
                            if (playlist.currentTrackNumber !== modelData.number)
                                return false
                            const discPath = ripper.currentDiscPlaybackPath
                            if (!discPath) return false
                            const src = audio.source
                                        ? audio.source.toString() : ""
                            if (!src.startsWith("file://")) return false
                            // file:// + path-prefix match means the
                            // currently-playing file lives inside the
                            // disc folder (with a trailing slash so
                            // /foo doesn't match /foo-bar).
                            return src.indexOf(discPath + "/") >= 0
                        }

                        readonly property bool isPlayingHere:
                            !isReadingThis
                            && (_previewPlayingHere || _filePlayingHere)

                        // Pending = the worker hasn't even started
                        // reading this track. Its FLAC doesn't exist
                        // yet, so it's not playable; render greyed
                        // out and swallow no clicks.
                        readonly property bool _isPending:
                            (modelData.status || "") === "pending"

                        opacity: _isPending ? 0.4 : 1.0

                        color: (isPlayingHere || isReadingThis)
                                ? Qt.rgba(0.05, 0.10, 0.20, 1.0)
                                : "transparent"

                        // Single-click is a no-op (could later select
                        // for keyboard nav). Double-click jumps audio
                        // playback to this track via the playlist —
                        // playCurrentAndQueueNext in main.cpp does the
                        // actual play+enqueue once setCurrentIndex
                        // fires. Disabled on pending rows so they read
                        // as not-yet-playable. MouseArea-only (no
                        // accepted = false games) — the ListView's
                        // Flickable handles scroll/drag independently.
                        MouseArea {
                            anchors.fill: parent
                            enabled: !trackRow._isPending
                            cursorShape: enabled ? Qt.PointingHandCursor
                                                 : Qt.ArrowCursor
                            onDoubleClicked: {
                                if (!modelData) return
                                // Find the playlist row whose path is
                                // <discFolder>/track_NN.flac for this
                                // track number. The rip worker names
                                // FLACs as track_%02u.flac, and
                                // PlaylistModel.appendTrack stores the
                                // absolute path as Track.url.
                                const n = modelData.number
                                const idx = playlist.indexOfRipTrack(
                                    ripper.currentDiscPlaybackPath, n)
                                if (idx >= 0) {
                                    playlist.setCurrentIndex(idx)
                                }
                                // Else: encoding is in flight but
                                // discTrackReady hasn't fired yet for
                                // this row — silently no-op, the user
                                // can double-click again a moment later.
                            }
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 2
                            anchors.rightMargin: 4
                            spacing: 8

                            Text {
                                Layout.preferredWidth: 22
                                text: (modelData.number < 10 ? "0" : "")
                                      + modelData.number
                                color: (trackRow.isPlayingHere
                                        || trackRow.isReadingThis)
                                       ? ripView.accent : ripView.veryMuted
                                font.family: "Menlo"; font.pixelSize: 10
                                horizontalAlignment: Text.AlignRight
                            }
                            Item {
                                Layout.preferredWidth: 16
                                Layout.preferredHeight: 14

                                // "Now playing" indicator: animated bars
                                // when this row is the audio source and
                                // playback is active. Otherwise the icon
                                // slot shows the appropriate ▶ / 💿 /
                                // status glyph.
                                readonly property bool _showBars:
                                    trackRow.isPlayingHere
                                    && audio.playing
                                    && !trackRow.isReadingThis

                                PlayingBars {
                                    anchors.centerIn: parent
                                    visible: parent._showBars
                                    fft: ripView.fftRef
                                    height: 12
                                }

                                Text {
                                    anchors.centerIn: parent
                                    visible: !parent._showBars
                                    text: {
                                        if (trackRow.isReadingThis) return "💿"
                                        if (trackRow.isPlayingHere)
                                            return audio.playing ? "" : "▶"
                                        const st = modelData.status || ""
                                        if (st === "ok")       return "✓"
                                        if (st === "warn")     return "⚠"
                                        if (st === "fail")     return "✕"
                                        if (st === "reading")  return "💿"
                                        if (st === "encoded")  return "·"
                                        if (st === "read")     return "·"
                                        return ""
                                    }
                                    color: {
                                        if (trackRow.isReadingThis) return ripView.accent
                                        if (trackRow.isPlayingHere) return ripView.accent
                                        const st = modelData.status || ""
                                        if (st === "ok")   return ripView.accent
                                        if (st === "warn") return Qt.rgba(1.0, 0.78, 0.30, 0.95)
                                        if (st === "fail") return Qt.rgba(1.0, 0.45, 0.42, 0.95)
                                        return ripView.veryMuted
                                    }
                                    font.pixelSize: 11
                                    horizontalAlignment: Text.AlignHCenter
                                }
                            }
                            Text {
                                Layout.fillWidth: true
                                text: modelData.title || "Track " + modelData.number
                                color: (trackRow.isPlayingHere
                                        || trackRow.isReadingThis)
                                       ? ripView.primary : ripView.muted
                                font.pixelSize: 11
                                elide: Text.ElideRight
                            }
                            Text {
                                text: ripView.fmtTime(modelData.durationSec || 0)
                                color: ripView.veryMuted
                                font.family: "Menlo"; font.pixelSize: 10
                            }
                        }
                    }

                    ScrollBar.vertical: ScrollBar { }
                }
            }
        }

        // Bottom bar removed — close goes through the top-left
        // stoplight, save auto-opens at SavePending, eject + batch
        // navigation will be reachable from elsewhere if needed.
    }

    // Close-confirmation dialog. Asks Resume later vs Delete batch when
    // the rip is mid-flight, so the user doesn't accidentally lose their
    // batch state by clicking the close stoplight.
    Dialog {
        id: confirmDialog
        modal: true
        parent: Overlay.overlay
        x: (parent.width  - width)  / 2
        y: (parent.height - height) / 2

        property bool inBatchAtOpen: false

        title: "Stop ripping?"
        standardButtons: Dialog.NoButton

        contentItem: ColumnLayout {
            spacing: 14
            Text {
                Layout.preferredWidth: 360
                Layout.fillWidth: true
                text: confirmDialog.inBatchAtOpen
                    ? "The in-progress disc will be discarded. You can resume this batch later from the Rip CD menu."
                    : "The current disc rip will be discarded."
                color: "white"
                wrapMode: Text.WordWrap
                font.pixelSize: 12
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Item { Layout.fillWidth: true }
                Button {
                    text: "Keep ripping"
                    onClicked: confirmDialog.close()
                }
                Button {
                    text: "Delete batch"
                    visible: confirmDialog.inBatchAtOpen
                    onClicked: {
                        ripper.stopRip(/*deleteBatch=*/true)
                        confirmDialog.close()
                        ripView.close()
                    }
                }
                Button {
                    text: confirmDialog.inBatchAtOpen
                          ? "Resume later" : "Stop"
                    highlighted: true
                    onClicked: {
                        ripper.stopRip(/*deleteBatch=*/false)
                        confirmDialog.close()
                        ripView.close()
                    }
                }
            }
        }
    }

    // Save picker — separate FolderPicker instance so its accepted signal
    // routes into ripper.saveTo() rather than the playlist (Main.qml's
    // picker still does the play-a-folder routing).
    //
    // Cancelling the picker while in SavePending is unusual (the user
    // dismissed a dialog asking where to save a freshly-completed rip),
    // so we surface a follow-up confirmation rather than silently
    // leaving them stranded with a saveless rip.
    property bool _saveAccepted: false
    FolderPicker {
        id: savePicker
        parent: Overlay.overlay
        x: (parent.width  - width)  / 2
        y: (parent.height - height) / 2
        width:  Math.min(parent.width  - 80, 720)
        height: Math.min(parent.height - 80, 520)
        recents: ripView.saveRecents
        onAccepted: (url) => {
            ripView._saveAccepted = true
            ripper.saveTo(url)
        }
        onClosed: {
            if (!ripView._saveAccepted
                && ripper.state === 6 /*SavePending*/) {
                cancelSaveDialog.open()
            }
            ripView._saveAccepted = false
        }
    }

    // Confirmation shown when the user dismisses the save picker mid-
    // SavePending. Choices: re-open the save picker, or delete the
    // freshly-ripped tracks and stop the session.
    Dialog {
        id: cancelSaveDialog
        modal: true
        parent: Overlay.overlay
        x: (parent.width  - width)  / 2
        y: (parent.height - height) / 2
        title: "Delete this rip?"
        standardButtons: Dialog.NoButton

        background: Rectangle {
            color: Qt.rgba(0.08, 0.08, 0.10, 0.98)
            border.color: ripView.subtleLine
            border.width: 1
            radius: 10
        }

        contentItem: ColumnLayout {
            spacing: 14
            Text {
                Layout.preferredWidth: 380
                Layout.fillWidth: true
                text: "The disc is ripped but hasn't been saved. Cancel "
                    + "and the ripped tracks will be discarded."
                color: "white"
                wrapMode: Text.WordWrap
                font.pixelSize: 12
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Item { Layout.fillWidth: true }
                Button {
                    text: "Choose folder"
                    highlighted: true
                    onClicked: {
                        cancelSaveDialog.close()
                        savePicker.volumes = ripView.saveVolumes
                        const initial = Platform.StandardPaths.writableLocation(
                            Platform.StandardPaths.MusicLocation)
                        savePicker.openAt(initial)
                    }
                }
                Button {
                    text: "Delete rip"
                    onClicked: {
                        cancelSaveDialog.close()
                        // Drop the staged temp dir but keep the batch
                        // resumable (the disc itself can still be
                        // re-ripped later from a fresh insertion).
                        ripper.discardStagedRip()
                        // Single-disc rips end here; batches transition
                        // back to WaitingForDisc and stay open.
                        if (!ripper.inBatch) ripView.close()
                    }
                }
            }
        }
    }

    function requestClose() {
        const s = ripper.state
        // Idle / Done / Saving / Failed close immediately.
        if (s === 0 || s === 7 || s === 8 || s === 10) {
            ripper.endSession()
            close()
            return
        }
        // WaitingForDisc with no read in progress also closes silently —
        // there's nothing to discard.
        if (s === 1) {
            ripper.endSession()
            close()
            return
        }
        // Mid-rip: confirm.
        confirmDialog.inBatchAtOpen = ripper.inBatch
        confirmDialog.open()
    }
}
