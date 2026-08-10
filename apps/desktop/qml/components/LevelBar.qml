pragma ComponentBehavior: Bound
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick

Item {
    id: root
    property real level: 0
    property int segmentCount: 46
    implicitHeight: 18

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
                    const active = segment.index < Math.round(root.level * root.segmentCount)
                    if (!active)
                        return "#20292e"
                    if (segment.index > root.segmentCount * 0.82)
                        return "#d2cd31"
                    return segment.index > root.segmentCount * 0.62 ? "#8bd334" : "#31d052"
                }
                opacity: segment.index % 3 === 0 ? 1.0 : 0.88
            }
        }
    }
}
