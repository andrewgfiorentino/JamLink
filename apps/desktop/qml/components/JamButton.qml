// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls.Basic

Button {
    id: control
    property bool primary: false
    property url iconSource
    property Gradient primaryGradient: Gradient {
        orientation: Gradient.Horizontal
        GradientStop { position: 0.0; color: "#4d269e" }
        GradientStop { position: 1.0; color: "#8342ca" }
    }
    hoverEnabled: true
    implicitHeight: 38
    font.family: "Segoe UI Variable Text"
    font.pixelSize: 13
    font.weight: Font.Medium

    background: Rectangle {
        radius: 7
        border.width: control.primary ? 0 : 1
        border.color: control.activeFocus ? "#3bc9ff" : "#313b42"
        gradient: control.primary && control.enabled ? control.primaryGradient : null
        color: control.primary
            ? (control.down ? "#5729a8" : control.hovered ? "#7d40d0" : "#6a35bd")
            : (control.down ? "#1b252b" : control.hovered ? "#171f25" : "#11171b")
    }

    contentItem: Row {
        spacing: 8
        anchors.centerIn: parent
        JamIcon {
            visible: control.iconSource.toString().length > 0
            width: visible ? 17 : 0
            height: 17
            source: control.iconSource
            color: control.enabled ? "#f4f1fa" : "#6c747a"
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: control.text
            color: control.enabled ? "#f5f3f8" : "#737a80"
            font: control.font
        }
    }
}
