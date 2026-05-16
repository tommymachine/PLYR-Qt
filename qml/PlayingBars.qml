// Tiny equalizer-style "now playing" indicator for playlist rows.
//
// Four vertical bars, four stacked segments each, mirroring the main
// visualizer's stylistic cues:
//   * trapezoidal taper — each segment shrinks slightly toward the
//     top of its bar
//   * inter-segment gaps so each bar reads as stacked tiles
//   * ocean→sky color ramp across the bar's height
//
// Bands sampled at indices 0, 4, 8, 12 of the 16-band FFT (b0.x ..
// b3.x): the four bars span sub-bass → low-mid → high-mid → air.
//
// Driven by a polling Timer. The FFT's `updated` NOTIFY signal does
// not always propagate through a deep `fft.b0.x` binding chain, so
// we poll at 60 Hz and write to plain real properties that the
// segments bind to directly.
//
// The lowest segment of each bar is always lit when audio is playing
// (acts as a visual baseline / liveness indicator). Higher segments
// light up as the band's magnitude crosses the segment's threshold.
//
// Usage:
//   PlayingBars { anchors.centerIn: parent; fft: fft; height: 14 }

import QtQuick

Item {
    id: root

    property var fft: null
    property color oceanColor: Qt.rgba(0.02, 0.18, 0.45, 1.0)
    property color skyColor:   Qt.rgba(0.45, 0.78, 1.00, 1.0)

    readonly property int barCount: 4
    readonly property int segCount: 8
    property real barWidth: 3
    property real barGap:   1.6
    property real segGap:   0.5
    property real taperK:   0.45

    // Perceptual boost: applied to each band as level^(1/boost) before
    // segment thresholding. The 16-band FFT pipeline targets the main
    // visualizer's geometry (110 segments per bar), so at 8 segments
    // here many bands sit below 0.5 even on lively music. Raising
    // boost compresses the curve toward 1.0 without saturating.
    //   boost = 1.0 → no change
    //   boost = 1.6 → moderate lift (level^0.625)
    //   boost = 2.0 → strong lift  (level^0.5)
    property real boost: 1.6

    implicitWidth:  barCount * barWidth + (barCount - 1) * barGap
    implicitHeight: 14

    // Cached band magnitudes. Bound to fft.b0.x .. fft.b3.x via the
    // Timer below so the segment bindings see plain real values
    // (avoids nested-property NOTIFY pitfalls).
    property real _b0: 0
    property real _b1: 0
    property real _b2: 0
    property real _b3: 0

    function _refresh() {
        if (!root.fft) return
        root._b0 = root.fft.b0.x
        root._b1 = root.fft.b1.x
        root._b2 = root.fft.b2.x
        root._b3 = root.fft.b3.x
    }

    Timer {
        interval: 16
        running: root.visible
        repeat: true
        onTriggered: root._refresh()
    }

    Connections {
        target: root.fft
        enabled: root.fft !== null
        function onUpdated() { root._refresh() }
    }

    Component.onCompleted: _refresh()

    Row {
        anchors.centerIn: parent
        spacing: root.barGap

        Repeater {
            model: root.barCount

            delegate: Item {
                id: barItem
                required property int index

                width: root.barWidth
                height: root.height

                readonly property real _rawLevel:
                                            index === 0 ? root._b0
                                          : index === 1 ? root._b1
                                          : index === 2 ? root._b2
                                          : root._b3
                readonly property real level:
                    Math.min(1.0, Math.pow(Math.max(0.0, _rawLevel),
                                           1.0 / root.boost))

                Repeater {
                    model: root.segCount

                    delegate: Rectangle {
                        required property int index

                        // Segment 0 is the baseline (always shown while
                        // visible). Higher segments light up as the
                        // band magnitude reaches their midpoint.
                        readonly property real _segMid:
                            (index + 0.5) / root.segCount
                        readonly property bool _lit:
                            index === 0 || _segMid <= barItem.level

                        readonly property real _segH:
                            (root.height - (root.segCount - 1) * root.segGap)
                            / root.segCount

                        // Trapezoidal taper: yNorm = 0 at top of bar,
                        // 1 at bottom. Each segment gets an average
                        // width between its top and bottom edge widths.
                        readonly property real _yTop:
                            1.0 - (index + 1) / root.segCount
                        readonly property real _yBot:
                            1.0 - index / root.segCount
                        readonly property real _topW:
                            root.barWidth * Math.exp(-root.taperK * (1.0 - _yTop))
                        readonly property real _botW:
                            root.barWidth * Math.exp(-root.taperK * (1.0 - _yBot))
                        readonly property real _avgW:
                            (_topW + _botW) / 2

                        readonly property real _t:
                            root.segCount === 1
                                ? 0 : index / (root.segCount - 1)

                        width:  _avgW
                        height: _segH
                        x: (root.barWidth - _avgW) / 2
                        y: parent.height - (index + 1) * _segH - index * root.segGap
                        radius: 0.6
                        color: Qt.rgba(
                            root.oceanColor.r + (root.skyColor.r - root.oceanColor.r) * _t,
                            root.oceanColor.g + (root.skyColor.g - root.oceanColor.g) * _t,
                            root.oceanColor.b + (root.skyColor.b - root.oceanColor.b) * _t,
                            1.0
                        )
                        opacity: _lit ? 1.0 : 0
                        Behavior on opacity {
                            NumberAnimation { duration: 70 }
                        }
                    }
                }
            }
        }
    }
}
