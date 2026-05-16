// AudioFeaturesHud — debug overlay for the AudioFeatures (Layer 0) DSP.
// Not wired into Main.qml — drop into a parent during dev to verify the
// extractor is producing sensible numbers. One-line invocation:
//
//   AudioFeaturesHud { anchors.top: parent.top; anchors.right: parent.right; anchors.margins: 12 }
//
// Bars: per-band attack-released envelope (BASS/MID/TREB/AIR), RMS.
// Numbers: spectral centroid (Hz), L/R phase correlation.
// Dot: flashes white for ~120 ms on each AudioFeatures.onset() signal.
// Strips: 512-px spectrum (row 0 of the 512×2 texture) + waveform (row 1).

import QtQuick
import PLYR

Item {
    id: root
    width:  500
    height: 260

    // Maximum bar value we show. Bands are calibrated to ≈ 1.0 on
    // moderately loud pink noise; cap at 1.5 so headroom is visible.
    readonly property real barMax: 1.5

    // White-on-black panel.
    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(0, 0, 0, 0.78)
        border.color: Qt.rgba(1, 1, 1, 0.12)
        border.width: 1
        radius: 4
    }

    Column {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 6

        // --- Per-band bars ---------------------------------------------
        Repeater {
            model: [
                { label: "BASS", value: () => audioFeatures.bass_att },
                { label: "MID",  value: () => audioFeatures.mid_att  },
                { label: "TREB", value: () => audioFeatures.treb_att },
                { label: "AIR",  value: () => audioFeatures.air_att  }
            ]
            delegate: Row {
                required property var modelData
                spacing: 8
                Text {
                    text: modelData.label
                    color: "white"
                    font.family: "Menlo"
                    font.pixelSize: 10
                    width: 40
                }
                Rectangle {
                    width: 360
                    height: 12
                    color: Qt.rgba(1, 1, 1, 0.08)
                    radius: 2
                    Rectangle {
                        width: Math.min(parent.width,
                                        parent.width * (modelData.value() / root.barMax))
                        height: parent.height
                        color: "white"
                        radius: 2
                    }
                }
                Text {
                    text: modelData.value().toFixed(2)
                    color: Qt.rgba(1, 1, 1, 0.65)
                    font.family: "Menlo"
                    font.pixelSize: 10
                    width: 36
                }
            }
        }

        // --- RMS bar --------------------------------------------------
        Row {
            spacing: 8
            Text {
                text: "RMS"
                color: "white"
                font.family: "Menlo"
                font.pixelSize: 10
                width: 40
            }
            Rectangle {
                width: 360
                height: 12
                color: Qt.rgba(1, 1, 1, 0.08)
                radius: 2
                Rectangle {
                    width: Math.min(parent.width,
                                    parent.width * (audioFeatures.rms_att / root.barMax))
                    height: parent.height
                    color: Qt.rgba(0.6, 0.95, 0.6, 1)
                    radius: 2
                }
            }
            Text {
                text: audioFeatures.rms_att.toFixed(2)
                color: Qt.rgba(1, 1, 1, 0.65)
                font.family: "Menlo"
                font.pixelSize: 10
                width: 36
            }
        }

        // --- Numeric readouts + onset dot -----------------------------
        Row {
            spacing: 14
            Text {
                text: "centroid " + audioFeatures.centroid_hz.toFixed(0) + " Hz"
                color: "white"
                font.family: "Menlo"
                font.pixelSize: 10
            }
            Text {
                text: "phase " + audioFeatures.phase_corr.toFixed(2)
                color: "white"
                font.family: "Menlo"
                font.pixelSize: 10
            }
            // Onset indicator. Goes opaque on each onset signal, fades
            // to invisible over 120 ms via the OpacityAnimator below.
            Rectangle {
                id: onsetDot
                width: 10
                height: 10
                radius: 5
                color: "white"
                opacity: 0
            }
        }

        Connections {
            target: audioFeatures
            function onOnset() {
                onsetDot.opacity = 1.0
                onsetFade.restart()
            }
        }
        NumberAnimation {
            id: onsetFade
            target: onsetDot
            property: "opacity"
            from: 1.0; to: 0.0
            duration: 120
        }

        // --- Spectrum + waveform strips -------------------------------
        // The 512×2 texture exposed by SpectrumTexture is the canonical
        // shader feed (drives every future visualizer layer). It's
        // instantiated here so the texture is uploaded once a frame
        // even while only the HUD is visible — a future RHI item or
        // baked .qsb shader can render it directly.
        //
        // The Canvas2D strips below are intentional dev-time proxies:
        // they approximate the spectrum / waveform shape from the
        // per-band envelopes + RMS without going through the texture
        // (Canvas2D can't sample a QSGTexture). They're enough to verify
        // that "audio is flowing and reasonable" — for true shader-fed
        // visualization, use SpectrumTexture from a ShaderEffect with a
        // baked .qsb shader.
        SpectrumTexture {
            id: specTex
            features: audioFeatures
            visible: false
        }

        Canvas {
            id: specCanvas
            width: 480
            height: 32
            antialiasing: false
            property var pix: null
            Connections {
                target: audioFeatures
                function onFeaturesUpdated() { specCanvas.requestPaint() }
            }
            onPaint: {
                const ctx = getContext("2d")
                ctx.clearRect(0, 0, width, height)
                ctx.fillStyle = "rgba(255,255,255,0.08)"
                ctx.fillRect(0, 0, width, height)
                ctx.fillStyle = "white"
                // Render 64 columns sampled from 512 raw bins via fft
                // bands as a proxy — the actual spectrum-row data is in
                // the texture (audioFeatures owns it); rendering it via
                // Canvas2D would require a pixel-reader path we don't
                // have. We approximate using the four band envelopes
                // expanded into a 64-bin curve.
                const cols = 64
                for (let i = 0; i < cols; ++i) {
                    const t = i / (cols - 1)
                    // Piecewise-linear interpolation across the four bands.
                    let v
                    if (t < 0.25)
                        v = audioFeatures.bass_att * (1 - t * 4)
                          + audioFeatures.mid_att  * (t * 4)
                    else if (t < 0.5)
                        v = audioFeatures.mid_att  * (1 - (t - 0.25) * 4)
                          + audioFeatures.treb_att * ((t - 0.25) * 4)
                    else
                        v = audioFeatures.treb_att * (1 - (t - 0.5) * 2)
                          + audioFeatures.air_att  * ((t - 0.5) * 2)
                    v = Math.min(1.0, v / root.barMax)
                    const colW = width / cols
                    const h = height * v
                    ctx.fillRect(i * colW + 1, height - h, colW - 2, h)
                }
            }
        }

        // Waveform strip: a centered horizontal line driven by the
        // mono signal envelope's peak — useful as a "is audio running"
        // sanity light. Same approximation caveat as the spectrum.
        Canvas {
            id: wavCanvas
            width: 480
            height: 28
            antialiasing: false
            Connections {
                target: audioFeatures
                function onFeaturesUpdated() { wavCanvas.requestPaint() }
            }
            onPaint: {
                const ctx = getContext("2d")
                ctx.clearRect(0, 0, width, height)
                ctx.fillStyle = "rgba(255,255,255,0.06)"
                ctx.fillRect(0, 0, width, height)
                ctx.strokeStyle = "white"
                ctx.lineWidth = 1
                ctx.beginPath()
                const mid = height / 2
                const amp = audioFeatures.peak * (mid - 2)
                // Synthetic sine driven by peak amplitude — gives the
                // dev a visual heartbeat even without sampling the
                // texture's waveform row.
                for (let x = 0; x < width; ++x) {
                    const phase = x / width * Math.PI * 6
                    const y = mid + Math.sin(phase + (Date.now() / 100)) * amp
                    if (x === 0) ctx.moveTo(x, y)
                    else         ctx.lineTo(x, y)
                }
                ctx.stroke()
            }
        }
    }
}
