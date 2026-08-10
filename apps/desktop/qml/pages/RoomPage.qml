pragma ComponentBehavior: Bound
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls.Basic
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

        Column {
            anchors.left: parent.left
            anchors.leftMargin: 22
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2
            Text {
                text: "Private Room"
                color: "#f2f4f5"
                font.family: "Segoe UI Variable Display"
                font.pixelSize: 17
                font.weight: Font.DemiBold
            }
            Text {
                text: root.controller.roomStatus
                color: root.controller.peerConnected ? "#44d86a" : "#929ba1"
                font.family: "Segoe UI Variable Text"
                font.pixelSize: 10
            }
        }
        JamButton {
            anchors.right: parent.right
            anchors.rightMargin: 16
            anchors.verticalCenter: parent.verticalCenter
            width: 84
            height: 32
            text: "Leave"
            enabled: root.controller.roomActive
            Accessible.name: "Leave private room"
            onClicked: root.controller.leaveSession()
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
        anchors.bottom: parent.bottom
        anchors.margins: 22
        spacing: 12

        JamCard {
            width: parent.width
            height: root.controller.inviteCode.length > 0 ? 142 : 78

            Column {
                anchors.fill: parent
                anchors.margins: 13
                spacing: 8
                Row {
                    width: parent.width
                    height: 20
                    spacing: 8
                    JamIcon {
                        width: 16
                        height: 16
                        source: Qt.resolvedUrl("../../assets/check_circle.svg")
                        color: root.controller.peerConnected ? "#38d65d" : "#8b56df"
                    }
                    Text {
                        width: parent.width - 24
                        text: !root.controller.roomActive
                            ? "No active private room"
                            : root.controller.peerConnected
                            ? "Your friend is connected"
                            : root.controller.inviteCode.length > 0
                                ? "Send this one-time room code to your friend"
                                : "Connecting to your friend's private room"
                        color: "#eef1f2"
                        font.family: "Segoe UI Variable Text"
                        font.pixelSize: 11
                        font.weight: Font.Medium
                    }
                }
                Row {
                    visible: root.controller.inviteCode.length > 0
                    width: parent.width
                    height: 54
                    spacing: 8
                    TextArea {
                        id: inviteArea
                        width: parent.width - copyButton.width - 8
                        height: 54
                        text: root.controller.inviteCode
                        readOnly: true
                        selectByMouse: true
                        wrapMode: Text.WrapAnywhere
                        color: "#cbd4d9"
                        selectionColor: "#6938c5"
                        font.family: "Cascadia Mono"
                        font.pixelSize: 9
                        background: Rectangle {
                            radius: 6
                            color: "#11191e"
                            border.color: "#28343b"
                        }
                        Accessible.name: "Encrypted invite code"
                    }
                    JamButton {
                        id: copyButton
                        width: 82
                        height: 34
                        anchors.verticalCenter: parent.verticalCenter
                        text: "Copy"
                        Accessible.name: "Copy invite code"
                        onClicked: root.controller.copyInvite()
                    }
                }
                Text {
                    width: parent.width
                    text: !root.controller.roomActive
                        ? "Create an invite or join from the Home screen."
                        : root.controller.inviteCode.length === 0
                        ? "JamLink is negotiating the encrypted direct connection."
                        : root.controller.automaticPortMapping
                            ? "Router mapping is ready. Keep JamLink open while your friend joins."
                            : "If your friend cannot connect, forward UDP port "
                                + root.controller.roomPort + " to this PC or enable UPnP."
                    color: root.controller.inviteCode.length === 0
                        || root.controller.automaticPortMapping ? "#7f8b91" : "#e4b352"
                    wrapMode: Text.WordWrap
                    font.family: "Segoe UI Variable Text"
                    font.pixelSize: 9
                }
            }
        }

        JamCard {
            width: parent.width
            height: 126

            Column {
                anchors.fill: parent
                anchors.margins: 14
                spacing: 10
                Row {
                    width: parent.width
                    Text {
                        width: parent.width - latencyText.width
                        text: "FRIEND AUDIO"
                        color: "#eef1f2"
                        font.family: "Segoe UI Variable Text"
                        font.pixelSize: 10
                        font.weight: Font.Medium
                    }
                    Text {
                        id: latencyText
                        text: root.controller.peerConnected
                            ? root.controller.roundTripMilliseconds + " ms network RTT"
                            : "Waiting"
                        color: root.controller.peerConnected ? "#43d96a" : "#7b858b"
                        font.family: "Segoe UI Variable Text"
                        font.pixelSize: 10
                    }
                }
                LevelBar {
                    width: parent.width
                    height: 20
                    level: root.controller.remoteLevel
                }
                Text {
                    width: parent.width
                    text: root.controller.packetSummary
                    color: "#778289"
                    font.family: "Segoe UI Variable Text"
                    font.pixelSize: 9
                }
                Row {
                    width: parent.width
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - sendSwitch.width
                        text: !root.controller.roomActive
                            ? "No audio is being sent"
                            : root.controller.sendMuted
                            ? "Your audio is muted to your friend"
                            : "Your guitar and microphone are being sent"
                        color: root.controller.sendMuted ? "#e4b352" : "#cbd2d6"
                        font.family: "Segoe UI Variable Text"
                        font.pixelSize: 10
                    }
                    JamSwitch {
                        id: sendSwitch
                        checked: root.controller.roomActive && !root.controller.sendMuted
                        enabled: root.controller.roomActive
                        Accessible.name: "Send my audio to friend"
                        onToggled: root.controller.sendMuted = !checked
                    }
                }
            }
        }

        JamCard {
            width: parent.width
            height: 80
            Row {
                anchors.fill: parent
                anchors.margins: 14
                spacing: 12
                JamIcon {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 22
                    height: 22
                    source: Qt.resolvedUrl("../../assets/headphones.svg")
                    color: "#3bc8ee"
                }
                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 4
                    Text {
                        text: "Direct encrypted UDP audio"
                        color: "#e7eaec"
                        font.family: "Segoe UI Variable Text"
                        font.pixelSize: 11
                        font.weight: Font.Medium
                    }
                    Text {
                        text: "5 ms PCM packets · no relay fallback in this test build"
                        color: "#818c92"
                        font.family: "Segoe UI Variable Text"
                        font.pixelSize: 9
                    }
                }
            }
        }
    }
}
