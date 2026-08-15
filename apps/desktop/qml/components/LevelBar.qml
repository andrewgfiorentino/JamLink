pragma ComponentBehavior: Bound
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick

Item {
    id: root
    property real level: 0
    property real peakHold: 0
    property bool clipped: false
    property int segmentCount: 46
    property real displayedLevel: level
    // Set by meters that own a latch. The meter is the thing a musician is
    // already looking at while reaching for the interface gain knob, so it
    // doubles as the reset target rather than making them find a small button.
    property bool resettable: false
    signal resetRequested()
    readonly property bool canReset: root.resettable && root.clipped
    implicitHeight: 18

    onLevelChanged: displayedLevel = level
    Behavior on displayedLevel {
        NumberAnimation { duration: 90; easing.type: Easing.OutCubic }
    }

    Row {
        anchors.fill: parent
        spacing: 1
        Repeater {
            model: root.segmentCount
            Rectangle {
                id: segment
                required property int index
                width: Math.max(2, (root.width - (root.segmentCount - 1)) / root.segmentCount)
                height: parent.height
                radius: 1
                color: {
                    // Reserve the final red segment for the latched detector.
                    // Rounding previously made a -0.095 dBFS signal look full
                    // even though it had not reached the clip threshold.
                    const normalSegments = Math.min(
                        root.segmentCount - 1,
                        Math.floor(Math.max(0, Math.min(1, root.displayedLevel))
                            * root.segmentCount))
                    const activeSegments = root.clipped
                        ? root.segmentCount : normalSegments
                    const active = segment.index < activeSegments
                    if (!active)
                        return Theme.borderSoft
                    if (segment.index > root.segmentCount * 0.92)
                        return Theme.error
                    if (segment.index > root.segmentCount * 0.72)
                        return Theme.warning
                    return segment.index > root.segmentCount * 0.56 ? "#94d43d" : Theme.connected
                }
                opacity: segment.index % 3 === 0 ? 1.0 : 0.88
            }
        }
    }

    Rectangle {
        visible: root.peakHold > 0.00001
        readonly property real shownPeak: root.clipped
            ? 1 : Math.min(root.peakHold, (root.segmentCount - 1) / root.segmentCount)
        x: Math.max(0, Math.min(root.width - width, shownPeak * root.width - width / 2))
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 2
        radius: 1
        color: root.clipped ? "#ff5a50" : "#f2f4f5"
    }

    Rectangle {
        anchors.fill: parent
        color: "transparent"
        radius: 2
        border.width: root.clipped ? 1 : 0
        border.color: resetArea.containsMouse && root.canReset ? "#ffffff" : "#ff5148"
    }

    MouseArea {
        id: resetArea
        anchors.fill: parent
        // Only active while there is a latch to clear, so a clean meter never
        // swallows clicks meant for whatever sits behind it.
        enabled: root.canReset
        visible: root.canReset
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.resetRequested()
    }

    Text {
        anchors.centerIn: parent
        visible: resetArea.containsMouse && root.canReset
        text: "Reset"
        color: "#ffffff"
        font.family: "Segoe UI"
        font.pixelSize: 9
        font.weight: Font.DemiBold
    }

    Accessible.role: Accessible.Indicator
    Accessible.name: root.clipped ? "Level meter, clipping latched" : "Level meter"
    Accessible.description: root.canReset
        ? "Clipping is latched. Activate the meter to reset it after lowering the input gain."
        : "Shows the current signal level and peak hold."
    Accessible.focusable: root.canReset
    Accessible.onPressAction: if (root.canReset) root.resetRequested()
}
