pragma ComponentBehavior: Bound
// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Layouts
import "../components"

Item {
    id: root
    required property AppController controller

    function linearGainText(gain, enabled) {
        if (!root.controller.audioActive)
            return "N/A"
        if (!enabled || gain <= 0.000001)
            return "Muted"
        return (20 * Math.log(gain) / Math.LN10).toFixed(1) + " dB"
    }

    function levelText(level) {
        if (!root.controller.audioActive)
            return "N/A"
        if (level <= 0.000001)
            return "No signal"
        return (20 * Math.log(level) / Math.LN10).toFixed(1) + " dBFS"
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 62
        color: "transparent"

        IconButton {
            anchors.left: parent.left
            anchors.leftMargin: 15
            anchors.verticalCenter: parent.verticalCenter
            iconSource: Qt.resolvedUrl("../../assets/arrow_back.svg")
            Accessible.name: "Back to setup overview"
            onClicked: root.controller.navigate("home")
        }

        Column {
            anchors.centerIn: parent
            spacing: 3
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Sound Check"
                color: "#f4f5f6"
                font.family: "Segoe UI"
                font.pixelSize: 17
                font.weight: Font.DemiBold
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Only you can hear this"
                color: "#929aa0"
                font.family: "Segoe UI"
                font.pixelSize: 11
            }
        }
    }

    RowLayout {
        id: sourceCards
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.topMargin: 80
        anchors.leftMargin: 24
        anchors.rightMargin: 24
        height: 230
        spacing: 12

        JamCard {
            Layout.fillWidth: true
            Layout.fillHeight: true

            Column {
                anchors.fill: parent
                anchors.margins: 14
                spacing: 8

                Text {
                    text: "GUITAR"
                    color: "#f1f2f3"
                    font.family: "Segoe UI"
                    font.pixelSize: 10
                    font.weight: Font.Medium
                }
                DeviceSelector {
                    width: parent.width
                    model: root.controller.instrumentDevices
                    currentIndex: root.controller.instrumentDeviceIndex
                    enabled: root.controller.devicesAvailable
                    Accessible.name: "Guitar input device"
                    onActivated: index => root.controller.instrumentDeviceIndex = index
                }
                LevelBar {
                    width: parent.width
                    height: 18
                    level: root.controller.instrumentLevel
                    peakHold: root.controller.instrumentPeakHold
                    clipped: root.controller.instrumentInputClipped
                        || root.controller.instrumentSendClipped
                    resettable: true
                    onResetRequested: root.controller.clearInstrumentClipping()
                }
                Row {
                    width: parent.width
                    height: 24
                    Column {
                        width: parent.width - instrumentClip.width
                        spacing: 1
                        Text {
                            text: "Current " + root.levelText(root.controller.instrumentLevel)
                            color: "#aeb7bc"
                            font.family: "Segoe UI"
                            font.pixelSize: 8
                        }
                        Text {
                            text: "Peak " + root.levelText(root.controller.instrumentPeakHold)
                            color: "#dce1e3"
                            font.family: "Segoe UI"
                            font.pixelSize: 8
                        }
                    }
                    ClipLatch {
                        id: instrumentClip
                        clipped: root.controller.instrumentInputClipped
                            || root.controller.instrumentSendClipped
                        testable: root.controller.audioActive
                        onClicked: {
                            if (clipped)
                                root.controller.clearInstrumentClipping()
                            else
                                root.controller.testInstrumentClipping()
                        }
                    }
                }
                Text {
                    width: parent.width
                    text: root.controller.instrumentSignalStatus + " · "
                        + root.controller.instrumentSignalGuidance
                    color: root.controller.instrumentInputClipped
                        || root.controller.instrumentSendClipped ? "#ff746b" : "#919da3"
                    elide: Text.ElideRight
                    font.family: "Segoe UI"
                    font.pixelSize: 8
                }
                Row {
                    width: parent.width
                    spacing: 8
                    JamSlider {
                        width: parent.width - gainText.width - 8
                        value: root.controller.instrumentMonitorGain
                        enabled: root.controller.devicesAvailable
                        Accessible.name: "Guitar monitor level"
                        onMoved: root.controller.instrumentMonitorGain = value
                    }
                    Text {
                        id: gainText
                        anchors.verticalCenter: parent.verticalCenter
                        width: 44
                        horizontalAlignment: Text.AlignRight
                        text: root.linearGainText(
                            root.controller.instrumentMonitorGain,
                            root.controller.instrumentMonitorEnabled)
                        color: "#d4d9dc"
                        font.family: "Segoe UI"
                        font.pixelSize: 10
                    }
                }
                Row {
                    width: parent.width
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - instrumentSwitch.width
                        text: "Monitor"
                        color: "#d8dde0"
                        font.family: "Segoe UI"
                        font.pixelSize: 11
                    }
                    JamSwitch {
                        id: instrumentSwitch
                        checked: root.controller.instrumentMonitorEnabled
                        enabled: root.controller.devicesAvailable
                        Accessible.name: "Monitor guitar locally"
                        onToggled: root.controller.instrumentMonitorEnabled = checked
                    }
                }
            }
        }

        JamCard {
            Layout.fillWidth: true
            Layout.fillHeight: true

            Column {
                anchors.fill: parent
                anchors.margins: 14
                spacing: 8

                Text {
                    text: "MICROPHONE"
                    color: "#f1f2f3"
                    font.family: "Segoe UI"
                    font.pixelSize: 10
                    font.weight: Font.Medium
                }
                DeviceSelector {
                    width: parent.width
                    model: root.controller.voiceDevices
                    currentIndex: root.controller.voiceDeviceIndex
                    enabled: root.controller.devicesAvailable
                    Accessible.name: "Microphone input device"
                    onActivated: index => root.controller.voiceDeviceIndex = index
                }
                LevelBar {
                    width: parent.width
                    height: 18
                    level: root.controller.voiceLevel
                    peakHold: root.controller.voicePeakHold
                    clipped: root.controller.voiceInputClipped
                        || root.controller.voiceSendClipped
                    resettable: true
                    onResetRequested: root.controller.clearVoiceClipping()
                }
                Row {
                    width: parent.width
                    height: 24
                    Column {
                        width: parent.width - voiceClip.width
                        spacing: 1
                        Text {
                            text: "Current " + root.levelText(root.controller.voiceLevel)
                            color: "#aeb7bc"
                            font.family: "Segoe UI"
                            font.pixelSize: 8
                        }
                        Text {
                            text: "Peak " + root.levelText(root.controller.voicePeakHold)
                            color: "#dce1e3"
                            font.family: "Segoe UI"
                            font.pixelSize: 8
                        }
                    }
                    ClipLatch {
                        id: voiceClip
                        clipped: root.controller.voiceInputClipped
                            || root.controller.voiceSendClipped
                        testable: root.controller.audioActive
                        onClicked: {
                            if (clipped)
                                root.controller.clearVoiceClipping()
                            else
                                root.controller.testVoiceClipping()
                        }
                    }
                }
                Text {
                    width: parent.width
                    text: root.controller.voiceSignalStatus + " · "
                        + root.controller.voiceSignalGuidance
                    color: root.controller.voiceInputClipped
                        || root.controller.voiceSendClipped ? "#ff746b" : "#919da3"
                    elide: Text.ElideRight
                    font.family: "Segoe UI"
                    font.pixelSize: 8
                }
                Row {
                    width: parent.width
                    spacing: 8
                    JamSlider {
                        width: parent.width - voiceGainText.width - 8
                        value: root.controller.voiceMonitorGain
                        enabled: root.controller.devicesAvailable
                        Accessible.name: "Microphone monitor level"
                        onMoved: root.controller.voiceMonitorGain = value
                    }
                    Text {
                        id: voiceGainText
                        anchors.verticalCenter: parent.verticalCenter
                        width: 44
                        horizontalAlignment: Text.AlignRight
                        text: root.linearGainText(
                            root.controller.voiceMonitorGain,
                            root.controller.voiceMonitorEnabled)
                        color: "#d4d9dc"
                        font.family: "Segoe UI"
                        font.pixelSize: 10
                    }
                }
                Row {
                    width: parent.width
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - voiceSwitch.width
                        text: "Monitor"
                        color: "#d8dde0"
                        font.family: "Segoe UI"
                        font.pixelSize: 11
                    }
                    JamSwitch {
                        id: voiceSwitch
                        checked: root.controller.voiceMonitorEnabled
                        enabled: root.controller.devicesAvailable
                        Accessible.name: "Monitor microphone locally"
                        onToggled: root.controller.voiceMonitorEnabled = checked
                    }
                }
            }
        }
    }

    JamCard {
        id: outputCard
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: sourceCards.bottom
        anchors.topMargin: 12
        anchors.leftMargin: 24
        anchors.rightMargin: 24
        height: 124

        Text {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.margins: 14
            text: "OUTPUT"
            color: "#f1f2f3"
            font.family: "Segoe UI"
            font.pixelSize: 10
            font.weight: Font.Medium
        }
        DeviceSelector {
            id: outputSelector
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.leftMargin: 14
            anchors.topMargin: 30
            width: Math.min(220, parent.width * 0.47)
            model: root.controller.outputDevices
            currentIndex: root.controller.outputDeviceIndex
            enabled: root.controller.devicesAvailable
            Accessible.name: "Monitor output device"
            onActivated: index => root.controller.outputDeviceIndex = index
        }
        LevelBar {
            anchors.left: outputSelector.right
            anchors.right: parent.right
            anchors.top: outputSelector.top
            anchors.leftMargin: 18
            anchors.rightMargin: 14
            height: 18
            level: root.controller.outputLevel
            peakHold: root.controller.outputPeakHold
            clipped: root.controller.outputClipped
            resettable: true
            onResetRequested: root.controller.clearOutputClipping()
        }
        Rectangle {
            anchors.left: parent.left
            anchors.right: outputDb.left
            anchors.bottom: parent.bottom
            anchors.leftMargin: 14
            anchors.rightMargin: 10
            anchors.bottomMargin: 19
            height: 3
            radius: 2
            color: "#293239"
            Rectangle {
                width: parent.width * root.controller.outputLevel
                height: parent.height
                radius: parent.radius
                color: root.controller.outputClipped ? "#ef4c43" : "#31d052"
            }
        }
        Text {
            id: outputDb
            anchors.right: outputClip.left
            anchors.bottom: parent.bottom
            anchors.rightMargin: 8
            anchors.bottomMargin: 17
            width: 106
            text: "Peak " + root.levelText(root.controller.outputPeakHold)
            color: "#d4d9dc"
            horizontalAlignment: Text.AlignRight
            font.family: "Segoe UI"
            font.pixelSize: 10
        }
        ClipLatch {
            id: outputClip
            anchors.right: parent.right
            anchors.rightMargin: 14
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 13
            clipped: root.controller.outputClipped
            testable: root.controller.audioActive
            onClicked: {
                if (clipped)
                    root.controller.clearOutputClipping()
                else
                    root.controller.testOutputClipping()
            }
        }
        Text {
            anchors.left: outputSelector.left
            anchors.right: outputDb.left
            anchors.top: outputSelector.bottom
            anchors.topMargin: 7
            text: root.controller.outputSignalStatus + " · "
                + root.controller.outputSignalGuidance
            color: root.controller.outputClipped ? "#ff746b" : "#849097"
            elide: Text.ElideRight
            font.family: "Segoe UI"
            font.pixelSize: 8
        }
    }

    Row {
        id: statusRow
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: outputCard.bottom
        anchors.topMargin: 11
        anchors.leftMargin: 25
        anchors.rightMargin: 25
        height: 32
        spacing: 7
        JamIcon {
            anchors.verticalCenter: parent.verticalCenter
            width: 14
            height: 14
            source: Qt.resolvedUrl("../../assets/headphones.svg")
            color: root.controller.audioActive ? "#42d97a" : "#687178"
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: root.controller.setupMessage
            color: "#89949a"
            elide: Text.ElideRight
            width: parent.width - outputTest.width - 32
            font.family: "Segoe UI"
            font.pixelSize: 10
        }
        JamButton {
            id: outputTest
            width: 112
            height: 30
            text: root.controller.audioActive ? "Test Output" : "Retry Audio"
            iconSource: Qt.resolvedUrl("../../assets/volume_up.svg")
            enabled: true
            Accessible.name: root.controller.audioActive
                ? "Play quiet output test tone"
                : "Retry Windows audio devices"
            onClicked: {
                if (root.controller.audioActive)
                    root.controller.testOutput()
                else
                    root.controller.retryAudio()
            }
        }
    }

    JamButton {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: 24
        anchors.rightMargin: 24
        anchors.bottomMargin: 8
        height: 36
        primary: true
        enabled: root.controller.audioActive
        text: root.controller.allReady ? "Save Sound Check" : "Verify & Save Sound Check"
        iconSource: Qt.resolvedUrl("../../assets/headphones.svg")
        Accessible.name: "Save private sound check"
        onClicked: root.controller.saveSoundcheck()
    }
}
