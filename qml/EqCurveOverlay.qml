// EqCurveOverlay — smooth response-curve overlay for the EQ panel.
//
// Renders a bezier-smoothed curve through the 10 band gain values, spanning
// the full width of the slider row. Cardinal-spline tangents give a natural
// look. Mouse events pass through to the sliders underneath.

import QtQuick

Canvas {
    id: root

    property var  bandGains: []       // array of N doubles, ±12 dB
    property bool active:    true
    property int  bandCount: 10

    antialiasing: true

    Connections {
        target: root
        function onBandGainsChanged() { root.requestPaint() }
        function onActiveChanged()    { root.requestPaint() }
    }
    onWidthChanged:  requestPaint()
    onHeightChanged: requestPaint()

    onPaint: {
        const ctx = getContext("2d")
        const W = width, H = height
        if (W <= 0 || H <= 0) return
        ctx.clearRect(0, 0, W, H)
        if (!bandGains || bandGains.length < 2) return

        const N = root.bandCount

        // Slider centers sit at evenly-spaced divisions of W.
        function xFor(i) { return (i + 0.5) / N * W }
        function yFor(db) { return H * (1 - (db + 12) / 24) }

        // Dashed zero line — reference for eye.
        ctx.setLineDash([2, 4])
        ctx.strokeStyle = Qt.rgba(1, 1, 1, 0.12)
        ctx.lineWidth = 1
        ctx.beginPath()
        ctx.moveTo(0, yFor(0))
        ctx.lineTo(W, yFor(0))
        ctx.stroke()
        ctx.setLineDash([])

        // Cardinal-spline through the N band points. Handles come from the
        // slope between neighbors (tension 0.5).
        ctx.lineWidth = 2
        ctx.strokeStyle = root.active
            ? Qt.rgba(1, 1, 1, 0.80)
            : Qt.rgba(1, 1, 1, 0.28)
        ctx.lineCap = "round"
        ctx.lineJoin = "round"

        ctx.beginPath()
        ctx.moveTo(xFor(0), yFor(bandGains[0]))
        for (let i = 0; i < N - 1; i++) {
            const xi   = xFor(i)
            const yi   = yFor(bandGains[i])
            const xj   = xFor(i + 1)
            const yj   = yFor(bandGains[i + 1])

            const iPrev = Math.max(0, i - 1)
            const iNext = Math.min(N - 1, i + 2)

            const c1x = xi + (xj - xFor(iPrev)) / 6
            const c1y = yi + (yj - yFor(bandGains[iPrev])) / 6
            const c2x = xj - (xFor(iNext) - xi) / 6
            const c2y = yj - (yFor(bandGains[iNext]) - yi) / 6
            ctx.bezierCurveTo(c1x, c1y, c2x, c2y, xj, yj)
        }
        ctx.stroke()
    }
}
