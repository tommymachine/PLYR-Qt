import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qt.labs.folderlistmodel
import Qt.labs.platform as Platform

// In-process folder browser that replaces Qt's QtQuick.Dialogs.FolderDialog
// on desktop. The native NSOpenPanel-via-Qt path was visibly slow on first
// open (QML fallback impl loaded, even with the macos_prewarm in place).
// FolderListModel walks one directory at a time, lazily — opening this
// popup is essentially free.
//
// Usage:
//   FolderPicker {
//       id: picker
//       recents: playlist.recentFolders   // optional StringList of paths
//       onAccepted: (url) => playlist.openFolderUrl(url)
//   }
//   picker.openAt(initialUrl)
Popup {
    id: picker
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    padding: 0

    // StringList of absolute paths, rendered as the sidebar's RECENTS
    // section. Empty → section is hidden. Caller wires this to whatever
    // recents source is appropriate.
    property var recents: []

    // List of {name, url} maps, rendered as the sidebar's VOLUMES section.
    // Empty → section is hidden. Re-populated by the caller before each
    // open() so newly mounted/ejected drives stay accurate.
    property var volumes: []

    signal accepted(url folder)

    // ---- Navigation history (back/forward stacks) --------------------
    // Updated automatically when folderModel.folder changes via user
    // navigation (sidebar click, double-click, up button). Programmatic
    // back/forward navigation sets _navigating=true to suppress the
    // history push for that one transition.
    property var  _backStack: []
    property var  _forwardStack: []
    property url  _previousFolder: ""
    property bool _navigating: false

    Connections {
        target: folderModel
        function onFolderChanged() {
            if (picker._navigating) {
                picker._navigating = false
            } else if (picker._previousFolder.toString() !== "") {
                // Reassign (not push) so the var-property change notifies
                // bound enabled states on the back/forward buttons.
                const next = picker._backStack.slice()
                next.push(picker._previousFolder)
                picker._backStack = next
                picker._forwardStack = []
            }
            picker._previousFolder = folderModel.folder
        }
    }

    function _goBack() {
        if (_backStack.length === 0) return
        const back = _backStack.slice()
        const dest = back.pop()
        const fwd  = _forwardStack.slice()
        fwd.push(_previousFolder)
        _backStack = back
        _forwardStack = fwd
        _navigating = true
        folderModel.folder = dest
    }

    function _goForward() {
        if (_forwardStack.length === 0) return
        const fwd  = _forwardStack.slice()
        const dest = fwd.pop()
        const back = _backStack.slice()
        back.push(_previousFolder)
        _backStack = back
        _forwardStack = fwd
        _navigating = true
        folderModel.folder = dest
    }

    function openAt(initialFolder) {
        // Each picker session starts with a clean history.
        _backStack = []
        _forwardStack = []
        _previousFolder = ""
        _navigating = true   // suppress the push for the initial set
        if (initialFolder && initialFolder.toString() !== "")
            folderModel.folder = initialFolder
        // Cover the case where folderModel was already at this URL — no
        // folderChanged would fire to clear _navigating, so do it here.
        _navigating = false
        _previousFolder = folderModel.folder
        open()
    }

    function _parentOf(folderUrl) {
        const s = folderUrl.toString()
        const i = s.lastIndexOf("/")
        // Don't climb past "file:///" (i.e., the volume root).
        return (i > "file://".length) ? Qt.url(s.substring(0, i)) : folderUrl
    }

    function _displayPath(folderUrl) {
        return folderUrl.toString().replace(/^file:\/\//, "")
    }

    // Percent-encode each segment so paths with spaces/specials become
    // valid file:// URLs. Same logic as Main.qml's helper.
    function _pathToUrl(path) {
        return Qt.url("file://" +
                      path.split("/").map(encodeURIComponent).join("/"))
    }

    function _basename(path) {
        const i = path.lastIndexOf("/")
        return i >= 0 ? path.substring(i + 1) : path
    }

    background: Rectangle {
        color: Qt.rgba(0.07, 0.07, 0.09, 0.98)
        border.color: Qt.rgba(1, 1, 1, 0.10)
        border.width: 1
        radius: 10
    }

    Overlay.modal: Rectangle { color: Qt.rgba(0, 0, 0, 0.55) }

    FolderListModel {
        id: folderModel
        showFiles: false
        showDirs: true
        showDotAndDotDot: false
        showHidden: false
        // Most-recently-modified first. Folders are usually touched when
        // music gets added/edited inside them, so this surfaces what the
        // user is likely reaching for. (FolderListModel's default Time
        // order is already newest-first; sortReversed would flip to
        // oldest-first.)
        sortField: FolderListModel.Time
    }

    // Sidebar "favorites" — pulled from QStandardPaths so it tracks the
    // user's localized standard locations. Computed once on Component
    // construction; these don't change at runtime.
    readonly property var _favorites: [
        { label: "Home",      icon: "🏠",
          url: Platform.StandardPaths.writableLocation(
                Platform.StandardPaths.HomeLocation) },
        { label: "Desktop",   icon: "🖥️",
          url: Platform.StandardPaths.writableLocation(
                Platform.StandardPaths.DesktopLocation) },
        { label: "Documents", icon: "📄",
          url: Platform.StandardPaths.writableLocation(
                Platform.StandardPaths.DocumentsLocation) },
        { label: "Downloads", icon: "⬇️",
          url: Platform.StandardPaths.writableLocation(
                Platform.StandardPaths.DownloadLocation) },
        { label: "Music",     icon: "🎵",
          url: Platform.StandardPaths.writableLocation(
                Platform.StandardPaths.MusicLocation) },
        { label: "Movies",    icon: "🎬",
          url: Platform.StandardPaths.writableLocation(
                Platform.StandardPaths.MoviesLocation) }
    ]

    contentItem: Item {

        // macOS-style close stoplight in the top-left. Colors sampled
        // from Big Sur+ (lwouis/macos-traffic-light-buttons-as-SVG):
        //   fill #ED6A5F · border #E24B41 · × stroke #460804.
        // The fill stays the same on hover; only the × appears, matching
        // Apple's actual behavior. Pressed state darkens to #BF4942.
        Rectangle {
            id: closeBtn
            width: 12
            height: 12
            radius: 6
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.topMargin: 12
            anchors.leftMargin: 12
            z: 10
            color: closeMouse.pressed ? "#BF4942" : "#ED6A5F"
            border.color: "#E24B41"
            border.width: 1
            antialiasing: true

            MouseArea {
                id: closeMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: picker.close()
            }

            // × glyph: two rotated 1.5px-thick Rectangles, ~67% of the
            // button width per Apple's measured proportion. Drawn this
            // way (rather than via a Text glyph) to keep the stroke
            // crisp at a small size.
            Item {
                anchors.centerIn: parent
                width: 8
                height: 8
                visible: closeMouse.containsMouse
                Rectangle {
                    anchors.centerIn: parent
                    width: parent.width
                    height: 1.5
                    color: "#B30000"
                    rotation: 45
                    antialiasing: true
                }
                Rectangle {
                    anchors.centerIn: parent
                    width: parent.width
                    height: 1.5
                    color: "#B30000"
                    rotation: -45
                    antialiasing: true
                }
            }
        }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ---- Middle: sidebar | (toolbar + folder list) --------------
        // Sidebar runs the full height (everything above the footer).
        // The toolbar sits on the right column only, so its bottom edge
        // is the same y as where the folder list starts.
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // Sidebar
            Rectangle {
                Layout.preferredWidth: 180
                Layout.fillHeight: true
                color: Qt.rgba(1, 1, 1, 0.02)

                Flickable {
                    anchors.fill: parent
                    contentHeight: sidebarColumn.implicitHeight
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds

                    ColumnLayout {
                        id: sidebarColumn
                        width: parent.width
                        spacing: 2

                        // ----- FAVORITES section -----
                        Text {
                            Layout.leftMargin: 14
                            // Align the FAVORITES label's top edge with
                            // the top of the first folder row's text on
                            // the right. The right column starts at
                            // toolbar (40) + divider (1) = 41px down,
                            // and the row text is vertically centered
                            // inside a 30px tall row, so its top sits
                            // ~8px further down.
                            Layout.topMargin: 49
                            Layout.bottomMargin: 4
                            text: "FAVORITES"
                            color: Qt.rgba(1, 1, 1, 0.40)
                            font.family: "Menlo"
                            font.pixelSize: 9
                            font.bold: true
                        }

                        Repeater {
                            model: picker._favorites
                            delegate: Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 26
                                color: favMouse.containsMouse
                                       ? Qt.rgba(1, 1, 1, 0.06)
                                       : (folderModel.folder.toString() === modelData.url.toString()
                                          ? Qt.rgba(0.05, 0.10, 0.20, 0.7)
                                          : "transparent")

                                MouseArea {
                                    id: favMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    onClicked: folderModel.folder = modelData.url
                                }

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 14
                                    anchors.rightMargin: 8
                                    spacing: 8

                                    Text {
                                        text: modelData.icon
                                        font.pixelSize: 12
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: modelData.label
                                        color: "white"
                                        font.pixelSize: 11
                                        elide: Text.ElideRight
                                    }
                                }
                            }
                        }

                        // ----- RECENTS section (hidden if empty) -----
                        Text {
                            visible: picker.recents.length > 0
                            Layout.leftMargin: 14
                            Layout.topMargin: 14
                            Layout.bottomMargin: 4
                            text: "RECENTS"
                            color: Qt.rgba(1, 1, 1, 0.40)
                            font.family: "Menlo"
                            font.pixelSize: 9
                            font.bold: true
                        }

                        Repeater {
                            model: picker.recents
                            delegate: Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 26
                                readonly property url _url: picker._pathToUrl(modelData)
                                color: recentMouse.containsMouse
                                       ? Qt.rgba(1, 1, 1, 0.06)
                                       : (folderModel.folder.toString() === _url.toString()
                                          ? Qt.rgba(0.05, 0.10, 0.20, 0.7)
                                          : "transparent")

                                MouseArea {
                                    id: recentMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    onClicked: folderModel.folder = parent._url
                                    ToolTip.text: modelData
                                    ToolTip.visible: containsMouse
                                    ToolTip.delay: 700
                                }

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 14
                                    anchors.rightMargin: 8
                                    spacing: 8

                                    Text {
                                        text: "🕘"
                                        font.pixelSize: 11
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: picker._basename(modelData)
                                        color: "white"
                                        font.pixelSize: 11
                                        elide: Text.ElideRight
                                    }
                                }
                            }
                        }

                        // ----- VOLUMES section (hidden if empty) -----
                        Text {
                            visible: picker.volumes.length > 0
                            Layout.leftMargin: 14
                            Layout.topMargin: 14
                            Layout.bottomMargin: 4
                            text: "VOLUMES"
                            color: Qt.rgba(1, 1, 1, 0.40)
                            font.family: "Menlo"
                            font.pixelSize: 9
                            font.bold: true
                        }

                        Repeater {
                            model: picker.volumes
                            delegate: Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 26
                                color: volMouse.containsMouse
                                       ? Qt.rgba(1, 1, 1, 0.06)
                                       : (folderModel.folder.toString() === modelData.url.toString()
                                          ? Qt.rgba(0.05, 0.10, 0.20, 0.7)
                                          : "transparent")

                                MouseArea {
                                    id: volMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    onClicked: folderModel.folder = modelData.url
                                }

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 14
                                    anchors.rightMargin: 8
                                    spacing: 8

                                    Text {
                                        text: "💽"
                                        font.pixelSize: 11
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: modelData.name
                                        color: "white"
                                        font.pixelSize: 11
                                        elide: Text.ElideRight
                                    }
                                }
                            }
                        }

                        Item { Layout.preferredHeight: 12 } // bottom pad
                    }
                }
            }

            // Vertical divider
            Rectangle {
                Layout.preferredWidth: 1
                Layout.fillHeight: true
                color: Qt.rgba(1, 1, 1, 0.08)
            }

            // Right column: toolbar above, folder list below
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                // ---- Toolbar (back/forward/up + path) ----------------
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 40
                    color: Qt.rgba(1, 1, 1, 0.04)

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 12
                        spacing: 2

                        Button {
                            text: "←"
                            flat: true
                            font.pixelSize: 16
                            Layout.preferredWidth: 30
                            Layout.preferredHeight: 32
                            padding: 0
                            enabled: picker._backStack.length > 0
                            onClicked: picker._goBack()
                            background: Rectangle { color: "transparent" }
                            ToolTip.text: "Back"
                            ToolTip.visible: hovered && enabled
                            ToolTip.delay: 600
                        }

                        Button {
                            text: "→"
                            flat: true
                            font.pixelSize: 16
                            Layout.preferredWidth: 30
                            Layout.preferredHeight: 32
                            padding: 0
                            enabled: picker._forwardStack.length > 0
                            onClicked: picker._goForward()
                            background: Rectangle { color: "transparent" }
                            ToolTip.text: "Forward"
                            ToolTip.visible: hovered && enabled
                            ToolTip.delay: 600
                        }

                        Button {
                            text: "↑"
                            flat: true
                            font.pixelSize: 16
                            Layout.preferredWidth: 30
                            Layout.preferredHeight: 32
                            padding: 0
                            enabled: picker._parentOf(folderModel.folder) !== folderModel.folder
                            onClicked: folderModel.folder = picker._parentOf(folderModel.folder)
                            background: Rectangle { color: "transparent" }
                            Layout.rightMargin: 6
                            ToolTip.text: "Parent folder"
                            ToolTip.visible: hovered && enabled
                            ToolTip.delay: 600
                        }

                        Text {
                            Layout.fillWidth: true
                            text: picker._displayPath(folderModel.folder)
                            color: "white"
                            font.pixelSize: 12
                            font.family: "Menlo"
                            elide: Text.ElideMiddle
                        }
                    }
                }

                // Toolbar / list divider
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: Qt.rgba(1, 1, 1, 0.08)
                }

                // ---- Folder list -----------------------------------
                ListView {
                    id: list
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: folderModel
                    currentIndex: -1

                Connections {
                    target: folderModel
                    function onFolderChanged() { list.currentIndex = -1 }
                }

                delegate: Rectangle {
                    width: ListView.view.width
                    height: 30
                    color: ListView.isCurrentItem
                           ? Qt.rgba(0.05, 0.10, 0.20, 1.0)
                           : (mouse.containsMouse ? Qt.rgba(1, 1, 1, 0.05) : "transparent")

                    MouseArea {
                        id: mouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: list.currentIndex = index
                        onDoubleClicked: {
                            // Use filePath + our URL encoder rather than
                            // relying on FolderListModel's fileURL role —
                            // role-name casing has been historically
                            // fragile across Qt versions.
                            const target = picker._pathToUrl(filePath)
                            console.log("FolderPicker: cd into", filePath)
                            folderModel.folder = target
                        }
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 14
                        anchors.rightMargin: 12
                        spacing: 8

                        Text {
                            text: "📁"
                            font.pixelSize: 13
                        }
                        Text {
                            Layout.fillWidth: true
                            text: fileName
                            color: "white"
                            font.pixelSize: 12
                            elide: Text.ElideRight
                        }
                    }
                }

                ScrollBar.vertical: ScrollBar { }

                Text {
                    anchors.centerIn: parent
                    visible: folderModel.count === 0 && folderModel.status === FolderListModel.Ready
                    text: "No subfolders"
                    color: Qt.rgba(1, 1, 1, 0.30)
                    font.pixelSize: 11
                }
            }
        }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: Qt.rgba(1, 1, 1, 0.08)
        }

        // ---- Footer: cancel + choose ---------------------------------
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 52
            color: Qt.rgba(1, 1, 1, 0.04)

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 8

                Item { Layout.fillWidth: true }

                Button {
                    text: "Cancel"
                    onClicked: picker.close()
                }

                Button {
                    readonly property url _target:
                        list.currentIndex >= 0
                            ? picker._pathToUrl(
                                folderModel.get(list.currentIndex, "filePath"))
                            : folderModel.folder

                    text: "Choose This Folder"
                    highlighted: true
                    onClicked: {
                        picker.accepted(_target)
                        picker.close()
                    }
                }
            }
        }
    }
    }
}
