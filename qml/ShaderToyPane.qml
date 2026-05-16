// ShaderToyPane — Layer 3a of the visualizer roadmap. Hosts a single
// ShaderEffect whose fragmentShader URL is swapped between a library of
// bundled "ShaderToy-style" .qsb shaders. Each shader is a self-contained
// piece of GPU art driven by:
//
//   * iChannel0   — the 512x2 R8 SpectrumTexture (row 0 = FFT, row 1 =
//                   waveform). Matches ShaderToy's iChannel0 convention,
//                   so existing ShaderToy code translates with near-zero
//                   changes (the only real difference is binding number).
//   * iTime       — seconds since the pane was created.
//   * iResolution — vec3(width, height, 1).
//   * iMouse      — vec4(x, y, pressed, 0).
//   * bass / mid / treb / rms / centroid / flux — scalar audio features
//                   from AudioFeatures (passed as uniforms so the shader
//                   doesn't have to re-derive them by sampling iChannel0).
//
// Cycling: chevron button top-right, OR left/right arrow keys when the
// pane has keyboard focus. Index wraps both directions.
//
// New shaders: add a file pair (vert + frag) under shaders/ and an entry
// in the shaderLibrary list below. Note that each shader needs its own
// .vert (even though the four available ones are identical) — qt_add_shaders
// bakes per-file, not per-pair.

import QtQuick
import QtQuick.Controls
import PLYR

Item {
    id: root

    // The AudioFeatures pointer is normally taken from the QML context
    // property `audioFeatures` exposed by main.cpp. Override if you
    // want a different source for testing.
    property var audioSource: audioFeatures

    // Each entry: { name, vert, frag, desc }. The cycle button shows
    // `name`; the description tag shows `desc`.
    readonly property var shaderLibrary: [
        { name: "Polar Spectrum",
          vert: "qrc:/shaders/st_polar_spectrum.vert.qsb",
          frag: "qrc:/shaders/st_polar_spectrum.frag.qsb",
          desc: "Spectrum drawn around a polar coordinate" },
        { name: "Domain Warp",
          vert: "qrc:/shaders/st_domain_warp.vert.qsb",
          frag: "qrc:/shaders/st_domain_warp.frag.qsb",
          desc: "Bass warps an fBm noise field" },
        { name: "Spectrum Tunnel",
          vert: "qrc:/shaders/st_spectrum_tunnel.vert.qsb",
          frag: "qrc:/shaders/st_spectrum_tunnel.frag.qsb",
          desc: "Raymarched tunnel; FFT drives wall pattern" },
        { name: "Waveform Heightmap",
          vert: "qrc:/shaders/st_waveform_heightmap.vert.qsb",
          frag: "qrc:/shaders/st_waveform_heightmap.frag.qsb",
          desc: "Waveform as a heightmap landscape" },
        { name: "Reaction-Diffusion",
          vert: "qrc:/shaders/st_reaction_diffusion.vert.qsb",
          frag: "qrc:/shaders/st_reaction_diffusion.frag.qsb",
          desc: "Gray-Scott audio-modulated (NB: stateless approximation)" }
    ]
    property int currentIndex: 0

    // The 512x2 spectrum/waveform texture. Owned by SpectrumTexture
    // (C++ QQuickItem registered as a QML element); it re-uploads its
    // bytes whenever AudioFeatures::featuresUpdated() fires.
    SpectrumTexture {
        id: specTex
        features: root.audioSource
        // Doesn't need to paint itself — only exists to be sampled by
        // the ShaderEffect below. Hide so it doesn't grab any layout
        // space.
        visible: false
    }

    // Drives iTime — accumulating seconds since the component was
    // created. We don't use a NumberAnimation here because Qt's QML AOT
    // compiler treats Animation.duration as an int and rejects literals
    // larger than INT32_MAX (~24.8 days in ms). A Timer driving a
    // floating-point accumulator has no such limit; it also gives us
    // an explicit, easy-to-pause iTime if we ever need it.
    property real elapsed: 0
    Timer {
        id: clockTick
        interval: 16        // ~60 Hz; matches the AudioFeatures refresh.
        running: true
        repeat: true
        property real lastMs: 0
        onTriggered: {
            const now = Date.now()
            if (lastMs === 0) {
                lastMs = now
                return
            }
            root.elapsed += (now - lastMs) / 1000.0
            lastMs = now
        }
    }

    // Mouse tracking for iMouse. Hover-enabled so the position updates
    // even without a click — matches ShaderToy's behaviour.
    property point mousePos: Qt.point(0, 0)
    property bool mousePressed: false
    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        onPositionChanged: (m) => root.mousePos = Qt.point(m.x, m.y)
        onPressedChanged: root.mousePressed = pressed
        // Forward focus so arrow keys work after a click.
        onClicked: root.forceActiveFocus()
    }

    ShaderEffect {
        id: paint
        anchors.fill: parent

        // -- Standard ShaderToy uniforms ----------------------------------
        property var       iChannel0: specTex
        property real      iTime: root.elapsed
        property vector3d  iResolution: Qt.vector3d(width, height, 1)
        property vector4d  iMouse: Qt.vector4d(
            root.mousePos.x,
            root.mousePos.y,
            root.mousePressed ? 1.0 : 0.0,
            0.0)

        // -- Audio scalars (UBO uniforms) ---------------------------------
        // Passed individually so shaders can drive global effects without
        // re-sampling iChannel0 (cheaper, and not subject to row-0/row-1
        // sampling ambiguity). Bound to AudioFeatures' attack-followed
        // properties so each one has musical envelope behaviour.
        // NB: GLSL reserves the bare identifier `centroid` as an
        // interpolation qualifier (alongside `flat`, `smooth`, etc.). To
        // avoid the parser tripping at compile time, both the QML property
        // and the UBO field use the short alias `cent`. Spectral-centroid
        // semantics unchanged — value still comes from
        // AudioFeatures::centroid_norm.
        property real bass: root.audioSource ? root.audioSource.bass_att     : 0.0
        property real mid:  root.audioSource ? root.audioSource.mid_att      : 0.0
        property real treb: root.audioSource ? root.audioSource.treb_att     : 0.0
        property real rms:  root.audioSource ? root.audioSource.rms_att      : 0.0
        property real cent: root.audioSource ? root.audioSource.centroid_norm: 0.0
        property real flux: root.audioSource ? root.audioSource.flux_norm    : 0.0

        // Switching shader URLs at runtime is what makes cycling work —
        // ShaderEffect re-bakes its pipeline when these change.
        vertexShader:   root.shaderLibrary[root.currentIndex].vert
        fragmentShader: root.shaderLibrary[root.currentIndex].frag
    }

    // ---------------- Cycle button (top-right) -------------------------
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
            onClicked: root.currentIndex =
                (root.currentIndex + 1) % root.shaderLibrary.length
        }
        Text {
            id: nameLabel
            anchors.centerIn: parent
            text: root.shaderLibrary[root.currentIndex].name + "  ›"  // U+203A SINGLE RIGHT-POINTING ANGLE QUOTATION MARK
            color: "white"
            font.family: "Iosevka"
            font.pixelSize: 10
        }
    }

    // ---------------- Description tag (bottom-left) --------------------
    Text {
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.margins: 12
        text: root.shaderLibrary[root.currentIndex].desc
        color: Qt.rgba(1, 1, 1, 0.4)
        font.family: "Iosevka"
        font.pixelSize: 9
    }

    // ---------------- Keyboard cycling ---------------------------------
    Keys.onRightPressed: currentIndex =
        (currentIndex + 1) % shaderLibrary.length
    Keys.onLeftPressed: currentIndex =
        (currentIndex - 1 + shaderLibrary.length) % shaderLibrary.length
    focus: true
}
