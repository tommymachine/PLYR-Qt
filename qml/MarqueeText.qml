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
    readonly property bool needsScroll: textWidth > width + 0.5

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
    SequentialAnimation on row.x {
        id: anim
        running: root.needsScroll && root.visible
        loops: Animation.Infinite

        PauseAnimation { duration: root.initialPause }
        NumberAnimation {
            from: 0
            to:   -(root.textWidth + root.gap)
            duration: Math.max(1,
                        1000 * (root.textWidth + root.gap) / root.speed)
            easing.type: Easing.Linear
        }
        PropertyAction  { value: 0 }
    }

    onTextChanged: anim.restart()
    onNeedsScrollChanged: if (!needsScroll) row.x = 0
}
