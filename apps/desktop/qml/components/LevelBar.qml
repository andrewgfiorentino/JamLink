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
                    const active = segment.index
                        < Math.round(root.displayedLevel * root.segmentCount)
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
        x: Math.max(0, Math.min(root.width - width, root.peakHold * root.width - width / 2))
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
        border.color: "#ff5148"
    }
}
