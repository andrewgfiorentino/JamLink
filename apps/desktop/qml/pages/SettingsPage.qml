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

        IconButton {
            anchors.right: parent.right
            anchors.rightMargin: 15
            anchors.verticalCenter: parent.verticalCenter
            iconSource: Qt.resolvedUrl("../../assets/close.svg")
            Accessible.name: "Close settings"
            onClicked: root.controller.navigate("home")
        }
        Text {
            anchors.left: parent.left
            anchors.leftMargin: 16
            anchors.verticalCenter: parent.verticalCenter
            text: "Settings"
            color: "#f2f4f5"
            font.family: "Segoe UI Variable Display"
            font.pixelSize: 17
            font.weight: Font.DemiBold
        }
    }

    Row {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        anchors.leftMargin: 14
        anchors.rightMargin: 14
        anchors.bottomMargin: 14
        spacing: 10

        Item {
            width: 132
            height: parent.height

            Column {
                anchors.fill: parent
                spacing: 6

                Rectangle {
                    width: parent.width
                    height: 34
                    radius: 5
                    color: "#5326a7"
                    Row {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        spacing: 8
                        JamIcon {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 15
                            height: 15
                            source: Qt.resolvedUrl("../../assets/volume_up.svg")
                            color: "#f0eafa"
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: "Audio"
                            color: "#f5f1fb"
                            font.family: "Segoe UI Variable Text"
                            font.pixelSize: 11
                            font.weight: Font.Medium
                        }
                    }
                }

                Repeater {
                    model: ["Devices", "Recording", "Network", "Shortcuts", "Appearance", "Advanced", "About"]
                    Item {
                        id: navigationItem
                        required property string modelData
                        width: parent.width
                        height: 29
                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 10
                            anchors.verticalCenter: parent.verticalCenter
                            text: navigationItem.modelData
                            color: "#69747b"
                            font.family: "Segoe UI Variable Text"
                            font.pixelSize: 10
                        }
                        Text {
                            anchors.right: parent.right
                            anchors.rightMargin: 6
                            anchors.verticalCenter: parent.verticalCenter
                            text: "later"
                            color: "#4f5960"
                            font.family: "Segoe UI Variable Text"
                            font.pixelSize: 8
                        }
                    }
                }
            }

            Rectangle {
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.right: parent.right
                width: 1
                color: "#202a30"
            }
        }

        JamCard {
            width: parent.width - 142
            height: parent.height

            Column {
                anchors.fill: parent
                anchors.margins: 15
                spacing: 12

                Text {
                    text: "Audio Devices"
                    color: "#eef0f2"
                    font.family: "Segoe UI Variable Text"
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                }

                Repeater {
                    model: [
                        {label: "Guitar input", values: root.controller.instrumentDevices, index: root.controller.instrumentDeviceIndex, kind: 0},
                        {label: "Microphone", values: root.controller.voiceDevices, index: root.controller.voiceDeviceIndex, kind: 1},
                        {label: "Output", values: root.controller.outputDevices, index: root.controller.outputDeviceIndex, kind: 2}
                    ]
                    Column {
                        id: deviceRow
                        required property var modelData
                        width: parent.width
                        spacing: 5
                        Text {
                            text: deviceRow.modelData.label
                            color: "#dfe3e5"
                            font.family: "Segoe UI Variable Text"
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

                Rectangle { width: parent.width; height: 1; color: "#273138" }

                Row {
                    width: parent.width
                    height: 34
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 84
                        text: "Sample rate"
                        color: "#dfe3e5"
                        font.family: "Segoe UI Variable Text"
                        font.pixelSize: 10
                    }
                    DeviceSelector {
                        width: parent.width - 84
                        model: root.controller.sampleRates
                        currentIndex: root.controller.sampleRateIndex
                        enabled: root.controller.devicesAvailable
                        Accessible.name: "Sample rate"
                    onActivated: index => root.controller.sampleRateIndex = index
                    }
                }
                Row {
                    width: parent.width
                    height: 34
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 84
                        text: "Buffer size"
                        color: "#dfe3e5"
                        font.family: "Segoe UI Variable Text"
                        font.pixelSize: 10
                    }
                    DeviceSelector {
                        width: parent.width - 84
                        model: root.controller.bufferSizes
                        currentIndex: root.controller.bufferSizeIndex
                        enabled: root.controller.devicesAvailable
                        Accessible.name: "Buffer size"
                    onActivated: index => root.controller.bufferSizeIndex = index
                    }
                }
                Rectangle { width: parent.width; height: 1; color: "#273138" }
                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: 7
                    JamIcon {
                        width: 15
                        height: 15
                        source: Qt.resolvedUrl("../../assets/check_circle.svg")
                        color: root.controller.devicesAvailable ? "#38d55b" : "#747c82"
                    }
                    Text {
                        text: root.controller.devicesAvailable ? "Selections available" : "Backend unavailable"
                        color: root.controller.devicesAvailable ? "#47d969" : "#889198"
                        font.family: "Segoe UI Variable Text"
                        font.pixelSize: 10
                    }
                }
            }
        }
    }
}
