pragma ComponentBehavior: Bound
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
                font.family: "Segoe UI Variable Display"
                font.pixelSize: 17
                font.weight: Font.DemiBold
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Only you can hear this"
                color: "#929aa0"
                font.family: "Segoe UI Variable Text"
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
        height: 228
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
                    font.family: "Segoe UI Variable Text"
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
                        font.family: "Segoe UI Variable Text"
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
                        font.family: "Segoe UI Variable Text"
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
                Rectangle { width: parent.width; height: 1; color: "#252e34" }
                Row {
                    width: parent.width
                    spacing: 7
                    JamIcon {
                        width: 15
                        height: 15
                        source: Qt.resolvedUrl("../../assets/check_circle.svg")
                        color: root.controller.audioActive ? "#35d75b" : "#687178"
                    }
                    Text {
                        text: root.controller.audioActive ? "Live input" : root.controller.audioStatus
                        color: root.controller.audioActive ? "#aeb9b1" : "#757e84"
                        elide: Text.ElideRight
                        width: parent.width - 22
                        font.family: "Segoe UI Variable Text"
                        font.pixelSize: 10
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
                    font.family: "Segoe UI Variable Text"
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
                        font.family: "Segoe UI Variable Text"
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
                        font.family: "Segoe UI Variable Text"
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
                Rectangle { width: parent.width; height: 1; color: "#252e34" }
                Row {
                    width: parent.width
                    spacing: 7
                    JamIcon {
                        width: 15
                        height: 15
                        source: Qt.resolvedUrl("../../assets/mic.svg")
                        color: root.controller.devicesAvailable ? "#d3d8dc" : "#687178"
                    }
                    Text {
                        text: root.controller.audioActive
                            ? (root.controller.voiceMonitorEnabled ? "Local monitor on" : "Local monitor muted")
                            : root.controller.audioStatus
                        color: "#aeb5ba"
                        elide: Text.ElideRight
                        width: parent.width - 22
                        font.family: "Segoe UI Variable Text"
                        font.pixelSize: 10
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
        height: 108

        Text {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.margins: 14
            text: "OUTPUT"
            color: "#f1f2f3"
            font.family: "Segoe UI Variable Text"
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
                color: "#31bdf4"
            }
        }
        Text {
            id: outputDb
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.rightMargin: 14
            anchors.bottomMargin: 16
            width: 60
            text: root.levelText(root.controller.outputLevel)
            color: "#d4d9dc"
            horizontalAlignment: Text.AlignRight
            font.family: "Segoe UI Variable Text"
            font.pixelSize: 10
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
            color: root.controller.audioActive ? "#42d7b3" : "#687178"
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: root.controller.setupMessage
            color: "#89949a"
            elide: Text.ElideRight
            width: parent.width - outputTest.width - 32
            font.family: "Segoe UI Variable Text"
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
