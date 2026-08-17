pragma ComponentBehavior: Bound
// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Shapes
import "../components"

Item {
    id: root
    required property AppController controller
    readonly property real inTuneCents: 3.0
    readonly property bool inTune: root.controller.tunerDetected
        && Math.abs(root.controller.tunerCents) <= root.inTuneCents
    readonly property real boundedCents: Math.max(-50, Math.min(50, root.controller.tunerCents))

    Rectangle {
        id: header
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 58
        color: "transparent"
        Text {
            anchors.left: parent.left
            anchors.leftMargin: 22
            anchors.verticalCenter: parent.verticalCenter
            text: "Tuner"
            color: Theme.text
            font.family: Theme.fontFamily
            font.pixelSize: 18
            font.weight: Font.DemiBold
        }
        IconButton {
            anchors.right: parent.right
            anchors.rightMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            iconSource: Qt.resolvedUrl("../../assets/close.svg")
            Accessible.name: "Close tuner"
            onClicked: root.controller.closeTuner()
        }
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: 16
            anchors.rightMargin: 16
            height: 1
            color: Theme.borderSoft
        }
    }

    Column {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        anchors.margins: 22
        spacing: 12

        Row {
            width: parent.width
            height: 34
            spacing: 10
            Rectangle {
                width: (parent.width - 10) / 2
                height: parent.height
                radius: Theme.radiusSmall
                color: Theme.surfaceRaised
                border.color: Theme.border
                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.controller.profilePrimaryInstrument
                    color: Theme.text
                    font.family: Theme.fontFamily
                    font.pixelSize: 10
                }
            }
            Rectangle {
                width: (parent.width - 10) / 2
                height: parent.height
                radius: Theme.radiusSmall
                color: Theme.surfaceRaised
                border.color: Theme.border
                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Chromatic · A4 440 Hz"
                    color: Theme.text
                    font.family: Theme.fontFamily
                    font.pixelSize: 10
                }
            }
        }

        JamCard {
            width: parent.width
            height: 286
            color: "#091014"

            Item {
                id: gauge
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 16
                height: 170

                Shape {
                    anchors.fill: parent
                    antialiasing: true
                    ShapePath {
                        strokeColor: "#263038"
                        strokeWidth: 7
                        capStyle: ShapePath.RoundCap
                        fillColor: "transparent"
                        PathAngleArc {
                            centerX: gauge.width / 2
                            centerY: gauge.height - 4
                            radiusX: Math.min(168, gauge.width / 2 - 16)
                            radiusY: 132
                            startAngle: 205
                            sweepAngle: 130
                        }
                    }
                    ShapePath {
                        strokeColor: "#39d763"
                        strokeWidth: 7
                        capStyle: ShapePath.RoundCap
                        fillColor: "transparent"
                        PathAngleArc {
                            centerX: gauge.width / 2
                            centerY: gauge.height - 4
                            radiusX: Math.min(168, gauge.width / 2 - 16)
                            radiusY: 132
                            startAngle: 205
                            sweepAngle: 52
                        }
                    }
                    ShapePath {
                        strokeColor: "#e0cc39"
                        strokeWidth: 7
                        capStyle: ShapePath.FlatCap
                        fillColor: "transparent"
                        PathAngleArc {
                            centerX: gauge.width / 2
                            centerY: gauge.height - 4
                            radiusX: Math.min(168, gauge.width / 2 - 16)
                            radiusY: 132
                            startAngle: 257
                            sweepAngle: 34
                        }
                    }
                    ShapePath {
                        strokeColor: "#e9773d"
                        strokeWidth: 7
                        capStyle: ShapePath.RoundCap
                        fillColor: "transparent"
                        PathAngleArc {
                            centerX: gauge.width / 2
                            centerY: gauge.height - 4
                            radiusX: Math.min(168, gauge.width / 2 - 16)
                            radiusY: 132
                            startAngle: 291
                            sweepAngle: 44
                        }
                    }
                }

                Rectangle {
                    id: needle
                    visible: root.controller.tunerDetected
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 50
                    width: 3
                    height: 72
                    radius: 2
                    color: root.inTune ? Theme.connected : Theme.text
                    transformOrigin: Item.Bottom
                    rotation: root.boundedCents / 50 * 64
                    Behavior on rotation {
                        NumberAnimation { duration: 95; easing.type: Easing.OutCubic }
                    }
                    Rectangle {
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.bottom: parent.bottom
                        width: 11
                        height: 11
                        radius: 6
                        color: parent.color
                    }
                }

                Text {
                    anchors.left: parent.left
                    anchors.bottom: parent.bottom
                    text: "-50"
                    color: Theme.textMuted
                    font.family: Theme.numericFontFamily
                    font.pixelSize: 9
                }
                Text {
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    text: "+50"
                    color: Theme.textMuted
                    font.family: Theme.numericFontFamily
                    font.pixelSize: 9
                }
            }

            Column {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top
                anchors.topMargin: 130
                spacing: 2
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: root.controller.tunerDetected
                        ? root.controller.tunerNote : "–"
                    color: Theme.text
                    font.family: Theme.fontFamily
                    font.pixelSize: 74
                    font.weight: Font.Light
                    Behavior on color { ColorAnimation { duration: 120 } }
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: root.controller.tunerDetected
                        ? (root.controller.tunerCents >= 0 ? "+" : "")
                            + root.controller.tunerCents.toFixed(1) + " cents"
                        : "Play one clear note"
                    color: root.inTune ? Theme.connected : Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: 13
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: root.controller.tunerDetected
                        ? root.controller.tunerFrequency.toFixed(2) + " Hz" : ""
                    color: Theme.textMuted
                    font.family: Theme.numericFontFamily
                    font.pixelSize: 9
                }
            }
        }

        Row {
            visible: !root.controller.roomActive
                && root.controller.profilePrimaryInstrument.toLowerCase().includes("guitar")
            width: parent.width
            height: visible ? 32 : 0
            Repeater {
                model: ["E", "A", "D", "G", "B", "E"]
                Text {
                    required property string modelData
                    width: parent.width / 6
                    text: modelData
                    color: modelData === root.controller.tunerNote
                        ? Theme.connected : Theme.textSecondary
                    horizontalAlignment: Text.AlignHCenter
                    font.family: Theme.fontFamily
                    font.pixelSize: 15
                    font.weight: Font.Medium
                }
            }
        }

        JamCard {
            visible: root.controller.roomActive
            width: parent.width
            height: visible ? 66 : 0
            Row {
                anchors.fill: parent
                anchors.margins: 13
                spacing: 10
                JamIcon {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 18
                    height: 18
                    source: Qt.resolvedUrl("../../assets/music_note.svg")
                    color: root.controller.tunerMutesInstrument
                        ? Theme.warning : Theme.textMuted
                }
                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - 28 - muteSwitch.width - 14
                    spacing: 2
                    Text {
                        text: root.controller.tunerMutesInstrument
                            ? "Instrument muted to the room" : "Instrument is live to the room"
                        color: root.controller.tunerMutesInstrument
                            ? Theme.warning : Theme.connected
                        font.family: Theme.fontFamily
                        font.pixelSize: 11
                        font.weight: Font.Medium
                    }
                    Text {
                        text: "Your microphone stays connected so you can keep talking."
                        color: Theme.textMuted
                        elide: Text.ElideRight
                        width: parent.width
                        font.family: Theme.fontFamily
                        font.pixelSize: 9
                    }
                }
                JamSwitch {
                    id: muteSwitch
                    anchors.verticalCenter: parent.verticalCenter
                    checked: root.controller.tunerMutesInstrument
                    onToggled: root.controller.tunerMutesInstrument = checked
                }
            }
        }

        Text {
            width: parent.width
            text: root.controller.roomActive
                ? "Close Tuner to return to your live room."
                : "Close Tuner to return Home."
            color: Theme.textMuted
            horizontalAlignment: Text.AlignHCenter
            font.family: Theme.fontFamily
            font.pixelSize: 9
        }
    }
}
