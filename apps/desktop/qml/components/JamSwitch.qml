// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls.Basic

Switch {
    id: control
    implicitWidth: 34
    implicitHeight: 20

    indicator: Rectangle {
        implicitWidth: 34
        implicitHeight: 18
        x: control.leftPadding
        y: parent.height / 2 - height / 2
        radius: 9
        color: control.checked ? "#7d40d0" : "#303a41"
        border.color: control.activeFocus ? "#c7a9f2" : "transparent"
        Rectangle {
            x: control.checked ? parent.width - width - 3 : 3
            y: 3
            width: 12
            height: 12
            radius: 6
            color: "#f7f8f8"
            Behavior on x { NumberAnimation { duration: 90 } }
        }
    }

    contentItem: Item {}
}
