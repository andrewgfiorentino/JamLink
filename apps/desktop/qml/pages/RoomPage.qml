pragma ComponentBehavior: Bound
// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls.Basic
import "../components"

Item {
    id: root
    required property AppController controller
    property bool chatOpen: false
    property bool tunerOpen: false

    // The detector only runs while the panel is up, and closing it always
    // releases the mute so nobody is left silently muted to the room.
    onTunerOpenChanged: root.controller.tunerActive = root.tunerOpen
    property bool waitingOpen: root.controller.visualPrivateRoomFixture === "host-drawer"

    Rectangle {
        id: header
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 58
        color: "transparent"

        Column {
            id: roomHeading
            anchors.left: parent.left
            anchors.leftMargin: 20
            anchors.right: headerActions.left
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2
            Text {
                width: parent.width
                text: root.controller.privateRoomCode.length > 0
                    ? "Room: " + root.controller.privateRoomCode : "Room: Private Jam"
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: Theme.text
                font.family: Theme.fontFamily
                font.pixelSize: 15
                font.weight: Font.DemiBold
            }
            // One coherent answer, from the session conductor. The interface
            // does not reason across audio, network, transport and recording
            // states itself; it shows what they add up to.
            Text {
                width: parent.width
                text: root.controller.sessionHeadline
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: root.controller.sessionPlayable ? Theme.connected : Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: 9
            }
        }

        // Shown only when there is genuinely one thing worth doing. Guidance
        // offers a single action, never a list, so this is one button.
        Rectangle {
            id: guidanceBanner
            visible: root.controller.sessionActionEnabled
                && root.controller.sessionActionLabel !== ""
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.bottom
            anchors.leftMargin: 15
            anchors.rightMargin: 15
            anchors.topMargin: 8
            height: visible ? Math.max(40, guidanceText.implicitHeight + 16) : 0
            radius: 9
            color: "#1a1626"
            border.color: "#3a2f52"
            z: 2
            Text {
                id: guidanceText
                anchors.left: parent.left
                anchors.right: guidanceButton.left
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: 11
                anchors.rightMargin: 9
                text: root.controller.sessionExplanation
                wrapMode: Text.WordWrap
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: 9
            }
            JamButton {
                id: guidanceButton
                anchors.right: parent.right
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(150, parent.width / 2)
                height: 26
                text: root.controller.sessionActionLabel
                onClicked: root.controller.takeSessionAction()
            }
        }

        Row {
            id: headerActions
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
            Item {
                width: 82
                height: 30
                JamButton {
                    anchors.fill: parent
                    visible: !root.controller.recording
                        && root.controller.waitingRoomRequests.length > 0
                    text: "Waiting " + root.controller.waitingRoomRequests.length
                    onClicked: root.waitingOpen = !root.waitingOpen
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
                height: visible ? 142 : 0
                Column {
                    anchors.fill: parent
                    anchors.margins: 13
                    spacing: 8
                    Text {
                        text: root.controller.privateRoomWaiting
                            ? "Waiting for host approval"
                            : root.controller.inviteCode.length === 0
                                && root.controller.privateRoomCode.length > 0
                                ? root.controller.privateRoomStatus
                            : root.controller.inviteCode.length > 0
                                ? root.controller.connectionPreflightStatus
                                : "Opening your private room…"
                        color: root.controller.connectionPreflightReady
                            ? Theme.connected : root.controller.inviteCode.length > 0
                                ? Theme.warning : Theme.text
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
                                text: root.controller.privateRoomCode.length > 0
                                    ? root.controller.privateRoomCode
                                    : "Encrypted direct invite ready"
                                color: "#d9c7f5"
                                elide: Text.ElideMiddle
                                font.family: root.controller.privateRoomCode.length > 0
                                    ? Theme.numericFontFamily : Theme.fontFamily
                                font.pixelSize: 10
                                Accessible.name: root.controller.privateRoomCode.length > 0
                                    ? "Temporary invite code " + root.controller.privateRoomCode
                                    : "Encrypted direct invite ready"
                            }
                        }
                        JamButton {
                            id: copyInvite
                            width: 104
                            height: 36
                            text: "Copy Invite"
                            onClicked: root.controller.copyInvite()
                        }
                    }
                    Text {
                        width: parent.width
                        text: root.controller.privateRoomWaiting
                            ? "No room audio or media key is available until the host lets you in."
                            : root.controller.inviteCode.length === 0
                                && root.controller.privateRoomCode.length > 0
                                ? "No room audio was shared. Leave to return home."
                            : root.controller.privateRoomCode.length > 0
                                ? "Tell your friend this temporary code. Their request appears quietly here."
                                : root.controller.connectionPreflightDetail
                        color: Theme.textMuted
                        wrapMode: Text.WordWrap
                        font.family: Theme.fontFamily
                        font.pixelSize: 9
                    }
                }
            }

            Flow {
                id: participantsFlow
                visible: !root.controller.privateRoomWaiting
                readonly property int cardColumns: width >= 900
                    ? Math.min(3, root.controller.roomParticipantCount)
                    : width >= 440 ? Math.min(2, root.controller.roomParticipantCount) : 1
                readonly property int cardRows: Math.ceil(
                    root.controller.roomParticipantCount / Math.max(1, cardColumns))
                width: parent.width
                height: visible
                    ? cardRows * 330 + Math.max(0, cardRows - 1) * spacing : 0
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
                                    textFormat: Text.PlainText
                                    color: Theme.text
                                    elide: Text.ElideRight
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 11
                                    font.weight: Font.DemiBold
                                }
                                Text {
                                    id: participantState
                                    text: participantCard.modelData.stateLabel
                                    textFormat: Text.PlainText
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
                                interactiveMute: participantCard.modelData.canMute
                                meterFollowsMute: !participantCard.modelData.local
                                accessibleLabel: participantCard.modelData.displayName
                                    + " " + participantCard.modelData.instrument
                                muteLabel: participantCard.modelData.local
                                    ? "Send my " + participantCard.modelData.instrument
                                        + " to my friend"
                                    : "Hear " + participantCard.modelData.displayName
                                        + "'s " + participantCard.modelData.instrument
                                note: participantCard.modelData.instrumentMutedByPeer
                                    ? "muted by them" : ""
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
                                interactiveMute: participantCard.modelData.canMute
                                meterFollowsMute: !participantCard.modelData.local
                                accessibleLabel: participantCard.modelData.displayName
                                    + " microphone"
                                muteLabel: participantCard.modelData.local
                                    ? "Send my microphone to my friend"
                                    : "Hear " + participantCard.modelData.displayName
                                        + "'s microphone"
                                note: participantCard.modelData.voiceMutedByPeer
                                    ? "muted by them" : ""
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
                visible: !root.controller.privateRoomWaiting
                width: parent.width
                height: visible ? 44 : 0
                spacing: 9
                JamButton {
                    width: (parent.width - 27) / 4
                    height: parent.height
                    text: "Tuner"
                    iconSource: Qt.resolvedUrl("../../assets/tune.svg")
                    onClicked: {
                        root.chatOpen = false
                        root.tunerOpen = !root.tunerOpen
                    }
                }
                JamButton {
                    width: (parent.width - 27) / 4
                    height: parent.height
                    text: root.controller.recording ? "Stop " + root.controller.recordingElapsed : "Record"
                    iconSource: Qt.resolvedUrl("../../assets/music_note.svg")
                    enabled: root.controller.audioActive
                    onClicked: root.controller.toggleRecording()
                }
                JamButton {
                    width: (parent.width - 27) / 4
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
                // Which device you are on is the setting most likely to need
                // changing once you can hear the result, and leaving the room
                // to change it was the only way to reach it.
                JamButton {
                    width: (parent.width - 27) / 4
                    height: parent.height
                    text: "Audio"
                    iconSource: Qt.resolvedUrl("../../assets/settings.svg")
                    Accessible.name: "Audio settings, without leaving the room"
                    onClicked: root.controller.openSettings()
                }
            }

            // Anyone whose interface does direct hardware monitoring is hearing
            // themselves twice: once instantly through the interface and once
            // late through JamLink. Turning the software monitor off leaves the
            // near-zero-latency hardware path on its own, and must not disturb
            // capture, meters, clipping, the tuner, recording, or what the
            // other person hears.
            JamCard {
                visible: !root.controller.privateRoomWaiting
                width: parent.width
                height: visible ? 78 : 0

                Row {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 10

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "Monitor yourself"
                        color: Theme.text
                        font.family: Theme.fontFamily
                        font.pixelSize: 11
                        font.weight: Font.Medium
                    }
                    Item { width: 4; height: 1 }

                    Repeater {
                        model: [
                            {label: "Guitar", icon: "music_note.svg", instrument: true},
                            {label: "Voice", icon: "mic.svg", instrument: false}
                        ]
                        Row {
                            id: monitorRow
                            required property var modelData
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 7

                            readonly property bool monitorOn: monitorRow.modelData.instrument
                                ? root.controller.instrumentMonitorEnabled
                                : root.controller.voiceMonitorEnabled

                            JamIcon {
                                anchors.verticalCenter: parent.verticalCenter
                                width: 15
                                height: 15
                                source: Qt.resolvedUrl(
                                    "../../assets/" + monitorRow.modelData.icon)
                                color: monitorRow.monitorOn ? Theme.accent : "#5d666c"
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: monitorRow.modelData.label
                                color: monitorRow.monitorOn
                                    ? Theme.textSecondary : "#5d666c"
                                font.family: Theme.fontFamily
                                font.pixelSize: 10
                            }
                            JamSwitch {
                                anchors.verticalCenter: parent.verticalCenter
                                checked: monitorRow.monitorOn
                                Accessible.name: "Hear my own "
                                    + monitorRow.modelData.label + " through JamLink"
                                onToggled: {
                                    if (monitorRow.modelData.instrument)
                                        root.controller.instrumentMonitorEnabled = checked
                                    else
                                        root.controller.voiceMonitorEnabled = checked
                                }
                            }
                            Item { width: 6; height: 1 }
                        }
                    }

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - 470
                        visible: width > 60
                        text: (!root.controller.instrumentMonitorEnabled
                                && !root.controller.voiceMonitorEnabled)
                            ? "Off · use your interface's direct monitoring"
                            : "Your friend still hears you either way"
                        color: Theme.textSecondary
                        elide: Text.ElideRight
                        font.family: Theme.fontFamily
                        font.pixelSize: 9
                    }
                }
            }

            JamCard {
                visible: !root.controller.privateRoomWaiting
                width: parent.width
                height: visible ? 70 : 0
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
                        resettable: true
                        onResetRequested: root.controller.clearOutputClipping()
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

    // A compact tuner that lives beside the room rather than replacing it, so
    // the meters, the participants and the chat stay visible while tuning.
    // Opening it mutes the guitar to the room by default, because the usual
    // reason to open it is not wanting to be heard tuning; the switch lets it
    // stay open while playing for anyone who would rather watch their pitch.
    Rectangle {
        id: tunerDrawer
        z: 31
        visible: root.tunerOpen
        anchors.top: header.bottom
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: Math.min(268, parent.width - 16)
        color: Theme.surfaceNested
        border.color: "#3a454d"
        radius: Theme.radiusPanel

        readonly property real cents: root.controller.tunerCents
        readonly property bool inTune: root.controller.tunerDetected
            && Math.abs(tunerDrawer.cents) <= 3.0

        Column {
            anchors.fill: parent
            anchors.margins: 14
            spacing: 10

            Row {
                width: parent.width
                height: 26
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - 28
                    text: "Tuner"
                    color: Theme.text
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                }
                IconButton {
                    anchors.verticalCenter: parent.verticalCenter
                    iconSource: Qt.resolvedUrl("../../assets/close.svg")
                    Accessible.name: "Close tuner"
                    onClicked: root.tunerOpen = false
                }
            }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: root.controller.tunerNote
                    + (root.controller.tunerDetected ? root.controller.tunerOctave : "")
                color: root.controller.tunerDetected
                    ? (tunerDrawer.inTune ? Theme.connected : Theme.text)
                    : Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: 46
                font.weight: Font.Light
                Behavior on color { ColorAnimation { duration: 120 } }
            }

            Item {
                width: parent.width
                height: 28
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width
                    height: 3
                    radius: 2
                    color: Theme.borderSoft
                }
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: parent.width * 0.12
                    height: 3
                    radius: 2
                    color: "#1f5c33"
                }
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 2
                    height: 14
                    color: Theme.textMuted
                    x: parent.width / 2 - 1
                }
                Rectangle {
                    visible: root.controller.tunerDetected
                    width: 4
                    height: 22
                    radius: 2
                    anchors.verticalCenter: parent.verticalCenter
                    x: parent.width / 2
                        + Math.max(-50, Math.min(50, tunerDrawer.cents)) / 50.0
                            * (parent.width / 2) - 2
                    color: tunerDrawer.inTune ? Theme.connected : Theme.accent
                    Behavior on x { NumberAnimation { duration: 90; easing.type: Easing.OutQuad } }
                }
            }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: root.controller.tunerDetected
                    ? (tunerDrawer.cents >= 0 ? "+" : "") + tunerDrawer.cents.toFixed(1) + " cents"
                    : "Play a single string"
                color: tunerDrawer.inTune ? Theme.connected : Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: 10
            }

            Rectangle { width: parent.width; height: 1; color: Theme.borderSoft }

            Row {
                width: parent.width
                spacing: 8
                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - tunerMuteSwitch.width - 8
                    spacing: 2
                    Text {
                        text: "Mute guitar while tuning"
                        color: Theme.text
                        font.family: Theme.fontFamily
                        font.pixelSize: 10
                    }
                    Text {
                        width: parent.width
                        text: root.controller.tunerMutesInstrument
                            ? "Your friend cannot hear you tune. Your mic still works."
                            : "Your friend can hear you. Leave the tuner open and play."
                        color: Theme.textSecondary
                        wrapMode: Text.WordWrap
                        font.family: Theme.fontFamily
                        font.pixelSize: 8
                    }
                }
                JamSwitch {
                    id: tunerMuteSwitch
                    anchors.verticalCenter: parent.verticalCenter
                    checked: root.controller.tunerMutesInstrument
                    Accessible.name: "Mute my guitar to the room while the tuner is open"
                    onToggled: root.controller.tunerMutesInstrument = checked
                }
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
                                textFormat: Text.PlainText
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
                            textFormat: Text.PlainText
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

    Rectangle {
        id: waitingDrawer
        z: 31
        visible: root.waitingOpen && !root.controller.recording
        anchors.top: header.bottom
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: Math.min(390, parent.width - 16)
        color: Theme.surfaceNested
        border.color: "#3a454d"
        radius: Theme.radiusPanel

        Rectangle {
            id: waitingHeader
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 62
            color: "transparent"
            Column {
                anchors.left: parent.left
                anchors.leftMargin: 17
                anchors.verticalCenter: parent.verticalCenter
                spacing: 2
                Text {
                    text: "Waiting to join"
                    color: Theme.text
                    font.family: Theme.fontFamily
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                }
                Text {
                    text: "No room audio is available before approval"
                    color: Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: 9
                }
            }
            IconButton {
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                iconSource: Qt.resolvedUrl("../../assets/close.svg")
                Accessible.name: "Close waiting list"
                onClicked: root.waitingOpen = false
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
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: waitingHeader.bottom
            anchors.bottom: parent.bottom
            anchors.margins: 14
            spacing: 10
            clip: true
            model: root.controller.waitingRoomRequests
            delegate: JamCard {
                id: waitingEntry
                required property var modelData
                width: ListView.view.width
                height: 128
                Column {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 7
                    Row {
                        width: parent.width
                        spacing: 10
                        ProfileAvatar {
                            width: 38
                            height: 38
                            avatarId: waitingEntry.modelData.avatar_id
                        }
                        Column {
                            width: parent.width - 48
                            spacing: 2
                            Text {
                                text: waitingEntry.modelData.display_name
                                textFormat: Text.PlainText
                                color: Theme.text
                                font.family: Theme.fontFamily
                                font.pixelSize: 12
                                font.weight: Font.DemiBold
                            }
                            Text {
                                text: waitingEntry.modelData.primary_instrument.length > 0
                                    ? waitingEntry.modelData.primary_instrument : "Musician"
                                textFormat: Text.PlainText
                                color: Theme.textSecondary
                                font.family: Theme.fontFamily
                                font.pixelSize: 9
                            }
                        }
                    }
                    Row {
                        width: parent.width
                        spacing: 8
                        JamButton {
                            width: (parent.width - 8) / 2
                            height: 34
                            text: "Deny"
                            enabled: !root.controller.privateRoomBusy
                            onClicked: root.controller.decideWaitingRequest(
                                waitingEntry.modelData.request_id, false)
                        }
                        JamButton {
                            width: (parent.width - 8) / 2
                            height: 34
                            primary: true
                            text: root.controller.peerConnected ? "Room full" : "Let In"
                            enabled: !root.controller.privateRoomBusy
                                && !root.controller.peerConnected
                            onClicked: root.controller.decideWaitingRequest(
                                waitingEntry.modelData.request_id, true)
                        }
                    }
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
        // A remote stream that is muted genuinely stopped arriving, so zeroing
        // its meter is the truth. Your own muted stream is still being
        // captured, and hiding its level would take away the very thing you
        // mute in order to check: whether the input is still too hot.
        property bool meterFollowsMute: true
        property string accessibleLabel: title
        property string muteLabel: accessibleLabel
        // State the panel reports but cannot change, such as the friend having
        // muted themselves.
        property string note: ""
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
                    width: parent.width - 20 - (stream.note === "" ? 0 : noteLabel.width + 6)
                    text: stream.title
                    color: stream.muted ? Theme.textMuted : Theme.text
                    elide: Text.ElideRight
                    font.family: Theme.fontFamily
                    font.pixelSize: 10
                    font.weight: Font.Medium
                }
                Text {
                    id: noteLabel
                    anchors.verticalCenter: parent.verticalCenter
                    visible: stream.note !== ""
                    text: stream.note
                    color: Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: 9
                }
            }
            LevelBar {
                width: parent.width
                height: 9
                segmentCount: 24
                level: stream.muted && stream.meterFollowsMute ? 0 : stream.level
            }
            Row {
                width: parent.width
                height: 22
                spacing: 7
                JamSlider {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - (stream.interactiveMute ? muteSwitch.width + 7 : 0)
                    value: stream.gain
                    Accessible.name: stream.accessibleLabel + " level"
                    onMoved: stream.gainMoved(value)
                }
                JamSwitch {
                    id: muteSwitch
                    visible: stream.interactiveMute
                    anchors.verticalCenter: parent.verticalCenter
                    checked: !stream.muted
                    Accessible.name: stream.muteLabel
                    onToggled: {
                        stream.muteToggled(!checked)
                        // Clicking a Switch replaces the binding above with a
                        // plain value. Restoring it keeps the control showing
                        // what is actually true rather than the last thing that
                        // was clicked, so a refused or later-reset mute cannot
                        // leave the switch lying about the state.
                        checked = Qt.binding(function() { return !stream.muted })
                    }
                }
            }
        }
    }
}
