pragma ComponentBehavior: Bound
// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Dialogs
import "../components"

Item {
    id: root
    required property AppController controller

    // Every section here is backed by something real. A category that could
    // only show a placeholder does not appear at all.
    property int section: 0
    readonly property var sections: [
        {label: "Audio", icon: "volume_up.svg"},
        {label: "Recording", icon: "music_note.svg"},
        {label: "Network", icon: "headphones.svg"},
        {label: "Shortcuts", icon: "tune.svg"},
        {label: "About", icon: "check_circle.svg"}
    ]

    Rectangle {
        id: header
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 58
        color: "transparent"

        Text {
            anchors.left: parent.left
            anchors.leftMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            text: "Settings"
            color: "#f2f4f5"
            font.family: "Segoe UI"
            font.pixelSize: 17
            font.weight: Font.DemiBold
        }
        IconButton {
            anchors.right: parent.right
            anchors.rightMargin: 15
            anchors.verticalCenter: parent.verticalCenter
            iconSource: Qt.resolvedUrl("../../assets/close.svg")
            Accessible.name: "Close settings"
            onClicked: root.controller.closeSettings()
        }
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: 18
            anchors.rightMargin: 18
            height: 1
            color: "#1d272d"
        }
    }

    Row {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        anchors.margins: 16
        spacing: 12

        Column {
            id: sidebar
            width: 124
            spacing: 4

            Repeater {
                model: root.sections
                Rectangle {
                    id: navigationItem
                    required property int index
                    required property var modelData
                    width: sidebar.width
                    height: 33
                    radius: 9
                    color: root.section === navigationItem.index ? "#5326a7" : "transparent"

                    Row {
                        anchors.fill: parent
                        anchors.leftMargin: 11
                        spacing: 9
                        JamIcon {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 14
                            height: 14
                            source: Qt.resolvedUrl("../../assets/" + navigationItem.modelData.icon)
                            color: root.section === navigationItem.index ? "#f0eafa" : "#69747b"
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: navigationItem.modelData.label
                            color: root.section === navigationItem.index ? "#f5f1fb" : "#8c959b"
                            font.family: "Segoe UI"
                            font.pixelSize: 11
                            font.weight: root.section === navigationItem.index
                                ? Font.Medium : Font.Normal
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.section = navigationItem.index
                    }
                }
            }
        }

        JamCard {
            width: parent.width - sidebar.width - 12
            height: parent.height

            Flickable {
                anchors.fill: parent
                anchors.margins: 16
                contentHeight: pane.height
                clip: true
                boundsBehavior: Flickable.StopAtBounds

                Column {
                    id: pane
                    width: parent.width
                    spacing: 14

                    // Settings is reachable from inside a room, so it has to
                    // say which changes are free and which cost a short
                    // silence. Swapping the audio device does not end the
                    // session, and a musician who does not know that will not
                    // risk it while a friend is waiting.
                    Rectangle {
                        visible: root.controller.settingsSessionNotice !== ""
                        width: pane.width
                        height: visible ? sessionNotice.implicitHeight + 20 : 0
                        radius: 9
                        color: "#161f26"
                        border.color: "#2c3a44"
                        Text {
                            id: sessionNotice
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.leftMargin: 11
                            anchors.rightMargin: 11
                            text: root.controller.settingsSessionNotice
                            color: "#a8b3ba"
                            wrapMode: Text.WordWrap
                            font.family: "Segoe UI"
                            font.pixelSize: 10
                            lineHeight: 1.25
                        }
                    }

                    // ---------------------------------------------- Audio
                    Column {
                        visible: root.section === 0
                        width: parent.width
                        spacing: 12

                        // Three devices that each work can still be unable to
                        // run together, and a musician has no way to know
                        // which one is wrong. A field test lost an evening to
                        // this: the report said "unsupported Windows format",
                        // which was true of none of them. It sits above the
                        // pickers because it is about them, and it offers the
                        // single change rather than describing the problem.
                        Rectangle {
                            id: setupBlockedBanner
                            visible: root.controller.audioSetupBlocked
                            width: pane.width
                            height: visible ? blockedContent.implicitHeight + 22 : 0
                            radius: 9
                            color: "#1c1710"
                            border.color: "#7a4b12"

                            Column {
                                id: blockedContent
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.leftMargin: 12
                                anchors.rightMargin: 12
                                spacing: 8

                                Text {
                                    width: parent.width
                                    text: root.controller.audioSetupAdvice
                                    color: "#e8dcc4"
                                    wrapMode: Text.WordWrap
                                    font.family: "Segoe UI"
                                    font.pixelSize: 10
                                    lineHeight: 1.25
                                }
                                JamButton {
                                    // Named after the device, never after the
                                    // audio system it happens to use.
                                    visible: root.controller.audioSetupFixAvailable
                                    height: 32
                                    width: parent.width
                                    primary: true
                                    font.pixelSize: 11
                                    text: root.controller.audioSetupFixLabel
                                    Accessible.name: root.controller.audioSetupFixLabel
                                    onClicked: root.controller.applyAudioSetupFix()
                                }
                                Text {
                                    width: parent.width
                                    visible: root.controller.audioSetupFixDetail !== ""
                                    text: root.controller.audioSetupFixDetail
                                    color: "#a08e6f"
                                    wrapMode: Text.WordWrap
                                    font.family: "Segoe UI"
                                    font.pixelSize: 9
                                }
                            }
                        }

                        SettingsHeading { text: "Audio devices" }

                        Repeater {
                            model: [
                                {label: "Guitar input", values: root.controller.instrumentDevices, index: root.controller.instrumentDeviceIndex, kind: 0},
                                {label: "Microphone", values: root.controller.voiceDevices, index: root.controller.voiceDeviceIndex, kind: 1},
                                {label: "Output", values: root.controller.outputDevices, index: root.controller.outputDeviceIndex, kind: 2}
                            ]
                            Column {
                                id: deviceRow
                                required property var modelData
                                width: pane.width
                                spacing: 5
                                Text {
                                    text: deviceRow.modelData.label
                                    color: "#dfe3e5"
                                    font.family: "Segoe UI"
                                    font.pixelSize: 10
                                }
                                DeviceSelector {
                                    width: parent.width
                                    model: deviceRow.modelData.values
                                    currentIndex: deviceRow.modelData.index
                                    enabled: root.controller.devicesAvailable
                                    Accessible.name: deviceRow.modelData.label
                                    onActivated: index => {
                                        if (deviceRow.modelData.kind === 0)
                                            root.controller.instrumentDeviceIndex = index
                                        else if (deviceRow.modelData.kind === 1)
                                            root.controller.voiceDeviceIndex = index
                                        else
                                            root.controller.outputDeviceIndex = index
                                    }
                                }
                            }
                        }

                        SettingsRule {}
                        SettingsHeading { text: "Format" }

                        SettingsRow {
                            label: "Buffer size"
                            DeviceSelector {
                                width: 210
                                model: root.controller.bufferSizes
                                currentIndex: root.controller.bufferSizeIndex
                                enabled: root.controller.devicesAvailable
                                Accessible.name: "Buffer size"
                                onActivated: index => root.controller.bufferSizeIndex = index
                            }
                        }
                        SettingsNote { text: root.controller.bufferSizeExplanation }
                        SettingsNote {
                            text: "Smaller means you hear yourself sooner. Too small for "
                                + "your computer and the sound breaks into crackles. Start "
                                + "low, and go up one step if that happens."
                        }

                        SettingsRow {
                            label: "Sample rate"
                            DeviceSelector {
                                width: 210
                                model: root.controller.sampleRates
                                currentIndex: root.controller.sampleRateIndex
                                enabled: root.controller.devicesAvailable
                                    && root.controller.sampleRates.length > 1
                                Accessible.name: "Sample rate"
                                onActivated: index => root.controller.sampleRateIndex = index
                            }
                        }
                        SettingsNote { text: root.controller.sampleRateExplanation }

                        SettingsRule {}
                        SettingsHeading { text: "Hearing yourself" }
                        SettingsNote { text: root.controller.monitorPathSummary }

                        SettingsRule {}
                        Row {
                            spacing: 7
                            JamIcon {
                                anchors.verticalCenter: parent.verticalCenter
                                width: 15
                                height: 15
                                source: Qt.resolvedUrl("../../assets/check_circle.svg")
                                color: root.controller.audioActive ? "#38d55b" : "#747c82"
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: root.controller.audioStatus
                                color: root.controller.audioActive ? "#47d969" : "#889198"
                                font.family: "Segoe UI"
                                font.pixelSize: 10
                            }
                        }
                    }

                    // ------------------------------------------ Recording
                    Column {
                        visible: root.section === 1
                        width: parent.width
                        spacing: 12

                        SettingsHeading { text: "Where takes are saved" }

                        Rectangle {
                            width: parent.width
                            height: 40
                            radius: 9
                            color: "#0e151a"
                            border.color: "#28343b"
                            Text {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.margins: 11
                                anchors.verticalCenter: parent.verticalCenter
                                text: root.controller.recordingDirectory
                                color: "#cbd4d9"
                                elide: Text.ElideMiddle
                                font.family: "Cascadia Mono"
                                font.pixelSize: 9
                            }
                        }
                        Row {
                            spacing: 8
                            JamButton {
                                width: 108
                                height: 32
                                text: "Change folder"
                                Accessible.name: "Change the recording folder"
                                onClicked: folderDialog.open()
                            }
                            JamButton {
                                width: 100
                                height: 32
                                text: "Open folder"
                                Accessible.name: "Open the recording folder"
                                onClicked: root.controller.openRecordingFolder()
                            }
                        }

                        SettingsRule {}
                        SettingsHeading { text: "What gets recorded" }
                        SettingsNote {
                            text: "Every take writes four 32-bit float WAV files, aligned to "
                                + "one timeline: your instrument, your voice, and each of "
                                + "your friend's two streams. They are separate so you can "
                                + "mix them afterwards."
                        }
                        SettingsNote {
                            text: "Your friend's tracks are what arrived over the network, "
                                + "including any concealment used to cover packet loss."
                        }
                    }

                    // -------------------------------------------- Network
                    Column {
                        visible: root.section === 2
                        width: parent.width
                        spacing: 12

                        SettingsHeading { text: "Latency" }
                        SettingsRow {
                            label: "Priority"
                            DeviceSelector {
                                width: 168
                                model: ["Lowest latency", "Balanced", "Most stable"]
                                currentIndex: root.controller.latencyMode
                                Accessible.name: "Latency priority"
                                onActivated: index => root.controller.latencyMode = index
                            }
                        }
                        SettingsNote { text: root.controller.latencyModeDetail }

                        SettingsRule {}
                        SettingsHeading { text: "Connection" }

                        Row {
                            width: parent.width
                            Column {
                                anchors.verticalCenter: parent.verticalCenter
                                width: parent.width - mappingSwitch.width - 14
                                spacing: 2
                                Text {
                                    text: "Ask the router to open the port"
                                    color: "#dfe3e5"
                                    font.family: "Segoe UI"
                                    font.pixelSize: 10
                                }
                                Text {
                                    text: "Uses UPnP. Turn off if your router refuses it or "
                                        + "you forward the port yourself."
                                    color: "#78838a"
                                    width: parent.width
                                    wrapMode: Text.WordWrap
                                    font.family: "Segoe UI"
                                    font.pixelSize: 9
                                }
                            }
                            JamSwitch {
                                id: mappingSwitch
                                anchors.verticalCenter: parent.verticalCenter
                                checked: root.controller.automaticRouterMapping
                                Accessible.name: "Automatic router port mapping"
                                onToggled: root.controller.automaticRouterMapping = checked
                            }
                        }

                        SettingsRow {
                            label: "UDP port"
                            Rectangle {
                                width: 168
                                height: 34
                                radius: 9
                                color: "#0e151a"
                                border.color: portField.activeFocus ? "#8b56df" : "#28343b"
                                TextInput {
                                    id: portField
                                    anchors.fill: parent
                                    anchors.leftMargin: 11
                                    anchors.rightMargin: 11
                                    verticalAlignment: Text.AlignVCenter
                                    color: "#e8ebed"
                                    selectionColor: "#6938c5"
                                    font.family: "Cascadia Mono"
                                    font.pixelSize: 10
                                    maximumLength: 5
                                    validator: IntValidator { bottom: 0; top: 65535 }
                                    text: root.controller.preferredUdpPort === 0
                                        ? "" : String(root.controller.preferredUdpPort)
                                    Accessible.name: "Preferred UDP port"
                                    onEditingFinished:
                                        root.controller.preferredUdpPort = parseInt(text || "0")
                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        visible: portField.text.length === 0
                                        text: "Automatic"
                                        color: "#5c666d"
                                        font: portField.font
                                    }
                                }
                            }
                        }
                        SettingsNote {
                            text: "Leave blank and JamLink takes any free port. Set one if "
                                + "you forward a specific port on your router. Applies to "
                                + "the next room you create."
                        }

                        SettingsRule {}
                        SettingsHeading { text: "Live diagnostics" }
                        Text {
                            width: parent.width
                            text: root.controller.networkDiagnostics
                            color: "#8c959b"
                            wrapMode: Text.WordWrap
                            lineHeight: 1.35
                            font.family: "Cascadia Mono"
                            font.pixelSize: 9
                        }
                    }

                    // ------------------------------------------ Shortcuts
                    Column {
                        visible: root.section === 3
                        width: parent.width
                        spacing: 10

                        SettingsHeading { text: "Keyboard" }
                        Repeater {
                            model: [
                                {keys: "T", action: "Open the tuner"},
                                {keys: "R", action: "Start or stop recording"},
                                {keys: "M", action: "Mute or unmute what you send"},
                                {keys: "Ctrl+,", action: "Open settings"},
                                {keys: "Esc", action: "Back to Home"}
                            ]
                            Row {
                                id: shortcutRow
                                required property var modelData
                                width: pane.width
                                height: 26
                                Rectangle {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 62
                                    height: 22
                                    radius: 6
                                    color: "#161e24"
                                    border.color: "#2b363d"
                                    Text {
                                        anchors.centerIn: parent
                                        text: shortcutRow.modelData.keys
                                        color: "#cbd4d9"
                                        font.family: "Cascadia Mono"
                                        font.pixelSize: 9
                                    }
                                }
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    leftPadding: 12
                                    text: shortcutRow.modelData.action
                                    color: "#b6bfc5"
                                    font.family: "Segoe UI"
                                    font.pixelSize: 10
                                }
                            }
                        }
                        SettingsNote {
                            text: "Shortcuts work on any screen except while you are typing "
                                + "in a field."
                        }
                    }

                    // ---------------------------------------------- About
                    Column {
                        visible: root.section === 4
                        width: parent.width
                        spacing: 12

                        SettingsHeading { text: "Support" }
                        Text {
                            width: pane.width
                            text: "If something goes wrong, this gathers what JamLink "
                                + "knows into one file and copies it to your clipboard. "
                                + "It contains no room secrets, keys, invites or chat."
                            wrapMode: Text.WordWrap
                            color: "#8c959b"
                            font.family: "Segoe UI"
                            font.pixelSize: 10
                        }
                        JamButton {
                            width: 190
                            height: 30
                            text: "Export Support Bundle"
                            onClicked: {
                                const written = root.controller.exportSupportBundle()
                                supportResult.text = written === ""
                                    ? "Could not write the bundle."
                                    : "Saved to " + written + " and copied."
                            }
                        }
                        Text {
                            id: supportResult
                            width: pane.width
                            text: ""
                            visible: text !== ""
                            wrapMode: Text.WordWrap
                            color: "#a8b3ba"
                            font.family: "Segoe UI"
                            font.pixelSize: 10
                        }

                        SettingsHeading { text: "JamLink" }
                        Repeater {
                            model: [
                                {label: "Version", value: root.controller.applicationVersion},
                                {label: "Qt", value: root.controller.qtVersion},
                                {label: "Licence", value: "GPL-3.0-or-later"}
                            ]
                            Row {
                                id: aboutRow
                                required property var modelData
                                width: pane.width
                                height: 22
                                Text {
                                    width: 86
                                    text: aboutRow.modelData.label
                                    color: "#78838a"
                                    font.family: "Segoe UI"
                                    font.pixelSize: 10
                                }
                                Text {
                                    text: aboutRow.modelData.value
                                    color: "#dfe3e5"
                                    font.family: "Cascadia Mono"
                                    font.pixelSize: 10
                                }
                            }
                        }

                        SettingsRule {}
                        SettingsHeading { text: "Updates" }
                        Text {
                            width: pane.width
                            text: root.controller.updateStatus
                            color: root.controller.updateAvailable
                                ? "#d7b7ff" : "#8f999f"
                            wrapMode: Text.WordWrap
                            font.family: "Segoe UI"
                            font.pixelSize: 10
                        }
                        Rectangle {
                            visible: root.controller.updateBusy
                                && root.controller.updateProgress > 0
                            width: pane.width
                            height: 4
                            radius: 2
                            color: "#202a31"
                            Rectangle {
                                width: parent.width * root.controller.updateProgress
                                height: parent.height
                                radius: 2
                                color: "#9355e8"
                            }
                        }
                        JamButton {
                            width: 142
                            height: 32
                            primary: root.controller.updateAvailable
                            text: root.controller.updateBusy
                                ? "Working…"
                                : root.controller.updateAvailable
                                    ? "Update Now"
                                    : "Check for Updates"
                            enabled: !root.controller.updateBusy
                                && (!root.controller.updateAvailable
                                    || !root.controller.roomActive)
                            Accessible.name: text
                            onClicked: {
                                if (root.controller.updateAvailable)
                                    root.controller.installUpdate()
                                else
                                    root.controller.checkForUpdates()
                            }
                        }
                        SettingsNote {
                            visible: root.controller.roomActive
                                && root.controller.updateAvailable
                            text: "Leave the room before installing so the update cannot compete with your music."
                        }

                        SettingsRule {}
                        SettingsNote {
                            text: "JamLink is free software. The exact source for this "
                                + "build ships beside the application, together with the "
                                + "full licence and third-party notices."
                        }
                    }
                }
            }
        }
    }

    FolderDialog {
        id: folderDialog
        title: "Choose where JamLink saves recordings"
        onAccepted: root.controller.recordingDirectory =
            selectedFolder.toString().replace(/^file:\/\/\//, "")
    }

    component SettingsHeading: Text {
        color: "#eef0f2"
        font.family: "Segoe UI"
        font.pixelSize: 12
        font.weight: Font.DemiBold
    }

    component SettingsRule: Rectangle {
        width: pane.width
        height: 1
        color: "#212b32"
    }

    component SettingsNote: Text {
        width: pane.width
        color: "#78838a"
        wrapMode: Text.WordWrap
        lineHeight: 1.3
        font.family: "Segoe UI"
        font.pixelSize: 9
    }

    component SettingsRow: Row {
        property alias label: rowLabel.text
        width: pane.width
        height: 34
        Text {
            id: rowLabel
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - 168
            color: "#dfe3e5"
            font.family: "Segoe UI"
            font.pixelSize: 10
        }
    }
}
