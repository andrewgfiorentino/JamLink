// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls.Basic

Button {
    id: control
    property url iconSource
    property color iconColor: control.enabled ? Theme.text : Theme.textMuted
    implicitWidth: 34
    implicitHeight: 34
    hoverEnabled: true

    background: Rectangle {
        radius: Theme.radiusControl
        color: control.down ? "#202a31" : control.hovered ? Theme.hover : "transparent"
        border.color: control.activeFocus ? Theme.accentBright : "transparent"
    }

    contentItem: JamIcon {
        anchors.centerIn: parent
        width: 18
        height: 18
        source: control.iconSource
        color: control.iconColor
    }
}
