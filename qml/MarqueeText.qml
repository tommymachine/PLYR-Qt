// Single-line label that scrolls horizontally, wrap-around style, when
// the text is too long for its container. Matches the Swift PLYR
// behaviour: 4-second pause at the start, then continuous leftward
// scroll at a fixed pts/sec, with a trailing duplicate of the text so
// the reset is seamless.

import QtQuick

Item {
    id: root
    property string text
    property font font
    property color color: "white"
    property real speed:        30     // pts/sec
    property real gap:          60     // pts between tail and head
    property real initialPause: 4000   // ms

    // Two-instance sync: set `followExternal: true` and bind `externalX`
    // to another MarqueeText's `currentX`. This instance then stops its
    // own animation and mirrors the source's scroll offset pixel-for-pixel,
    // so a card + overlay pair reads as a single continuous marquee.
    property bool followExternal: false
    property real externalX:      0
    readonly property real currentX: row.x

    clip: true

    // Hidden reference label — lets us measure the natural (unclipped)
    // text width without contributing to the visible layout.
    Text {
        id: measurer
        text: root.text
        font: root.font
        visible: false
    }

    readonly property real textWidth: measurer.implicitWidth
    // Small tolerance: don't scroll just because the text overflows by
    // a subpixel. Anything under ~2 px slack is not perceptible.
    readonly property bool needsScroll: textWidth > width + 2

    Row {
        id: row
        spacing: root.gap
        y: (root.height - label1.implicitHeight) / 2
        x: 0

        Text {
            id: label1
            text: root.text
            font: root.font
            color: root.color
        }
        Text {
            id: label2
            text: root.text
            font: root.font
            color: root.color
            visible: root.needsScroll
        }
    }

    // One-shot + repeating animation:
    //   pause → scroll to -(textWidth + gap) → snap to 0 → repeat
    // Driven imperatively (via _refresh below) rather than a `running:`
    // binding, so anim.restart() and the "should we be scrolling?"
    // decision always stay in lockstep.
    SequentialAnimation {
        id: anim
        loops: Animation.Infinite

        PauseAnimation { duration: root.initialPause }
        NumberAnimation {
            target: row
            property: "x"
            from: 0
            to:   -(root.textWidth + root.gap)
            duration: Math.max(1,
                        1000 * (root.textWidth + root.gap) / root.speed)
            easing.type: Easing.Linear
        }
        PropertyAction  { target: row; property: "x"; value: 0 }
    }

    // When in follow mode, replace the animation's direct writes with a
    // binding to the external offset. The Binding is only `when` true so
    // the primary instance's animation-driven x isn't clobbered.
    Binding {
        target: row
        property: "x"
        value: root.externalX
        when: root.followExternal
    }

    // Single decision point: given the current state, should we be
    // scrolling? Stop + reset whenever the answer is "no"; restart
    // whenever it's "yes" (so a new track always re-plays the pause).
    function _refresh() {
        if (followExternal) {
            anim.stop()
            return
        }
        if (needsScroll && visible) {
            anim.restart()
        } else {
            anim.stop()
            row.x = 0
        }
    }

    onTextChanged:           _refresh()
    onNeedsScrollChanged:    _refresh()
    onVisibleChanged:        _refresh()
    onFollowExternalChanged: _refresh()
    Component.onCompleted:   _refresh()
}
