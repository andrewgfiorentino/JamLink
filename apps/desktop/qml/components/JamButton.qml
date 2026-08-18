// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls.Basic

Button {
    id: control
    property bool primary: false
    property url iconSource
    property Gradient primaryGradient: Gradient {
        orientation: Gradient.Horizontal
        GradientStop { position: 0.0; color: "#5126a2" }
        GradientStop { position: 1.0; color: Theme.accentBright }
    }
    hoverEnabled: true
    implicitHeight: 38
    font.family: Theme.fontFamily
    font.pixelSize: 13
    font.weight: Font.Medium

    background: Rectangle {
        radius: Theme.radiusControl
        border.width: control.primary ? 0 : 1
        border.color: control.activeFocus ? Theme.accentBright : Theme.border
        gradient: control.primary && control.enabled ? control.primaryGradient : null
        color: control.primary
            ? (control.down ? "#5729a8" : control.hovered ? "#7d40d0" : "#6a35bd")
            : (control.down ? "#1b252b" : control.hovered ? Theme.hover : Theme.surfaceNested)
    }

    contentItem: Row {
        spacing: 8
        anchors.centerIn: parent
        JamIcon {
            id: buttonIcon
            visible: control.iconSource.toString().length > 0
            width: visible ? 17 : 0
            height: 17
            source: control.iconSource
            color: control.enabled ? "#f4f1fa" : "#6c747a"
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            // Bounded to the button. A label that does not fit is trimmed
            // rather than painted across whatever sits beside it, which is
            // what happens when the text is a device name somebody else chose.
            // Where there is room this is exactly implicitWidth, so nothing
            // that fits today renders differently.
            width: Math.min(
                implicitWidth,
                Math.max(0, control.width - 16 - (buttonIcon.visible ? 25 : 0)))
            elide: Text.ElideRight
            text: control.text
            color: control.enabled ? "#f5f3f8" : "#737a80"
            font: control.font
        }
    }
}
