pragma ComponentBehavior: Bound
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import "../components"

Item {
    id: root
    required property AppController controller

    Rectangle {
        id: header
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 58
        color: "transparent"

        Row {
            anchors.left: parent.left
            anchors.leftMargin: 24
            anchors.verticalCenter: parent.verticalCenter
            spacing: 10
            LevelBar {
                width: 27
                height: 18
                segmentCount: 7
                level: 0.7
            }
            Text {
                text: "JAM"
                color: "#f2f4f5"
                font.family: "Segoe UI Variable Display"
                font.pixelSize: 20
                font.weight: Font.DemiBold
            }
        }

        IconButton {
            anchors.right: parent.right
            anchors.rightMargin: 18
            anchors.verticalCenter: parent.verticalCenter
            iconSource: Qt.resolvedUrl("../../assets/settings.svg")
            Accessible.name: "Open settings"
            onClicked: root.controller.navigate("settings")
        }
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: 16
            anchors.rightMargin: 16
            height: 1
            color: "#1c252b"
        }
    }

    Column {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        anchors.margins: 24
        spacing: 12

        Text {
            text: "Your sound starts here"
            color: "#f4f5f6"
            font.family: "Segoe UI Variable Display"
            font.pixelSize: 22
            font.weight: Font.DemiBold
        }
        Text {
            text: "Set up a private monitor before entering any room."
            color: "#a4acb2"
            font.family: "Segoe UI Variable Text"
            font.pixelSize: 12
        }

        JamCard {
            width: parent.width
            height: 194

            Column {
                anchors.fill: parent
                anchors.margins: 14
                spacing: 12
                Text {
                    text: "YOUR SETUP"
                    color: "#dfe3e5"
                    font.family: "Segoe UI Variable Text"
                    font.pixelSize: 10
                    font.weight: Font.Medium
                }
                Repeater {
                    model: [
                        {name: "Guitar", detail: root.controller.instrumentDevices[root.controller.instrumentDeviceIndex], icon: "../../assets/music_note.svg"},
                        {name: "Microphone", detail: root.controller.voiceDevices[root.controller.voiceDeviceIndex], icon: "../../assets/mic.svg"},
                        {name: "Output", detail: root.controller.outputDevices[root.controller.outputDeviceIndex], icon: "../../assets/volume_up.svg"}
                    ]
                    Row {
                        id: setupRow
                        required property var modelData
                        width: parent.width
                        height: 23
                        spacing: 8
                        JamIcon {
                            width: 16
                            height: 16
                            source: Qt.resolvedUrl(setupRow.modelData.icon)
                            color: "#cbd2d6"
                        }
                        Text {
                            width: 88
                            text: setupRow.modelData.name
                            color: "#eff1f2"
                            font.family: "Segoe UI Variable Text"
                            font.pixelSize: 11
                            font.weight: Font.Medium
                        }
                        Text {
                            width: parent.width - 177
                            text: setupRow.modelData.detail
                            color: "#a5adb2"
                            elide: Text.ElideRight
                            font.family: "Segoe UI Variable Text"
                            font.pixelSize: 10
                        }
                        Row {
                            width: 57
                            spacing: 4
                            JamIcon {
                                width: 12
                                height: 12
                                source: Qt.resolvedUrl("../../assets/check_circle.svg")
                                color: root.controller.devicesAvailable && root.controller.allReady
                                    ? "#35dd5e"
                                    : "#687178"
                            }
                            Text {
                                text: root.controller.readinessLabel
                                color: root.controller.devicesAvailable && root.controller.allReady
                                    ? "#42db63"
                                    : "#7b848a"
                                font.family: "Segoe UI Variable Text"
                                font.pixelSize: 9
                            }
                        }
                    }
                }
                JamButton {
                    width: parent.width
                    height: 36
                    primary: true
                    text: "Check My Sound"
                    iconSource: Qt.resolvedUrl("../../assets/headphones.svg")
                    Accessible.name: "Open private sound check"
                    onClicked: root.controller.navigate("soundcheck")
                }
            }
        }

        JamCard {
            width: parent.width
            height: 74
            Row {
                anchors.fill: parent
                anchors.margins: 14
                spacing: 12
                JamIcon {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 24
                    height: 24
                    source: Qt.resolvedUrl("../../assets/headphones.svg")
                    color: "#8a55d9"
                }
                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 4
                    Text {
                        text: "Room sessions are not enabled yet"
                        color: "#e8ebed"
                        font.family: "Segoe UI Variable Text"
                        font.pixelSize: 12
                        font.weight: Font.Medium
                    }
                    Text {
                        text: "Secure transport and real audio backends must pass QA first."
                        color: "#89939a"
                        font.family: "Segoe UI Variable Text"
                        font.pixelSize: 10
                    }
                }
            }
        }

        Item { width: 1; height: 1 }
        Text {
            width: parent.width
            text: root.controller.saveMessage.length > 0
                ? root.controller.saveMessage
                : "Settings save automatically"
            color: "#667178"
            horizontalAlignment: Text.AlignHCenter
            font.family: "Segoe UI Variable Text"
            font.pixelSize: 10
        }
    }
}
