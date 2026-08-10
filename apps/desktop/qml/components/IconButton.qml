// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls.Basic

Button {
    id: control
    property url iconSource
    property color iconColor: control.enabled ? "#e7ebee" : "#667079"
    implicitWidth: 34
    implicitHeight: 34
    hoverEnabled: true

    background: Rectangle {
        radius: 8
        color: control.down ? "#202a31" : control.hovered ? "#172027" : "transparent"
        border.color: control.activeFocus ? "#8b56df" : "transparent"
    }

    contentItem: JamIcon {
        anchors.centerIn: parent
        width: 18
        height: 18
        source: control.iconSource
        color: control.iconColor
    }
}
