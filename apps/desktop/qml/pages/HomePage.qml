pragma ComponentBehavior: Bound
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls.Basic
import "../components"

Item {
    id: root
    required property AppController controller
    property bool joinExpanded: false
    property bool createExpanded: root.controller.visualFixture
        && root.controller.privateRoomCodesAvailable
    readonly property string greeting: {
        const hour = new Date().getHours()
        return hour < 12 ? "Good morning" : hour < 18 ? "Good afternoon" : "Good evening"
    }
    readonly property bool canJam: root.controller.audioActive && root.controller.allReady

    Rectangle {
        id: header
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 58
        color: "transparent"

        Row {
            anchors.left: parent.left
            anchors.leftMargin: 22
            anchors.verticalCenter: parent.verticalCenter
            spacing: 9
            LevelBar {
                anchors.verticalCenter: parent.verticalCenter
                width: 27
                height: 18
                segmentCount: 7
                level: 0.72
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "JAM"
                color: Theme.text
                font.family: Theme.fontFamily
                font.pixelSize: 20
                font.weight: Font.DemiBold
            }
        }

        Row {
            anchors.right: parent.right
            anchors.rightMargin: 16
            anchors.verticalCenter: parent.verticalCenter
            spacing: 3
            IconButton {
                iconSource: Qt.resolvedUrl("../../assets/settings.svg")
                Accessible.name: "Open settings"
                onClicked: root.controller.navigate("settings")
            }
            ProfileAvatar {
                anchors.verticalCenter: parent.verticalCenter
                width: 34
                height: 34
                avatarId: root.controller.profileAvatarId
                customSource: root.controller.profileCustomAvatarSource
                Accessible.name: "Open musician profile"
                Accessible.role: Accessible.Button
                TapHandler { onTapped: root.controller.navigate("profile") }
            }
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

    Flickable {
        id: scroll
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        clip: true
        contentHeight: content.implicitHeight + 36
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: content
            x: Theme.pagePadding
            y: 18
            width: scroll.width - Theme.pagePadding * 2
            spacing: 12

            Column {
                width: parent.width
                spacing: 3
                Text {
                    text: root.greeting + ", " + root.controller.profileDisplayName
                    color: Theme.text
                    font.family: Theme.fontFamily
                    font.pixelSize: 22
                    font.weight: Font.DemiBold
                }
                Text {
                    text: root.canJam ? "Your rig is ready to jam." : "Let’s get your rig ready."
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                }
            }

            JamCard {
                width: parent.width
                height: 190

                Column {
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 9
                    Text {
                        text: "YOUR SETUP"
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: 10
                        font.weight: Font.DemiBold
                    }
                    Repeater {
                        model: [
                            {label: "Guitar", icon: "music_note.svg",
                             detail: root.controller.instrumentDevices.length > root.controller.instrumentDeviceIndex
                                ? root.controller.instrumentDevices[root.controller.instrumentDeviceIndex] : "Not connected"},
                            {label: "Microphone", icon: "mic.svg",
                             detail: root.controller.voiceDevices.length > root.controller.voiceDeviceIndex
                                ? root.controller.voiceDevices[root.controller.voiceDeviceIndex] : "Not connected"},
                            {label: "Output", icon: "volume_up.svg",
                             detail: root.controller.outputDevices.length > root.controller.outputDeviceIndex
                                ? root.controller.outputDevices[root.controller.outputDeviceIndex] : "Not connected"}
                        ]
                        Row {
                            id: setupRow
                            required property var modelData
                            width: parent.width
                            height: 22
                            spacing: 8
                            JamIcon {
                                anchors.verticalCenter: parent.verticalCenter
                                width: 15
                                height: 15
                                source: Qt.resolvedUrl("../../assets/" + setupRow.modelData.icon)
                                color: Theme.textSecondary
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                width: 84
                                text: setupRow.modelData.label
                                color: Theme.text
                                font.family: Theme.fontFamily
                                font.pixelSize: 11
                                font.weight: Font.Medium
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                width: parent.width - 188
                                text: setupRow.modelData.detail
                                color: Theme.textSecondary
                                elide: Text.ElideRight
                                font.family: Theme.fontFamily
                                font.pixelSize: 10
                            }
                            Row {
                                anchors.verticalCenter: parent.verticalCenter
                                width: 80
                                spacing: 4
                                JamIcon {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 12
                                    height: 12
                                    source: Qt.resolvedUrl("../../assets/check_circle.svg")
                                    color: root.canJam ? Theme.connected : Theme.textMuted
                                }
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: root.canJam ? "Ready" : root.controller.readinessLabel
                                    color: root.canJam ? Theme.connected : Theme.textMuted
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 9
                                }
                            }
                        }
                    }
                    JamButton {
                        width: parent.width
                        height: 36
                        primary: true
                        text: root.canJam ? "Check My Sound" : "Finish Sound Check"
                        iconSource: Qt.resolvedUrl("../../assets/headphones.svg")
                        Accessible.name: "Open private sound check"
                        onClicked: root.controller.navigate("soundcheck")
                    }
                }
            }

            Row {
                width: parent.width
                height: 78
                spacing: 10

                ActionCard {
                    width: (parent.width - parent.spacing) / 2
                    height: parent.height
                    title: "Start a Jam"
                    detail: "Create a room and invite a friend"
                    icon: "headphones.svg"
                    enabled: root.canJam
                    onActivated: {
                        if (root.controller.privateRoomCodesAvailable)
                            root.createExpanded = !root.createExpanded
                        else
                            root.controller.hostSession()
                    }
                }
                ActionCard {
                    width: (parent.width - parent.spacing) / 2
                    height: parent.height
                    title: "Join a Friend"
                    detail: "Enter a temporary code or full invite"
                    icon: "music_note.svg"
                    enabled: root.canJam
                    onActivated: root.joinExpanded = !root.joinExpanded
                }
            }

            JamCard {
                visible: root.createExpanded
                width: parent.width
                height: visible ? 146 : 0
                Column {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 8
                    Row {
                        width: parent.width
                        height: 28
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - randomCodeButton.width
                            text: "TEMPORARY PRIVATE INVITE CODE"
                            color: Theme.textSecondary
                            font.family: Theme.fontFamily
                            font.pixelSize: 9
                            font.weight: Font.DemiBold
                        }
                        JamButton {
                            id: randomCodeButton
                            width: 112
                            height: 28
                            text: "Generate Random"
                            onClicked: roomNameField.text =
                                root.controller.generatePrivateInviteCode()
                        }
                    }
                    Row {
                        width: parent.width
                        spacing: 8
                        TextField {
                            id: roomNameField
                            width: parent.width - namedHostButton.width - 8
                            height: 40
                            placeholderText: "andrew-mike"
                            maximumLength: 64
                            validator: RegularExpressionValidator {
                                regularExpression: /[A-Za-z0-9_-]{0,64}/
                            }
                            color: Theme.text
                            placeholderTextColor: Theme.textMuted
                            selectionColor: Theme.accent
                            font.family: Theme.fontFamily
                            font.pixelSize: 10
                            background: Rectangle {
                                radius: Theme.radiusControl
                                color: Theme.surfaceRaised
                                border.color: roomNameField.activeFocus
                                    ? Theme.accentBright : Theme.border
                            }
                            Keys.onReturnPressed: event => {
                                if (roomNameField.text.trim().length >= 4)
                                    root.controller.hostInviteCodeSession(roomNameField.text)
                                event.accepted = true
                            }
                        }
                        JamButton {
                            id: namedHostButton
                            width: 92
                            height: 40
                            primary: true
                            text: root.controller.privateRoomBusy ? "Creating…" : "Create"
                            enabled: !root.controller.privateRoomBusy
                                && roomNameField.text.trim().length >= 4
                            onClicked: root.controller.hostInviteCodeSession(roomNameField.text)
                        }
                    }
                    Text {
                        text: "Expires when this jam ends · private and unlisted"
                        color: Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: 9
                    }
                }
            }

            JamCard {
                visible: root.joinExpanded
                width: parent.width
                height: visible ? 72 : 0
                Row {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 8
                    TextField {
                        id: inviteField
                        width: parent.width - joinButton.width - 8
                        height: 40
                        placeholderText: root.controller.privateRoomCodesAvailable
                            ? "Temporary code or full JL1 invite" : "Paste the full JL1 invite"
                        color: Theme.text
                        placeholderTextColor: Theme.textMuted
                        selectionColor: Theme.accent
                        font.family: Theme.fontFamily
                        font.pixelSize: 10
                        background: Rectangle {
                            radius: Theme.radiusControl
                            color: Theme.surfaceRaised
                            border.color: inviteField.activeFocus
                                ? Theme.accentBright : Theme.border
                        }
                        Keys.onReturnPressed: event => {
                            if (inviteField.text.trim().length > 0)
                                root.controller.joinSession(inviteField.text)
                            event.accepted = true
                        }
                    }
                    JamButton {
                        id: joinButton
                        width: 92
                        height: 40
                        primary: true
                        text: "Join"
                        enabled: inviteField.text.trim().length > 0
                        onClicked: root.controller.joinSession(inviteField.text)
                    }
                }
            }

            Text {
                width: parent.width
                text: root.controller.setupMessage.length > 0
                    ? root.controller.setupMessage : "Settings save automatically"
                color: Theme.textMuted
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                font.family: Theme.fontFamily
                font.pixelSize: 10
            }
        }
    }

    component ActionCard: JamCard {
        id: action
        required property string title
        required property string detail
        required property string icon
        signal activated
        opacity: enabled ? 1.0 : 0.48
        color: actionArea.containsMouse && enabled ? Theme.hover : Theme.surface

        Row {
            anchors.fill: parent
            anchors.margins: 12
            spacing: 10
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 36
                height: 36
                radius: 12
                color: "#5930a2"
                JamIcon {
                    anchors.centerIn: parent
                    width: 18
                    height: 18
                    source: Qt.resolvedUrl("../../assets/" + action.icon)
                    color: "#ffffff"
                }
            }
            Column {
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - 46
                spacing: 2
                Text {
                    text: action.title
                    color: Theme.text
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    font.weight: Font.Medium
                }
                Text {
                    width: parent.width
                    text: action.detail
                    color: Theme.textMuted
                    wrapMode: Text.WordWrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                    font.family: Theme.fontFamily
                    font.pixelSize: 8
                }
            }
        }
        MouseArea {
            id: actionArea
            anchors.fill: parent
            enabled: action.enabled
            hoverEnabled: true
            cursorShape: action.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
            activeFocusOnTab: true
            Accessible.role: Accessible.Button
            Accessible.name: action.title
            Keys.onReturnPressed: action.activated()
            Keys.onSpacePressed: action.activated()
            onClicked: action.activated()
        }
    }
}
