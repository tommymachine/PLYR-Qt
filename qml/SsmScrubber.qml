// SsmScrubber -- a playback scrubber backed by a precomputed self-
// similarity matrix sidecar (Layer 4b).
//
// Two display modes:
//   * Inline -- one row of the matrix drawn as a horizontal strip
//               (slim, for use behind a seek bar). Bright cells along
//               the strip = "this point in the song sounds like the
//               current playback position".
//   * Wide   -- the full 2D heatmap, with a crosshair tracking the
//               playback fraction. Great for inspecting song structure
//               at a glance: visible blocks on the diagonal = sections,
//               off-diagonal stripes = repeats.
//
// Usage:
//   SsmScrubber {
//       ssmPath: "/path/to/track.flac.ssm"
//       playbackFraction: audio.position / Math.max(1, audio.duration)
//       displayMode: SsmScrubber.Wide
//       tintColor: "#5BBFFF"
//   }

import QtQuick
import PLYR

Item {
    id: root

    // Filesystem path to a .ssm sidecar. Empty = nothing loaded; the
    // ANALYZING placeholder stays hidden too (until a path is set the
    // user hasn't asked for an SSM display in the first place).
    property string ssmPath: ""

    // Playback head as a fraction of song length, [0, 1]. Bound by the
    // caller from audio.position / audio.duration; we don't peek into
    // the AudioFeatures here ourselves because the parent decides which
    // track's SSM is being shown.
    property real playbackFraction: 0.0

    // Display mode -- see component header. Use SsmScrubber.Inline or
    // SsmScrubber.Wide.
    enum DisplayMode { Inline, Wide }
    property int displayMode: SsmScrubber.Inline

    // Accent tint for the playback marker line. The SSM body uses magma,
    // so anything not in the magma range stands out.
    property color tintColor: "#5BBFFF"

    // Read-only diagnostics for QML callers that want to react to load
    // state (e.g. dimming the scrubber until ANALYZING is done).
    readonly property bool loaded: loader.loaded
    readonly property int  dim:    loader.dim

    SsmLoader { id: loader; sourceFile: root.ssmPath }
    SsmTexture { id: ssmTex; loader: loader; visible: false }

    // Wide mode -- 2D heatmap with crosshair. Visible only when both
    // displayMode requests it AND the loader has finished.
    ShaderEffect {
        anchors.fill: parent
        visible: root.displayMode === SsmScrubber.Wide && loader.loaded
        property var   source: ssmTex
        property real  playbackFrac: root.playbackFraction
        property color tintColor: root.tintColor
        vertexShader: "qrc:/shaders/ssm_2d.vert.qsb"
        fragmentShader: "qrc:/shaders/ssm_2d.frag.qsb"
    }

    // Inline mode -- one-row strip. Same shader pair as Wide, different
    // semantic; we use the dedicated inline shader so the strip looks
    // clean even when the QML quad is much wider than tall.
    ShaderEffect {
        anchors.fill: parent
        visible: root.displayMode === SsmScrubber.Inline && loader.loaded
        property var   source: ssmTex
        property real  playbackFrac: root.playbackFraction
        property color tintColor: root.tintColor
        vertexShader: "qrc:/shaders/ssm_inline.vert.qsb"
        fragmentShader: "qrc:/shaders/ssm_inline.frag.qsb"
    }

    // Loading placeholder. Shown only when a sidecar was requested but
    // hasn't loaded yet (e.g. the sidecar is being generated offline by
    // a background worker). Once loader.loaded flips true the shader
    // effects take over.
    Text {
        anchors.centerIn: parent
        visible: !loader.loaded && root.ssmPath !== ""
        text: loader.error.length > 0 ? "SSM ERROR" : "ANALYZING…"
        color: Qt.rgba(1, 1, 1, 0.4)
        font.family: "Iosevka"
        font.pixelSize: 10
    }
}
