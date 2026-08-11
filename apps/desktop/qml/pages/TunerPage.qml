pragma ComponentBehavior: Bound
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import "../components"

Item {
    id: root
    required property AppController controller

    // Anything inside this window reads as in tune. Wider than a cent so the
    // needle settles instead of hunting on a decaying string.
    readonly property real inTuneCents: 3.0
    readonly property bool inTune: root.controller.tunerDetected
        && Math.abs(root.controller.tunerCents) <= root.inTuneCents

    Rectangle {
        id: header
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 58
        color: "transparent"

        IconButton {
            anchors.left: parent.left
            anchors.leftMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            iconSource: Qt.resolvedUrl("../../assets/arrow_back.svg")
            Accessible.name: "Back"
            onClicked: root.controller.navigate("home")
        }
        Column {
            anchors.centerIn: parent
            spacing: 2
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Tuner"
                color: "#f2f4f5"
                font.family: "Segoe UI"
                font.pixelSize: 17
                font.weight: Font.DemiBold
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: root.controller.roomActive && root.controller.tunerMutesInstrument
                    ? "Your friend cannot hear you tuning"
                    : "Chromatic · A4 = 440 Hz"
                color: "#929ba1"
                font.family: "Segoe UI"
                font.pixelSize: 10
            }
        }
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: 16
            anchors.rightMargin: 16
            height: 1
            color: "#1d272d"
        }
    }

    Column {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: header.bottom
        anchors.margins: 22
        spacing: 12

        JamCard {
            width: parent.width
            height: 200

            Column {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 12

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: root.controller.tunerNote
                        + (root.controller.tunerDetected ? root.controller.tunerOctave : "")
                    color: root.controller.tunerDetected
                        ? (root.inTune ? "#38d65d" : "#f2f4f5")
                        : "#4f5960"
                    font.family: "Segoe UI"
                    font.pixelSize: 76
                    font.weight: Font.Light
                    Behavior on color { ColorAnimation { duration: 120 } }
                }

                // Cents scale. The needle is the only thing that moves, so the
                // eye tracks one object rather than a jumping number.
                Item {
                    width: parent.width
                    height: 42

                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width
                        height: 3
                        radius: 2
                        color: "#20292e"
                    }
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: 2 * parent.width * (root.inTuneCents / 50.0)
                        height: 3
                        radius: 2
                        color: "#1f5c33"
                    }
                    Repeater {
                        model: [-50, -25, 0, 25, 50]
                        Rectangle {
                            id: tick
                            required property int modelData
                            x: parent.width / 2 + (tick.modelData / 50.0) * (parent.width / 2) - width / 2
                            anchors.verticalCenter: parent.verticalCenter
                            width: tick.modelData === 0 ? 2 : 1
                            height: tick.modelData === 0 ? 20 : 12
                            color: tick.modelData === 0 ? "#5f6b72" : "#333d44"
                        }
                    }
                    Rectangle {
                        id: needle
                        visible: root.controller.tunerDetected
                        width: 4
                        height: 34
                        radius: 2
                        anchors.verticalCenter: parent.verticalCenter
                        x: parent.width / 2
                            + Math.max(-50, Math.min(50, root.controller.tunerCents)) / 50.0
                                * (parent.width / 2) - width / 2
                        color: root.inTune ? "#38d65d" : "#8b56df"
                        Behavior on x { NumberAnimation { duration: 90; easing.type: Easing.OutQuad } }
                        Behavior on color { ColorAnimation { duration: 120 } }
                    }
                }

                Row {
                    width: parent.width
                    Text {
                        width: parent.width / 2
                        text: root.controller.tunerDetected
                            ? (root.controller.tunerCents >= 0 ? "+" : "")
                                + root.controller.tunerCents.toFixed(1) + " cents"
                            : "Play a single string"
                        color: root.inTune ? "#38d65d" : "#a4acb2"
                        font.family: "Segoe UI"
                        font.pixelSize: 11
                    }
                    Text {
                        width: parent.width / 2
                        horizontalAlignment: Text.AlignRight
                        text: root.controller.tunerDetected
                            ? root.controller.tunerFrequency.toFixed(2) + " Hz"
                            : ""
                        color: "#78838a"
                        font.family: "Cascadia Mono"
                        font.pixelSize: 11
                    }
                }
            }
        }

        JamCard {
            width: parent.width
            height: 62
            visible: root.controller.roomActive

            Row {
                anchors.fill: parent
                anchors.margins: 14
                spacing: 10
                JamIcon {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 17
                    height: 17
                    source: Qt.resolvedUrl("../../assets/music_note.svg")
                    color: root.controller.tunerMutesInstrument ? "#8b56df" : "#5d666c"
                }
                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - 27 - muteSwitch.width - 20
                    spacing: 2
                    Text {
                        text: "Mute my guitar while tuning"
                        color: "#dfe3e5"
                        font.family: "Segoe UI"
                        font.pixelSize: 11
                    }
                    Text {
                        text: "Your microphone keeps working, so you can still talk."
                        color: "#78838a"
                        width: parent.width
                        elide: Text.ElideRight
                        font.family: "Segoe UI"
                        font.pixelSize: 9
                    }
                }
                JamSwitch {
                    id: muteSwitch
                    anchors.verticalCenter: parent.verticalCenter
                    checked: root.controller.tunerMutesInstrument
                    Accessible.name: "Mute my guitar to the room while tuning"
                    onToggled: root.controller.tunerMutesInstrument = checked
                }
            }
        }
    }
}
