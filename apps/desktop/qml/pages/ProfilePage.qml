pragma ComponentBehavior: Bound
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import "../components"

Item {
    id: root
    required property AppController controller

    function avatarSource(id) {
        const names = {
            "avatar:guitar-electric": "avatar_guitar_electric.svg",
            "avatar:guitar-acoustic": "avatar_guitar_acoustic.svg",
            "avatar:bass": "avatar_bass.svg",
            "avatar:drums": "avatar_drums.svg",
            "avatar:keys": "avatar_keys.svg",
            "avatar:vocals": "avatar_vocals.svg",
            "avatar:synth": "avatar_synth.svg",
            "avatar:listener": "avatar_listener.svg"
        }
        return Qt.resolvedUrl("../../assets/" + names[id])
    }

    FileDialog {
        id: avatarDialog
        title: "Choose a profile image"
        fileMode: FileDialog.OpenFile
        nameFilters: ["Images (*.png *.jpg *.jpeg *.webp)"]
        onAccepted: root.controller.setCustomAvatar(selectedFile)
    }

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
            Accessible.name: "Back to home"
            onClicked: root.controller.navigate("home")
        }
        Text {
            anchors.centerIn: parent
            text: "Musician Profile"
            color: "#f2f4f5"
            font.family: "Segoe UI"
            font.pixelSize: 17
            font.weight: Font.DemiBold
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

    ScrollView {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        anchors.margins: 18
        clip: true
        contentWidth: availableWidth

        Column {
            width: parent.width
            spacing: 12

            Row {
                width: parent.width
                height: 88
                spacing: 16
                ProfileAvatar {
                    width: 82
                    height: 82
                    avatarId: root.controller.profileAvatarId
                    customSource: root.controller.profileCustomAvatarSource
                }
                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - 98
                    spacing: 7
                    Text {
                        text: root.controller.profileDisplayName
                        color: "#f2f4f5"
                        font.family: "Segoe UI"
                        font.pixelSize: 19
                        font.weight: Font.DemiBold
                    }
                    Text {
                        text: root.controller.profileHandle.length > 0
                            ? "@" + root.controller.profileHandle
                            : "Private profile · handle optional"
                        color: "#929ca2"
                        font.family: "Segoe UI"
                        font.pixelSize: 10
                    }
                    JamButton {
                        width: 126
                        height: 30
                        text: "Custom image"
                        Accessible.name: "Choose custom profile image"
                        onClicked: avatarDialog.open()
                    }
                }
            }

            JamCard {
                width: parent.width
                height: 146
                Column {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 8
                    Text {
                        text: "CHOOSE AN ICON"
                        color: "#9ca5aa"
                        font.family: "Segoe UI"
                        font.pixelSize: 9
                        font.weight: Font.Medium
                    }
                    Grid {
                        columns: 8
                        spacing: 7
                        Repeater {
                            model: root.controller.profileAvatarIds
                            Item {
                                id: avatarChoice
                                required property int index
                                required property string modelData
                                width: 49
                                height: 78
                                ProfileAvatar {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    width: 46
                                    height: 46
                                    avatarId: avatarChoice.modelData
                                    ringColor: root.controller.profileAvatarId === avatarChoice.modelData
                                        ? "#b98cff" : "#344048"
                                }
                                Text {
                                    anchors.top: parent.top
                                    anchors.topMargin: 51
                                    width: parent.width
                                    text: root.controller.profileAvatarLabels[avatarChoice.index]
                                    color: root.controller.profileAvatarId === avatarChoice.modelData
                                        ? "#d7c2ff" : "#7f898f"
                                    font.family: "Segoe UI"
                                    font.pixelSize: 7
                                    horizontalAlignment: Text.AlignHCenter
                                    wrapMode: Text.WordWrap
                                }
                                TapHandler {
                                    onTapped: root.controller.profileAvatarId = avatarChoice.modelData
                                }
                            }
                        }
                    }
                }
            }

            JamCard {
                width: parent.width
                height: 326
                Column {
                    anchors.fill: parent
                    anchors.margins: 13
                    spacing: 8

                    Text { text: "DISPLAY NAME"; color: "#929ca2"; font.pixelSize: 9; font.family: "Segoe UI" }
                    TextField {
                        width: parent.width; height: 34
                        text: root.controller.profileDisplayName
                        maximumLength: 48
                        color: "#edf0f2"; font.pixelSize: 10; font.family: "Segoe UI"
                        onEditingFinished: root.controller.profileDisplayName = text
                        background: Rectangle { radius: 9; color: "#182127"; border.color: parent.activeFocus ? "#8b56df" : "#28343b" }
                    }
                    Row {
                        width: parent.width; spacing: 10
                        Column {
                            width: (parent.width - 10) / 2; spacing: 5
                            Text { text: "HANDLE"; color: "#929ca2"; font.pixelSize: 9; font.family: "Segoe UI" }
                            TextField {
                                width: parent.width; height: 34
                                text: root.controller.profileHandle
                                placeholderText: "andrewf"; maximumLength: 25
                                color: "#edf0f2"; placeholderTextColor: "#667178"
                                font.pixelSize: 10; font.family: "Segoe UI"
                                onEditingFinished: root.controller.profileHandle = text
                                background: Rectangle { radius: 9; color: "#182127"; border.color: parent.activeFocus ? "#8b56df" : "#28343b" }
                            }
                        }
                        Column {
                            width: (parent.width - 10) / 2; spacing: 5
                            Text { text: "PRIMARY ROLE"; color: "#929ca2"; font.pixelSize: 9; font.family: "Segoe UI" }
                            DeviceSelector {
                                width: parent.width
                                model: root.controller.profileInstrumentOptions
                                currentIndex: Math.max(0, root.controller.profileInstrumentOptions.indexOf(root.controller.profilePrimaryInstrument))
                                onActivated: root.controller.profilePrimaryInstrument = currentText
                            }
                        }
                    }
                    Text { text: "GENRES"; color: "#929ca2"; font.pixelSize: 9; font.family: "Segoe UI" }
                    TextField {
                        width: parent.width; height: 34
                        text: root.controller.profileGenres
                        placeholderText: "Rock, jazz, folk…"; maximumLength: 96
                        color: "#edf0f2"; placeholderTextColor: "#667178"
                        font.pixelSize: 10; font.family: "Segoe UI"
                        onEditingFinished: root.controller.profileGenres = text
                        background: Rectangle { radius: 9; color: "#182127"; border.color: parent.activeFocus ? "#8b56df" : "#28343b" }
                    }
                    Text { text: "BIO"; color: "#929ca2"; font.pixelSize: 9; font.family: "Segoe UI" }
                    TextArea {
                        width: parent.width; height: 62
                        text: root.controller.profileBio
                        placeholderText: "What do you like to play?"; wrapMode: TextEdit.Wrap
                        color: "#edf0f2"; placeholderTextColor: "#667178"
                        font.pixelSize: 10; font.family: "Segoe UI"
                        onActiveFocusChanged: if (!activeFocus) root.controller.profileBio = text
                        background: Rectangle { radius: 9; color: "#182127"; border.color: parent.activeFocus ? "#8b56df" : "#28343b" }
                    }
                }
            }

            Text {
                width: parent.width
                text: "Your profile is stored on this computer. Private rooms do not require an account."
                color: "#6f7a80"
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                font.family: "Segoe UI"
                font.pixelSize: 9
            }
            Item { width: 1; height: 8 }
        }
    }
}
