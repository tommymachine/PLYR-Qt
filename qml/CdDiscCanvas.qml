import QtQuick

// CD graphic. Two layers:
//
//   * Bottom: a ShaderEffect running shaders/cd_disc.{vert,frag} that
//     renders the silvery surface, the diffraction-grating iridescence,
//     and the orbiting specular highlight. The `lightAngle` uniform
//     animates continuously, so the rainbow shifts around the disc the
//     way a real CD does under a moving lamp.
//
//   * Top: a Canvas2D with the data-driven overlays — concentric track
//     boundaries, the rotating "drive head" sweep line, and per-track
//     verify rings. These are sparse vector primitives that don't
//     benefit from a fragment shader, and keeping them in Canvas2D
//     lets the rip view's QML signals drive them directly.
//
// requestPaint() on the Canvas2D fires only when overlay inputs change.
// The shader repaints continuously while lightAngle is animating, which
// is fine — it's one screen-sized quad in compiled Qt RHI bytecode.
Item {
    id: root

    // ---- Inputs (set from RipView) -------------------------------
    property real readProgress: 0.0
    property real encodeProgress: 0.0
    property real verifyProgress: 0.0
    // tracks: [{ number, startFraction, endFraction, status, ... }]
    property var  tracks: []
    // Zero-fill ranges kept on the property for the rip log, but the
    // disc graphic doesn't render them — the track-row ⚠ in the
    // RipView panel carries that signal instead.
    property var  zeroFilledRanges: []

    // Visual mode (drives sweep + read fill). Matches Ripper.State:
    //   "idle" | "waiting" | "identifying" | "reading" | "encoding" |
    //   "verifying" | "savePending" | "saving" | "done" | "cancelling" |
    //   "failed"
    property string mode: "waiting"

    // ---- Palette --------------------------------------------------
    property color trackLineColor: Qt.rgba(0, 0, 0, 0.225)
    property color encodedColor: Qt.rgba(0.75, 0.95, 1.00, 0.85)
    property color verifyOkColor: Qt.rgba(0.55, 0.95, 0.65, 0.90)
    property color verifyWarnColor: Qt.rgba(1.00, 0.78, 0.30, 0.85)
    property color verifyFailColor: Qt.rgba(1.00, 0.45, 0.42, 0.90)

    // ---- Geometry (fractions of disc radius = 1.0) --------------
    // Real CD anatomy per ECMA-130 §8 + IFPI SID guide. Outer radius
    // is 60 mm, so 1.0 ≡ 60 mm. From the centre outward:
    //   0..hubR              : spindle hole          (real  0   .. 7.5 mm)
    //   hubR..clearOuter     : clear polycarbonate   (real 7.5  .. 16.5 mm)
    //   clearOuter..mirrorOuter : mirror band — IFPI / matrix etch lives here
    //                                                (real 16.5 .. 22.9 mm)
    //   mirrorOuter..dataOuter : pit region (lead-in + program + lead-out)
    //                                                (real 22.9 .. 58 mm)
    //   dataInner..dataOuter : user program area subset, where read-fill maps
    //                                                (real 25   .. 58 mm)
    //   dataOuter..1.0       : outer buffer + rim    (real 58   .. 60 mm)
    readonly property real _hubFraction:    0.125
    readonly property real _clearOuter:     0.275
    readonly property real _mirrorOuter:    0.382
    readonly property real _dataInner:      0.417
    readonly property real _dataOuter:      0.967

    // Animated uniforms.
    //
    //   _axisAngle:  orientation of the diameter the light sits on.
    //                Rotates slowly — the whole disc effectively turns
    //                around its centre.
    //   _viewPhase:  rotates the virtual viewer-tilt vector (V·T in the
    //                diffraction grating equation). Independent rate
    //                from _axisAngle, so the V·T term varies even when
    //                the light is stationary. This is what produces the
    //                "morphing rainbow" — every pixel sees its
    //                wavelength selection drift continuously.
    //   _sweepAngle: independent "drive head" sweep on the Canvas2D
    //                overlay (degrees, 0..360).
    property real _axisAngle: 0
    property real _viewPhase: 0
    property real _sweepAngle: 0

    NumberAnimation on _axisAngle {
        running: true
        from: 0; to: 2 * Math.PI
        duration: 52000
        loops: Animation.Infinite
    }
    NumberAnimation on _viewPhase {
        running: true
        from: 0; to: 2 * Math.PI
        duration: 22500
        loops: Animation.Infinite
    }

    // Swing-in animation. Triggered by the rip view's onOpened —
    // simultaneously sweeps the light's axis from −75° back into place
    // *and* pulls it inward from a distant radius so the iridescence
    // builds up as the light "approaches" the disc. Decoupled from the
    // continuous _axisAngle / lightRadius so the steady drift
    // underneath stays uninterrupted.
    property real _swingOffset: 0       // radians, added to axisAngle
    property real _radiusOffset: 0      // added to lightRadius
    ParallelAnimation {
        id: swingInAnim
        NumberAnimation {
            target: root
            property: "_swingOffset"
            from: -Math.PI * 0.42
            to: 0
            duration: 777
            easing.type: Easing.OutExpo
        }
        NumberAnimation {
            target: root
            property: "_radiusOffset"
            from: 4.5
            to: 0
            duration: 777
            easing.type: Easing.OutExpo
        }
    }
    function swingIn() {
        swingInAnim.stop()
        _swingOffset  = -Math.PI * 0.42
        _radiusOffset = 4.5
        swingInAnim.start()
    }

    // Sweep line — the "drive head" indicator. Only visible during
    // active phases; matches the disc's slower mechanical rotation.
    NumberAnimation on _sweepAngle {
        running: root.mode === "reading" || root.mode === "identifying"
                 || root.mode === "encoding" || root.mode === "verifying"
                 || root.mode === "waiting"
        from: 0; to: 360
        duration: 4500
        loops: Animation.Infinite
    }

    // ---- CD surface (shader) -------------------------------------
    // Square-cropped to the smaller dimension so the disc stays round
    // when the parent isn't square.
    ShaderEffect {
        id: discSurface
        width:  Math.min(parent.width, parent.height)
        height: width
        anchors.centerIn: parent

        // No layer.enabled — that was capping the FBO at logical pixels
        // (360² on a 2× display) and bilinear-upscaling, which made the
        // disc borders look pixel-stairstepped. Rendering directly lets
        // each fragment shader pixel land on a device pixel.

        // Geometry uniforms — match the Canvas2D overlay's fractions.
        property real hubR:             root._hubFraction
        property real clearOuter:       root._clearOuter
        property real mirrorOuter:      root._mirrorOuter
        property real dataInner:        root._dataInner
        property real dataOuter:        root._dataOuter

        // Animated. The swing-in offset rides on top of the continuous
        // _axisAngle rotation, so the light eases into place when the
        // rip view opens without disturbing the steady drift.
        property real axisAngle:        root._axisAngle + root._swingOffset
        // Light sits at a fixed radial offset along the rotating axis —
        // no centre crossing. Pushed beyond the disc rim (>1) so the
        // light reads as a distant source rather than something sitting
        // on the surface; the iridescence's L direction varies less
        // across the disc, giving a more directional rainbow.
        property real lightRadius:      2.60 + root._radiusOffset
        // Viewer-tilt vector rotates with _viewPhase at its own rate.
        // 0.18 magnitude is enough to read dramatically without going
        // unphysical — Recipe 6 from the research suggested 0.12 as a
        // starting point; bumped up for more obvious morph.
        property real viewTiltX:        Math.cos(root._viewPhase) * 0.18
        property real viewTiltY:        Math.sin(root._viewPhase) * 0.18
        property real readProgress:     root.readProgress

        // Visual tuning — overridable by the host if we want sliders
        // during design review. Values chosen by eye on a 360px disc.
        property real fringeSpacing:    1800.0
        property real iridescenceMix:   0.55
        property real specularStrength: 0.55

        // Matrix text mask — the shader uses its alpha channel to
        // composite iridescence inside the mirror band only where text
        // glyphs are. The diffraction physics (axisAngle, viewTilt,
        // lightRadius) applies to those pixels exactly as in the data
        // region, so the etched text breathes with the rest of the disc.
        property variant matrixMask: matrixSource

        vertexShader:   "qrc:/shaders/cd_disc.vert.qsb"
        fragmentShader: "qrc:/shaders/cd_disc.frag.qsb"
    }

    // ---- Data overlays (Canvas2D, on top) -----------------------
    // Two separate canvases so the expensive curved-text rendering
    // (strip-warped, iridescent) doesn't get redone every sweep tick.
    //   matrixCanvas — paints only when the disc identity / size
    //                  changes; pinned mirror-band matrix text.
    //   canvas       — paints on every sweep tick + progress change;
    //                  fast vector overlays (boundaries, sweep, rings).
    onTracksChanged: {
        canvas.requestPaint()
        matrixCanvas.requestPaint()
    }
    onModeChanged:             canvas.requestPaint()
    on_SweepAngleChanged:      canvas.requestPaint()
    onReadProgressChanged:     canvas.requestPaint()
    onEncodeProgressChanged:   canvas.requestPaint()
    onVerifyProgressChanged:   canvas.requestPaint()

    // The matrix-text canvas is consumed as a sampler by the disc
    // shader (via matrixSource below). We paint glyphs in solid white-
    // on-transparent so the shader can use the alpha channel directly
    // as a mask: wherever the mask is non-zero in the mirror band, the
    // shader applies the same iridescence physics it does in the data
    // region. The glyphs get the real light/V·T morphing, not a
    // baked-in static rainbow.
    Canvas {
        id: matrixCanvas
        anchors.fill: discSurface
        antialiasing: true
        visible: false   // only used as a texture source

        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()

            const cx = width / 2
            const cy = height / 2
            const R  = Math.min(width, height) / 2 - 2

            const clearOuter  = R * root._clearOuter
            const mirrorOuter = R * root._mirrorOuter
            const mirrorMid   = (clearOuter + mirrorOuter) / 2
            const bandH       = mirrorOuter - clearOuter
            const fontPx      = Math.max(8, Math.round(bandH * 0.42))

            const text = "CONCERTO v. " + Qt.application.version
                       + "  ·  CD-DA  ·  IFPI L3777  ·  "
                       + "Distributed by Three Seven Studios © "
                       + new Date().getFullYear()
                       + "  ⋆  ⋆  ⋆  "
            const n = text.length

            const totalSweep = Math.PI * 1.88
            const charSweep  = totalSweep / n
            const baseAng    = -Math.PI / 2 - totalSweep / 2

            ctx.font         = "bold " + fontPx + "px Iosevka"
            ctx.textAlign    = "center"
            ctx.textBaseline = "middle"
            ctx.fillStyle    = "white"   // mask — shader colours it

            // Per-character wedge in 3 horizontal strips so the glyph
            // contracts toward the centre. The shader picks up this
            // shape as a mask and runs the diffraction physics through it.
            const numStrips = 3
            const stripH    = fontPx / numStrips
            const halfH     = fontPx / 2

            for (let i = 0; i < n; ++i) {
                const a = baseAng + (i + 0.5) * charSweep
                ctx.save()
                ctx.translate(cx + mirrorMid * Math.cos(a),
                              cy + mirrorMid * Math.sin(a))
                // After this rotation: local +x = tangent CCW; local
                // +y = radial INWARD (toward disc centre).
                ctx.rotate(a + Math.PI / 2)

                for (let s = 0; s < numStrips; ++s) {
                    const yMid    = -halfH + (s + 0.5) * stripH
                    const worldR  = mirrorMid - yMid
                    const scaleX  = worldR / mirrorMid

                    ctx.save()
                    ctx.beginPath()
                    ctx.rect(-fontPx, yMid - stripH / 2,
                             fontPx * 2, stripH)
                    ctx.clip()
                    ctx.scale(scaleX, 1.0)
                    ctx.fillText(text.charAt(i), 0, 0)
                    ctx.restore()
                }
                ctx.restore()
            }
        }
    }

    // Wrap the matrix canvas as a texture source so the shader can
    // sample it. hideSource keeps Qt from compositing the canvas to the
    // scene a second time on top of the shader.
    ShaderEffectSource {
        id: matrixSource
        sourceItem: matrixCanvas
        anchors.fill: discSurface
        hideSource: true
        live: true
        visible: false
    }

    Canvas {
        id: canvas
        anchors.fill: discSurface
        antialiasing: true

        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()

            const cx = width / 2
            const cy = height / 2
            const R  = Math.min(width, height) / 2 - 2

            const hubR        = R * root._hubFraction
            const dataInner   = R * root._dataInner
            const dataOuter   = R * root._dataOuter

            const clearOuter  = R * root._clearOuter
            const mirrorOuter = R * root._mirrorOuter

            // Area-correct fraction → radius. A real CD lays data along
            // a single constant-linear-velocity spiral, so equal-duration
            // tracks occupy equal *areas*. That means the radial width of
            // a track band scales as ~1/r — outer tracks are thinner
            // bands, inner tracks are wider ones. Without this mapping
            // the boundary lines would be evenly spaced, which is the
            // wrong physics.
            const dataInner2 = dataInner * dataInner
            const dataOuter2 = dataOuter * dataOuter
            function fracToR(f) {
                return Math.sqrt(dataInner2 + f * (dataOuter2 - dataInner2))
            }

            // Matrix-band text now lives on its own canvas (matrixCanvas)
            // so it can do expensive per-character strip warping without
            // being redrawn on every sweep tick.

            // ============================================================
            // PER-TRACK VERIFY RINGS — colored annular arcs along each
            // track's band when verify lands a result.
            // ============================================================
            if (root.tracks && root.tracks.length > 0) {
                for (let i = 0; i < root.tracks.length; ++i) {
                    const t = root.tracks[i]
                    const s = t.startFraction
                    const e = t.endFraction
                    if (s === undefined || e === undefined) continue
                    const innerR = fracToR(s)
                    const outerR = fracToR(e)
                    const midR   = (innerR + outerR) / 2

                    let color = null
                    const st = t.status || ""
                    if (st === "ok")        color = root.verifyOkColor
                    else if (st === "warn") color = root.verifyWarnColor
                    else if (st === "fail") color = root.verifyFailColor
                    else if (st === "encoded" && root.mode === "encoding")
                        color = root.encodedColor

                    if (color) {
                        ctx.strokeStyle = color
                        ctx.lineWidth = Math.max(2, (outerR - innerR) * 0.35)
                        ctx.beginPath()
                        ctx.arc(cx, cy, midR, 0, Math.PI * 2)
                        ctx.stroke()
                    }
                }
            }

            // ============================================================
            // TRACK BOUNDARIES — thin concentric circles at each track
            // start radius, using the area-correct mapping. Half the
            // width and half the alpha of the previous version so the
            // disc surface reads as primary and the boundaries as a
            // subtle overlay.
            // ============================================================
            ctx.strokeStyle = root.trackLineColor
            ctx.lineWidth = 0.5
            if (root.tracks && root.tracks.length > 0) {
                for (let i = 0; i < root.tracks.length; ++i) {
                    const t = root.tracks[i]
                    const s = t.startFraction
                    if (s === undefined || s <= 0) continue
                    const r = fracToR(s)
                    ctx.beginPath()
                    ctx.arc(cx, cy, r, 0, Math.PI * 2)
                    ctx.stroke()
                }
            }

            // ============================================================
            // SWEEP — single rotating radial line, the "drive head".
            // Visible during any active phase; trails inside the current
            // read radius (where the head physically is) during read.
            // ============================================================
            if (root.mode === "reading" || root.mode === "identifying"
                || root.mode === "encoding" || root.mode === "verifying") {
                const sweepEnd = (root.mode === "reading"
                                  && root.readProgress > 0)
                    ? fracToR(Math.min(1.0, root.readProgress))
                    : dataOuter
                const ang = (root._sweepAngle - 90) * Math.PI / 180
                ctx.strokeStyle = (root.mode === "reading")
                    ? Qt.rgba(1.0, 1.0, 1.0, 0.55)
                    : Qt.rgba(1.0, 1.0, 1.0, 0.30)
                ctx.lineWidth = 1.5
                ctx.beginPath()
                ctx.moveTo(cx + dataInner * Math.cos(ang),
                           cy + dataInner * Math.sin(ang))
                ctx.lineTo(cx + sweepEnd * Math.cos(ang),
                           cy + sweepEnd * Math.sin(ang))
                ctx.stroke()
            }

            // ============================================================
            // HUB TRIM — thin ring around the spindle hole so it lines
            // up cleanly with the shader's hub cutout.
            // ============================================================
            ctx.strokeStyle = "rgba(255, 255, 255, 0.12)"
            ctx.lineWidth = 1
            ctx.beginPath()
            ctx.arc(cx, cy, hubR, 0, Math.PI * 2)
            ctx.stroke()
        }
    }
}
