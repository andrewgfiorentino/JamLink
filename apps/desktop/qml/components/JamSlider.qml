// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls.Basic

Slider {
    id: control
    from: 0
    to: 1
    implicitHeight: 22

    background: Rectangle {
        x: control.leftPadding
        y: control.topPadding + control.availableHeight / 2 - height / 2
        width: control.availableWidth
        height: 3
        radius: 2
        color: "#293239"
        Rectangle {
            width: control.visualPosition * parent.width
            height: parent.height
            radius: parent.radius
            color: Theme.accentBright
        }
    }

    handle: Rectangle {
        x: control.leftPadding + control.visualPosition * (control.availableWidth - width)
        y: control.topPadding + control.availableHeight / 2 - height / 2
        width: 12
        height: 12
        radius: 6
        color: control.pressed ? "#dce8ee" : "#f4f7f8"
        border.color: "#d2d9dc"
    }
}
