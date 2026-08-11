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
            anchors.leftMargin: 22
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2
            Text {
                text: root.controller.peerConnected
                    ? "Room with " + root.controller.remoteDisplayName
                    : "Private Room"
                color: "#f2f4f5"
                font.family: "Segoe UI"
                font.pixelSize: 17
                font.weight: Font.DemiBold
            }
            Text {
                text: root.controller.roomStatus
                color: root.controller.peerConnected ? "#44d86a" : "#929ba1"
                font.family: "Segoe UI"
                font.pixelSize: 10
            }
        }
        Row {
            anchors.right: parent.right
            anchors.rightMargin: 16
            anchors.verticalCenter: parent.verticalCenter
            spacing: 6
            ProfileAvatar {
                visible: root.controller.peerConnected
                anchors.verticalCenter: parent.verticalCenter
                width: visible ? 30 : 0
                height: 30
                avatarId: root.controller.remoteAvatarId
                ringColor: "#42d97a"
            }
            JamButton {
                anchors.verticalCenter: parent.verticalCenter
                width: 70
                height: 32
                text: root.controller.unreadChatCount > 0
                    ? "Chat " + root.controller.unreadChatCount : "Chat"
                enabled: root.controller.peerConnected
                Accessible.name: "Open room chat"
                onClicked: {
                    root.chatOpen = !root.chatOpen
                    if (root.chatOpen)
                        root.controller.markChatRead()
                }
            }
            IconButton {
                anchors.verticalCenter: parent.verticalCenter
                iconSource: Qt.resolvedUrl("../../assets/tune.svg")
                Accessible.name: "Open tuner"
                onClicked: root.controller.navigate("tuner")
            }
            JamButton {
                anchors.verticalCenter: parent.verticalCenter
                width: 70
                height: 32
                text: "Leave"
                enabled: root.controller.roomActive
                Accessible.name: "Leave private room"
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
                            ? root.controller.remoteDisplayName + " is connected"
                            : root.controller.inviteCode.length > 0
                                ? "Send this one-time room code to your friend"
                                : "Connecting to your friend's private room"
                        color: "#eef1f2"
                        font.family: "Segoe UI"
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
                    font.family: "Segoe UI"
                    font.pixelSize: 9
                }
            }
        }

        JamCard {
            width: parent.width
            height: 220

            Column {
                anchors.fill: parent
                anchors.margins: 14
                spacing: 9
                Row {
                    width: parent.width
                    Text {
                        width: parent.width - latencyText.width
                        text: "WHAT YOU HEAR"
                        color: "#eef1f2"
                        font.family: "Segoe UI"
                        font.pixelSize: 10
                        font.weight: Font.Medium
                    }
                    Text {
                        id: latencyText
                        text: root.controller.peerConnected
                            ? root.controller.roundTripMilliseconds + " ms round trip"
                            : "Waiting"
                        color: root.controller.peerConnected ? "#43d96a" : "#7b858b"
                        font.family: "Segoe UI"
                        font.pixelSize: 10
                    }
                }

                // Instrument and voice arrive as independent streams, so each
                // gets its own meter, level, and mute.
                Repeater {
                    model: [
                        {
                            label: root.controller.peerConnected
                                ? root.controller.remoteDisplayName + " · "
                                    + root.controller.remotePrimaryInstrument
                                : "Their instrument",
                            icon: "music_note.svg",
                            tint: "#8b56df"
                        },
                        {
                            label: root.controller.peerConnected
                                ? root.controller.remoteDisplayName + " · voice"
                                : "Their voice",
                            icon: "mic.svg",
                            tint: "#42d97a"
                        }
                    ]
                    Row {
                        id: streamRow
                        required property int index
                        required property var modelData
                        width: parent.width
                        height: 30
                        spacing: 9

                        readonly property bool isInstrument: streamRow.index === 0
                        readonly property real streamLevel: streamRow.isInstrument
                            ? root.controller.remoteInstrumentLevel
                            : root.controller.remoteVoiceLevel
                        readonly property bool streamMuted: streamRow.isInstrument
                            ? root.controller.remoteInstrumentMuted
                            : root.controller.remoteVoiceMuted
                        readonly property bool sourceClipped: streamRow.isInstrument
                            ? root.controller.remoteInstrumentClipped
                            : root.controller.remoteVoiceClipped

                        JamIcon {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 15
                            height: 15
                            source: Qt.resolvedUrl("../../assets/" + streamRow.modelData.icon)
                            color: streamRow.streamMuted ? "#5d666c" : streamRow.modelData.tint
                        }
                        Column {
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - 15 - streamLevelSlider.width
                                - streamSwitch.width - 27
                            spacing: 3
                            Text {
                                text: streamRow.modelData.label
                                    + (streamRow.sourceClipped ? " · CLIPPING" : "")
                                color: streamRow.streamMuted ? "#7b858b"
                                    : streamRow.sourceClipped ? "#ff746b" : "#cbd2d6"
                                font.family: "Segoe UI"
                                font.pixelSize: 10
                            }
                            LevelBar {
                                width: parent.width
                                height: 12
                                segmentCount: 34
                                level: streamRow.streamMuted ? 0 : streamRow.streamLevel
                                clipped: streamRow.sourceClipped
                            }
                        }
                        JamSlider {
                            id: streamLevelSlider
                            anchors.verticalCenter: parent.verticalCenter
                            width: 86
                            enabled: root.controller.roomActive
                            value: streamRow.isInstrument
                                ? root.controller.remoteInstrumentGain
                                : root.controller.remoteVoiceGain
                            Accessible.name: streamRow.modelData.label + " level"
                            onMoved: {
                                if (streamRow.isInstrument)
                                    root.controller.remoteInstrumentGain = value
                                else
                                    root.controller.remoteVoiceGain = value
                            }
                        }
                        JamSwitch {
                            id: streamSwitch
                            anchors.verticalCenter: parent.verticalCenter
                            checked: !streamRow.streamMuted
                            enabled: root.controller.roomActive
                            Accessible.name: "Hear " + streamRow.modelData.label
                            onToggled: {
                                if (streamRow.isInstrument)
                                    root.controller.remoteInstrumentMuted = !checked
                                else
                                    root.controller.remoteVoiceMuted = !checked
                            }
                        }
                    }
                }

                Rectangle {
                    width: parent.width
                    height: 1
                    color: "#1d272d"
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
                            : "Your guitar and microphone are being sent separately"
                        color: root.controller.sendMuted ? "#e4b352" : "#cbd2d6"
                        font.family: "Segoe UI"
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

                Repeater {
                    model: [
                        { label: "Your guitar", instrument: true },
                        { label: "Your voice", instrument: false }
                    ]
                    Row {
                        id: localSignalRow
                        required property var modelData
                        width: parent.width
                        height: 20
                        spacing: 8
                        readonly property bool sourceClipped: localSignalRow.modelData.instrument
                            ? (root.controller.instrumentInputClipped
                                || root.controller.instrumentSendClipped)
                            : (root.controller.voiceInputClipped
                                || root.controller.voiceSendClipped)
                        readonly property real sourceLevel: localSignalRow.modelData.instrument
                            ? root.controller.instrumentLevel : root.controller.voiceLevel
                        readonly property real sourcePeak: localSignalRow.modelData.instrument
                            ? root.controller.instrumentPeakHold : root.controller.voicePeakHold

                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 58
                            text: localSignalRow.modelData.label
                            color: localSignalRow.sourceClipped ? "#ff746b" : "#aeb7bc"
                            font.family: "Segoe UI"
                            font.pixelSize: 9
                        }
                        LevelBar {
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - 58 - localClip.width - 16
                            height: 10
                            segmentCount: 34
                            level: localSignalRow.sourceLevel
                            peakHold: localSignalRow.sourcePeak
                            clipped: localSignalRow.sourceClipped
                        }
                        ClipLatch {
                            id: localClip
                            anchors.verticalCenter: parent.verticalCenter
                            width: 60
                            height: 20
                            clipped: localSignalRow.sourceClipped
                            onClicked: {
                                if (localSignalRow.modelData.instrument)
                                    root.controller.clearInstrumentClipping()
                                else
                                    root.controller.clearVoiceClipping()
                            }
                        }
                    }
                }
            }
        }

        // One button. The four separate tracks are an implementation detail the
        // musician only meets afterwards, in the folder.
        JamCard {
            width: parent.width
            height: 68

            Row {
                anchors.fill: parent
                anchors.margins: 13
                spacing: 12

                Rectangle {
                    id: recordDot
                    anchors.verticalCenter: parent.verticalCenter
                    width: 15
                    height: 15
                    radius: 8
                    color: root.controller.recording ? "#e0473f" : "#3a444b"
                    SequentialAnimation on opacity {
                        running: root.controller.recording
                        loops: Animation.Infinite
                        NumberAnimation { from: 1.0; to: 0.35; duration: 700 }
                        NumberAnimation { from: 0.35; to: 1.0; duration: 700 }
                        onStopped: recordDot.opacity = 1.0
                    }
                }
                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - recordDot.width - recordButton.width - 36
                    spacing: 3
                    Text {
                        text: root.controller.recording
                            ? "Recording " + root.controller.recordingElapsed
                            : "Record this jam"
                        color: root.controller.recording ? "#f2f4f5" : "#dfe3e5"
                        font.family: "Segoe UI"
                        font.pixelSize: 11
                        font.weight: Font.Medium
                    }
                    Text {
                        width: parent.width
                        text: root.controller.recordingMessage
                        color: "#78838a"
                        elide: Text.ElideRight
                        font.family: "Segoe UI"
                        font.pixelSize: 9
                    }
                }
                JamButton {
                    id: recordButton
                    anchors.verticalCenter: parent.verticalCenter
                    width: 92
                    height: 32
                    text: root.controller.recording ? "Stop" : "Record"
                    primary: !root.controller.recording
                    enabled: root.controller.audioActive
                    Accessible.name: root.controller.recording
                        ? "Stop recording" : "Start recording"
                    onClicked: root.controller.toggleRecording()
                }
            }
        }

        JamCard {
            width: parent.width
            height: 84
            Row {
                anchors.fill: parent
                anchors.margins: 14
                spacing: 12
                JamIcon {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 22
                    height: 22
                    source: Qt.resolvedUrl("../../assets/headphones.svg")
                    color: "#8b56df"
                }
                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - 34 - monitorClip.width - 12
                    spacing: 4
                    Text {
                        text: root.controller.peerConnected
                            ? root.controller.connectionQuality
                            : "Direct encrypted UDP audio"
                        color: "#e7eaec"
                        wrapMode: Text.WordWrap
                        width: parent.width
                        font.family: "Segoe UI"
                        font.pixelSize: 11
                        font.weight: Font.Medium
                    }
                    Text {
                        text: root.controller.roomActive
                            ? root.controller.packetSummary
                            : "5 ms PCM packets · no relay fallback in this test build"
                        color: "#818c92"
                        width: parent.width
                        wrapMode: Text.WordWrap
                        font.family: "Segoe UI"
                        font.pixelSize: 9
                    }
                }
                ClipLatch {
                    id: monitorClip
                    anchors.verticalCenter: parent.verticalCenter
                    clipped: root.controller.outputClipped
                    Accessible.name: clipped
                        ? "Monitor mix clipping detected. Activate to reset."
                        : "Monitor mix clipping latch is clear"
                    onClicked: root.controller.clearOutputClipping()
                }
            }
        }
    }

    Rectangle {
        id: chatDrawer
        z: 20
        visible: root.chatOpen
        anchors.top: header.bottom
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: Math.min(370, parent.width - 20)
        color: "#0b1115"
        border.color: "#303b42"
        radius: 14

        Rectangle {
            id: chatHeader
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 52
            color: "transparent"
            Text {
                anchors.left: parent.left
                anchors.leftMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                text: "Room Chat"
                color: "#f2f4f5"
                font.family: "Segoe UI"
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
                color: "#202a30"
            }
        }

        ListView {
            id: chatList
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: chatHeader.bottom
            anchors.bottom: composer.top
            anchors.margins: 12
            spacing: 8
            clip: true
            model: root.controller.chatMessages
            delegate: Item {
                id: chatEntry
                required property var modelData
                width: ListView.view.width
                height: messageColumn.implicitHeight + 8
                Column {
                    id: messageColumn
                    width: parent.width
                    spacing: 3
                    Row {
                        visible: !chatEntry.modelData.system
                        width: parent.width
                        Text {
                            width: parent.width - messageTime.width
                            text: chatEntry.modelData.own
                                ? "You" : chatEntry.modelData.sender
                            color: chatEntry.modelData.own ? "#b993ff" : "#58db84"
                            font.family: "Segoe UI"
                            font.pixelSize: 10
                            font.weight: Font.DemiBold
                        }
                        Text {
                            id: messageTime
                            text: chatEntry.modelData.time
                            color: "#68747b"
                            font.family: "Segoe UI"
                            font.pixelSize: 8
                        }
                    }
                    TextArea {
                        width: parent.width
                        text: chatEntry.modelData.text
                        readOnly: true
                        selectByMouse: true
                        wrapMode: TextEdit.Wrap
                        color: chatEntry.modelData.system ? "#778289" : "#dce1e4"
                        font.family: "Segoe UI"
                        font.pixelSize: chatEntry.modelData.system ? 9 : 10
                        horizontalAlignment: chatEntry.modelData.system
                            ? Text.AlignHCenter : Text.AlignLeft
                        background: null
                        padding: 0
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
            anchors.margins: 12
            height: 62
            spacing: 8
            TextArea {
                id: chatInput
                width: parent.width - sendChatButton.width - 8
                height: 54
                placeholderText: "Message " + root.controller.remoteDisplayName
                wrapMode: TextEdit.Wrap
                color: "#edf0f2"
                placeholderTextColor: "#68747b"
                selectionColor: "#6938c5"
                font.family: "Segoe UI"
                font.pixelSize: 10
                background: Rectangle {
                    radius: 9
                    color: "#151e23"
                    border.color: chatInput.activeFocus ? "#8b56df" : "#2c373e"
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
                id: sendChatButton
                anchors.verticalCenter: parent.verticalCenter
                width: 72
                height: 34
                primary: true
                text: "Send"
                enabled: root.controller.peerConnected && chatInput.text.trim().length > 0
                Accessible.name: "Send room chat message"
                onClicked: {
                    if (root.controller.sendChatMessage(chatInput.text))
                        chatInput.clear()
                }
            }
        }
    }
}
