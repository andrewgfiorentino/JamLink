pragma ComponentBehavior: Bound
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls.Basic
import "../components"

Item {
    id: root
    required property AppController controller
    property bool chatOpen: false

    Rectangle {
        id: header
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 58
        color: "transparent"

        Column {
            anchors.left: parent.left
            anchors.leftMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2
            Text {
                text: "Room: Private Jam"
                color: Theme.text
                font.family: Theme.fontFamily
                font.pixelSize: 15
                font.weight: Font.DemiBold
            }
            Text {
                text: root.controller.peerConnected
                    ? "Encrypted room session" : root.controller.roomStatus
                color: root.controller.peerConnected ? Theme.connected : Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: 9
            }
        }

        Row {
            anchors.right: parent.right
            anchors.rightMargin: 15
            anchors.verticalCenter: parent.verticalCenter
            spacing: 7
            Rectangle {
                visible: root.controller.peerConnected
                anchors.verticalCenter: parent.verticalCenter
                width: latencyLabel.implicitWidth + 18
                height: 26
                radius: 9
                color: "#0d2116"
                border.color: "#1d4b30"
                Text {
                    id: latencyLabel
                    anchors.centerIn: parent
                    text: root.controller.roundTripMilliseconds + " ms"
                    color: Theme.connected
                    font.family: Theme.numericFontFamily
                    font.pixelSize: 9
                }
            }
            JamButton {
                width: 68
                height: 30
                text: "Leave"
                enabled: root.controller.roomActive
                onClicked: root.controller.leaveSession()
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
        id: roomScroll
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        contentHeight: roomContent.implicitHeight + 34

        Column {
            id: roomContent
            x: 20
            y: 14
            width: roomScroll.width - 40
            spacing: 12

            JamCard {
                visible: root.controller.roomActive && !root.controller.peerConnected
                width: parent.width
                height: visible ? 116 : 0
                Column {
                    anchors.fill: parent
                    anchors.margins: 13
                    spacing: 8
                    Text {
                        text: root.controller.inviteCode.length > 0
                            ? "Your invite is ready" : "Opening your private room…"
                        color: Theme.text
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                    }
                    Row {
                        visible: root.controller.inviteCode.length > 0
                        width: parent.width
                        spacing: 8
                        Rectangle {
                            width: parent.width - copyInvite.width - 8
                            height: 36
                            radius: Theme.radiusControl
                            color: Theme.surfaceNested
                            border.color: Theme.border
                            Text {
                                anchors.fill: parent
                                anchors.margins: 10
                                verticalAlignment: Text.AlignVCenter
                                text: root.controller.inviteCode
                                color: "#d9c7f5"
                                elide: Text.ElideMiddle
                                font.family: Theme.numericFontFamily
                                font.pixelSize: 9
                            }
                        }
                        JamButton {
                            id: copyInvite
                            width: 76
                            height: 36
                            text: "Copy"
                            onClicked: root.controller.copyInvite()
                        }
                    }
                    Text {
                        width: parent.width
                        text: "Your friend can paste this code on Home. Keep JamLink open."
                        color: Theme.textMuted
                        wrapMode: Text.WordWrap
                        font.family: Theme.fontFamily
                        font.pixelSize: 9
                    }
                }
            }

            Flow {
                id: participantsFlow
                readonly property int cardColumns: width >= 900
                    ? Math.min(3, root.controller.roomParticipantCount)
                    : width >= 440 ? Math.min(2, root.controller.roomParticipantCount) : 1
                readonly property int cardRows: Math.ceil(
                    root.controller.roomParticipantCount / Math.max(1, cardColumns))
                width: parent.width
                height: cardRows * 330 + Math.max(0, cardRows - 1) * spacing
                spacing: 10

                Repeater {
                    model: root.controller.roomParticipants
                    delegate: JamCard {
                        id: participantCard
                        required property var modelData
                        width: (participantsFlow.width
                            - participantsFlow.spacing * (participantsFlow.cardColumns - 1))
                            / participantsFlow.cardColumns
                        height: 330
                        border.color: participantCard.modelData.accent
                        color: participantCard.modelData.surface

                        Column {
                            anchors.fill: parent
                            anchors.margins: 12
                            spacing: 9
                            Row {
                                width: parent.width
                                Text {
                                    width: parent.width - participantState.width
                                    text: participantCard.modelData.displayName.toUpperCase()
                                    color: Theme.text
                                    elide: Text.ElideRight
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 11
                                    font.weight: Font.DemiBold
                                }
                                Text {
                                    id: participantState
                                    text: participantCard.modelData.stateLabel
                                    color: participantCard.modelData.present
                                        ? Theme.connected : Theme.textMuted
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 8
                                    font.weight: Font.DemiBold
                                }
                            }
                            ProfileAvatar {
                                anchors.horizontalCenter: parent.horizontalCenter
                                width: 68
                                height: 68
                                avatarId: participantCard.modelData.avatarId
                                customSource: participantCard.modelData.customAvatarSource
                                ringColor: participantCard.modelData.accent
                            }
                            StreamPanel {
                                width: parent.width
                                title: participantCard.modelData.instrument
                                icon: "music_note.svg"
                                level: participantCard.modelData.instrumentLevel
                                gain: participantCard.modelData.instrumentGain
                                muted: participantCard.modelData.instrumentMuted
                                accent: participantCard.modelData.accent
                                enabled: participantCard.modelData.controlsEnabled
                                interactiveMute: !participantCard.modelData.local
                                    && participantCard.modelData.controlsEnabled
                                onGainMoved: value => root.controller.setRoomParticipantStreamGain(
                                    participantCard.modelData.participantId, "instrument", value)
                                onMuteToggled: muted => root.controller.setRoomParticipantStreamMuted(
                                    participantCard.modelData.participantId, "instrument", muted)
                            }
                            StreamPanel {
                                width: parent.width
                                title: "Microphone"
                                icon: "mic.svg"
                                level: participantCard.modelData.voiceLevel
                                gain: participantCard.modelData.voiceGain
                                muted: participantCard.modelData.voiceMuted
                                accent: participantCard.modelData.accent
                                enabled: participantCard.modelData.controlsEnabled
                                interactiveMute: !participantCard.modelData.local
                                    && participantCard.modelData.controlsEnabled
                                onGainMoved: value => root.controller.setRoomParticipantStreamGain(
                                    participantCard.modelData.participantId, "voice", value)
                                onMuteToggled: muted => root.controller.setRoomParticipantStreamMuted(
                                    participantCard.modelData.participantId, "voice", muted)
                            }
                        }
                    }
                }
            }

            Row {
                width: parent.width
                height: 44
                spacing: 9
                JamButton {
                    width: (parent.width - 18) / 3
                    height: parent.height
                    text: "Tuner"
                    iconSource: Qt.resolvedUrl("../../assets/tune.svg")
                    onClicked: root.controller.navigate("tuner")
                }
                JamButton {
                    width: (parent.width - 18) / 3
                    height: parent.height
                    text: root.controller.recording ? "Stop " + root.controller.recordingElapsed : "Record"
                    iconSource: Qt.resolvedUrl("../../assets/music_note.svg")
                    enabled: root.controller.audioActive
                    onClicked: root.controller.toggleRecording()
                }
                JamButton {
                    width: (parent.width - 18) / 3
                    height: parent.height
                    text: root.controller.unreadChatCount > 0
                        ? "Chat " + root.controller.unreadChatCount : "Chat"
                    iconSource: Qt.resolvedUrl("../../assets/mic.svg")
                    enabled: root.controller.peerConnected
                    onClicked: {
                        root.chatOpen = true
                        root.controller.markChatRead()
                    }
                }
            }

            JamCard {
                width: parent.width
                height: 70
                Row {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 10
                    DeviceSelector {
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.min(250, parent.width * 0.52)
                        model: root.controller.outputDevices
                        currentIndex: root.controller.outputDeviceIndex
                        enabled: root.controller.devicesAvailable
                        onActivated: index => root.controller.outputDeviceIndex = index
                    }
                    JamIcon {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 16
                        height: 16
                        source: Qt.resolvedUrl("../../assets/volume_up.svg")
                        color: Theme.textSecondary
                    }
                    LevelBar {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - 286
                        height: 12
                        segmentCount: 30
                        level: root.controller.outputLevel
                        peakHold: root.controller.outputPeakHold
                        clipped: root.controller.outputClipped
                    }
                }
            }

            Text {
                width: parent.width
                text: root.controller.roomStatus
                color: Theme.textMuted
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                font.family: Theme.fontFamily
                font.pixelSize: 9
            }
        }
    }

    Rectangle {
        id: chatDrawer
        z: 30
        visible: root.chatOpen
        anchors.top: header.bottom
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: Math.min(390, parent.width - 16)
        color: Theme.surfaceNested
        border.color: "#3a454d"
        radius: Theme.radiusPanel

        Rectangle {
            id: chatHeader
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 54
            color: "transparent"
            Text {
                anchors.left: parent.left
                anchors.leftMargin: 17
                anchors.verticalCenter: parent.verticalCenter
                text: "Room chat"
                color: Theme.text
                font.family: Theme.fontFamily
                font.pixelSize: 16
                font.weight: Font.DemiBold
            }
            IconButton {
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                iconSource: Qt.resolvedUrl("../../assets/close.svg")
                Accessible.name: "Close room chat"
                onClicked: root.chatOpen = false
            }
            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: Theme.borderSoft
            }
        }

        ListView {
            id: chatList
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: chatHeader.bottom
            anchors.bottom: composer.top
            anchors.margins: 14
            spacing: 10
            clip: true
            model: root.controller.chatMessages
            delegate: Item {
                id: chatEntry
                required property var modelData
                width: ListView.view.width
                height: messageBubble.implicitHeight + 8
                Rectangle {
                    id: messageBubble
                    width: chatEntry.modelData.system
                        ? parent.width : Math.min(parent.width, messageText.implicitWidth + 28)
                    anchors.right: chatEntry.modelData.own ? parent.right : undefined
                    anchors.left: chatEntry.modelData.own ? undefined : parent.left
                    implicitHeight: messageColumn.implicitHeight + 18
                    radius: 11
                    color: chatEntry.modelData.system ? "transparent"
                        : chatEntry.modelData.own ? "#322047" : Theme.surfaceRaised
                    Column {
                        id: messageColumn
                        anchors.fill: parent
                        anchors.margins: 9
                        spacing: 3
                        Row {
                            visible: !chatEntry.modelData.system
                            width: parent.width
                            Text {
                                width: parent.width - timeText.width
                                text: chatEntry.modelData.own ? "You" : chatEntry.modelData.sender
                                color: chatEntry.modelData.own ? "#d0aaf4" : Theme.connected
                                font.family: Theme.fontFamily
                                font.pixelSize: 9
                                font.weight: Font.DemiBold
                            }
                            Text {
                                id: timeText
                                text: chatEntry.modelData.time
                                color: Theme.textMuted
                                font.family: Theme.fontFamily
                                font.pixelSize: 8
                            }
                        }
                        Text {
                            id: messageText
                            width: Math.min(chatList.width - 28, implicitWidth)
                            text: chatEntry.modelData.text
                            color: chatEntry.modelData.system ? Theme.textMuted : Theme.text
                            wrapMode: Text.Wrap
                            horizontalAlignment: chatEntry.modelData.system
                                ? Text.AlignHCenter : Text.AlignLeft
                            font.family: Theme.fontFamily
                            font.pixelSize: 10
                        }
                    }
                }
            }
            Connections {
                target: root.controller
                function onChatChanged() {
                    chatList.positionViewAtEnd()
                    if (root.chatOpen)
                        root.controller.markChatRead()
                }
            }
        }

        Row {
            id: composer
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 13
            height: 58
            spacing: 8
            TextArea {
                id: chatInput
                width: parent.width - sendChat.width - 8
                height: 48
                placeholderText: "Type a message…"
                wrapMode: TextEdit.Wrap
                color: Theme.text
                placeholderTextColor: Theme.textMuted
                selectionColor: Theme.accent
                font.family: Theme.fontFamily
                font.pixelSize: 10
                background: Rectangle {
                    radius: Theme.radiusControl
                    color: Theme.surfaceRaised
                    border.color: chatInput.activeFocus ? Theme.accentBright : Theme.border
                }
                Keys.onReturnPressed: event => {
                    if ((event.modifiers & Qt.ShiftModifier) !== 0) {
                        chatInput.insert(chatInput.cursorPosition, "\n")
                    } else if (root.controller.sendChatMessage(chatInput.text)) {
                        chatInput.clear()
                    }
                    event.accepted = true
                }
            }
            JamButton {
                id: sendChat
                anchors.verticalCenter: parent.verticalCenter
                width: 72
                height: 36
                primary: true
                text: "Send"
                enabled: root.controller.peerConnected && chatInput.text.trim().length > 0
                onClicked: {
                    if (root.controller.sendChatMessage(chatInput.text))
                        chatInput.clear()
                }
            }
        }
    }

    component StreamPanel: Rectangle {
        id: stream
        required property string title
        required property string icon
        property real level: 0
        property real gain: 1
        property bool muted: false
        property color accent: Theme.accent
        property bool interactiveMute: false
        signal gainMoved(real value)
        signal muteToggled(bool muted)
        height: 88
        radius: 10
        color: Theme.surfaceNested
        border.color: Theme.borderSoft

        Column {
            anchors.fill: parent
            anchors.margins: 9
            spacing: 6
            Row {
                width: parent.width
                height: 17
                spacing: 6
                JamIcon {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 14
                    height: 14
                    source: Qt.resolvedUrl("../../assets/" + stream.icon)
                    color: stream.muted ? Theme.textMuted : stream.accent
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - 20
                    text: stream.title
                    color: stream.muted ? Theme.textMuted : Theme.text
                    elide: Text.ElideRight
                    font.family: Theme.fontFamily
                    font.pixelSize: 10
                    font.weight: Font.Medium
                }
            }
            LevelBar {
                width: parent.width
                height: 9
                segmentCount: 24
                level: stream.muted ? 0 : stream.level
            }
            Row {
                width: parent.width
                height: 22
                spacing: 7
                JamSlider {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - (stream.interactiveMute ? muteSwitch.width + 7 : 0)
                    value: stream.gain
                    onMoved: stream.gainMoved(value)
                }
                JamSwitch {
                    id: muteSwitch
                    visible: stream.interactiveMute
                    anchors.verticalCenter: parent.verticalCenter
                    checked: !stream.muted
                    onToggled: stream.muteToggled(!checked)
                }
            }
        }
    }
}
