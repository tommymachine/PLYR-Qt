// Custom slider matching the Swift PLYR design:
//   • tapered wedge-shaped track (short on left, tall on right)
//   • ocean → sky blue gradient on the active portion
//   • grayed-out version of the same gradient on the inactive portion
//   • no thumb element
//   • seek-on-release via editingStarted / editingEnded signals,
//     or set commitOnDrag: true for continuous updates (volume etc.)

import QtQuick

Item {
    id: root

    property real value: 0
    property real from:  0
    property real to:    1

    // If true, fires `moved` on every drag sample (good for volume).
    // If false, fires `editingStarted` on mouse-down and `editingEnded`
    // on mouse-up (good for scrub-then-commit seek).
    property bool commitOnDrag: false

    signal moved(real newValue)
    signal editingStarted()
    signal editingEnded()

    // Colors — match the Metal shader / Canvas slider in Swift PLYR.
    readonly property color ocean:     Qt.rgba(0.02, 0.18, 0.45, 1.0)
    readonly property color sky:       Qt.rgba(0.45, 0.78, 1.00, 1.0)
    // Precomputed `grayedOut` (CIE-Lab perceptual-lightness equivalents
    // of the two blues). ocean ≈ 0.22 gray, sky ≈ 0.74 gray.
    readonly property color oceanGray: Qt.rgba(0.22, 0.22, 0.22, 1.0)
    readonly property color skyGray:   Qt.rgba(0.74, 0.74, 0.74, 1.0)

    // Track-taper exponent — same shape as the Swift version.
    readonly property real taperK: 1.1

    Canvas {
        id: canvas
        anchors.fill: parent

        Connections {
            target: root
            function onValueChanged() { canvas.requestPaint() }
            function onFromChanged()  { canvas.requestPaint() }
            function onToChanged()    { canvas.requestPaint() }
        }
        onWidthChanged:  requestPaint()
        onHeightChanged: requestPaint()

        onPaint: {
            const ctx = getContext("2d")
            const W = width, H = height
            if (W <= 0 || H <= 0) return

            ctx.clearRect(0, 0, W, H)

            const span = root.to - root.from
            const t    = span > 0 ? Math.max(0, Math.min(1, (root.value - root.from) / span)) : 0
            const splitX = t * W

            const baseSize = H * 0.55
            const k        = root.taperK
            const centerY  = H / 2

            // height(x) = baseSize * exp(k * (x/W - 0.5))
            function heightAtX(x) {
                return baseSize * Math.exp(k * (x / W - 0.5))
            }

            // Build the tapered-track path.
            const samples = 80
            ctx.beginPath()
            ctx.moveTo(0, centerY - heightAtX(0) / 2)
            for (let i = 1; i <= samples; ++i) {
                const x = i / samples * W
                ctx.lineTo(x, centerY - heightAtX(x) / 2)
            }
            for (let i = samples; i >= 0; --i) {
                const x = i / samples * W
                ctx.lineTo(x, centerY + heightAtX(x) / 2)
            }
            ctx.closePath()

            // Active portion → blue gradient.
            ctx.save()
            ctx.beginPath()
            ctx.rect(0, 0, splitX, H)
            ctx.clip()
            const blueGrad = ctx.createLinearGradient(0, centerY, W, centerY)
            blueGrad.addColorStop(0.0, root.ocean)
            blueGrad.addColorStop(1.0, root.sky)
            ctx.fillStyle = blueGrad
            // re-add the track path under the clip
            ctx.beginPath()
            ctx.moveTo(0, centerY - heightAtX(0) / 2)
            for (let i = 1; i <= samples; ++i) {
                const x = i / samples * W
                ctx.lineTo(x, centerY - heightAtX(x) / 2)
            }
            for (let i = samples; i >= 0; --i) {
                const x = i / samples * W
                ctx.lineTo(x, centerY + heightAtX(x) / 2)
            }
            ctx.closePath()
            ctx.fill()
            ctx.restore()

            // Inactive portion → grayed-out gradient (same luminance).
            ctx.save()
            ctx.beginPath()
            ctx.rect(splitX, 0, W - splitX, H)
            ctx.clip()
            const grayGrad = ctx.createLinearGradient(0, centerY, W, centerY)
            grayGrad.addColorStop(0.0, root.oceanGray)
            grayGrad.addColorStop(1.0, root.skyGray)
            ctx.fillStyle = grayGrad
            ctx.beginPath()
            ctx.moveTo(0, centerY - heightAtX(0) / 2)
            for (let i = 1; i <= samples; ++i) {
                const x = i / samples * W
                ctx.lineTo(x, centerY - heightAtX(x) / 2)
            }
            for (let i = samples; i >= 0; --i) {
                const x = i / samples * W
                ctx.lineTo(x, centerY + heightAtX(x) / 2)
            }
            ctx.closePath()
            ctx.fill()
            ctx.restore()
        }
    }

    // -------- Interaction ---------------------------------------------
    MouseArea {
        id: ma
        anchors.fill: parent
        property bool dragging: false

        function valueFor(x) {
            const clamped = Math.max(0, Math.min(root.width, x))
            const t = root.width > 0 ? clamped / root.width : 0
            return root.from + t * (root.to - root.from)
        }

        onPressed: (mouse) => {
            dragging = true
            root.editingStarted()
            const v = valueFor(mouse.x)
            if (root.commitOnDrag) {
                root.value = v
                root.moved(v)
            } else {
                root.value = v   // visual preview only
            }
        }
        onPositionChanged: (mouse) => {
            if (!dragging) return
            const v = valueFor(mouse.x)
            root.value = v
            if (root.commitOnDrag) root.moved(v)
        }
        onReleased: {
            dragging = false
            if (!root.commitOnDrag) root.moved(root.value)
            root.editingEnded()
        }
    }
}
