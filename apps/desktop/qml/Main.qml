pragma ComponentBehavior: Bound
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls.Basic
import "components"
import "pages"

ApplicationWindow {
    id: window
    required property AppController controller
    readonly property int chromeHeight: 28
    property bool updatePromptDismissed: false
    visible: true
    width: window.controller.preferredWindowWidth
    height: window.controller.preferredWindowHeight + chromeHeight
    minimumWidth: 532
    minimumHeight: 480 + chromeHeight
    color: "transparent"
    flags: Qt.Window | Qt.FramelessWindowHint
    title: "JamLink"

    Component.onCompleted: {
        if (window.controller.hasPreferredWindowPosition) {
            x = window.controller.preferredWindowX
            y = window.controller.preferredWindowY
        }
    }

    onClosing: window.controller.updateWindowPlacement(
        x, y, width, Math.max(480, height - chromeHeight))

    background: Rectangle {
        color: "#070b0e"
        radius: 18
        border.color: "#303940"
        border.width: 1
    }

    // A narrow custom chrome keeps the visual design while restoring the
    // ordinary desktop operations a frameless window otherwise loses.
    Rectangle {
        id: windowChrome
        z: 90
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.leftMargin: 1
        anchors.rightMargin: 1
        anchors.topMargin: 1
        height: window.chromeHeight - 1
        color: "#090e12"
        radius: 17

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: "#182127"
        }

        Rectangle {
            anchors.centerIn: parent
            width: 44
            height: 3
            radius: 2
            color: moveArea.containsMouse ? "#657078" : "#39444b"
        }

        MouseArea {
            id: moveArea
            anchors.left: parent.left
            anchors.right: closeButton.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton
            cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
            Accessible.name: "Move JamLink window"
            onPressed: mouse => {
                if (!window.startSystemMove())
                    mouse.accepted = false
            }
        }

        Rectangle {
            id: closeButton
            anchors.right: parent.right
            anchors.top: parent.top
            width: 34
            height: parent.height
            radius: 10
            color: closeArea.containsMouse ? "#b7333a" : "transparent"

            JamIcon {
                anchors.centerIn: parent
                width: 15
                height: 15
                source: Qt.resolvedUrl("../assets/close.svg")
                color: closeArea.containsMouse ? "#ffffff" : "#9ba5aa"
            }
            MouseArea {
                id: closeArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                Accessible.role: Accessible.Button
                Accessible.name: "Close JamLink"
                onClicked: window.close()
            }
        }
    }

    // Documented in Settings > Shortcuts. Anything listed there works here.
    Shortcut {
        sequences: ["T"]
        onActivated: {
            if (window.controller.currentPage === "tuner")
                window.controller.closeTuner()
            else
                window.controller.navigate("tuner")
        }
    }
    Shortcut {
        sequences: ["R"]
        onActivated: window.controller.toggleRecording()
    }
    Shortcut {
        sequences: ["M"]
        enabled: window.controller.roomActive
        onActivated: window.controller.sendMuted = !window.controller.sendMuted
    }
    Shortcut {
        sequences: [StandardKey.Preferences, "Ctrl+,"]
        onActivated: {
            if (window.controller.currentPage === "settings")
                window.controller.closeSettings()
            else
                window.controller.openSettings()
        }
    }
    Shortcut {
        sequences: ["Esc"]
        enabled: window.controller.currentPage !== "home"
        onActivated: {
            if (window.controller.currentPage === "tuner")
                window.controller.closeTuner()
            else if (window.controller.currentPage === "settings")
                window.controller.closeSettings()
            else
                window.controller.navigate("home")
        }
    }

    Loader {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.topMargin: window.chromeHeight
        sourceComponent: window.controller.currentPage === "soundcheck"
            ? soundCheckPage
            : window.controller.currentPage === "settings" ? settingsPage
            : window.controller.currentPage === "room" ? roomPage
            : window.controller.currentPage === "tuner" ? tunerPage
            : window.controller.currentPage === "profile" ? profilePage : homePage
    }

    Component { id: homePage; HomePage { controller: window.controller } }
    Component { id: roomPage; RoomPage { controller: window.controller } }
    Component { id: soundCheckPage; SoundCheckPage { controller: window.controller } }
    Component { id: settingsPage; SettingsPage { controller: window.controller } }
    Component { id: tunerPage; TunerPage { controller: window.controller } }
    Component { id: profilePage; ProfilePage { controller: window.controller } }

    Connections {
        target: window.controller
        function onUpdateChanged() {
            if (window.controller.updateAvailable)
                window.updatePromptDismissed = false
        }
    }

    Item {
        id: updatePrompt
        z: 180
        anchors.fill: parent
        visible: window.controller.updateAvailable
            && (!window.updatePromptDismissed || window.controller.updateBusy)

        Rectangle {
            anchors.fill: parent
            color: "#d9070a0d"
        }

        JamCard {
            anchors.centerIn: parent
            width: Math.min(430, parent.width - 48)
            height: window.controller.updateBusy ? 270 : 246
            radius: 18
            border.color: "#4b3a65"

            Column {
                anchors.fill: parent
                anchors.margins: 24
                spacing: 14

                Row {
                    width: parent.width
                    spacing: 13
                    Rectangle {
                        width: 42
                        height: 42
                        radius: 13
                        color: "#51269f"
                        JamIcon {
                            anchors.centerIn: parent
                            width: 21
                            height: 21
                            source: Qt.resolvedUrl("../assets/check_circle.svg")
                            color: "#ffffff"
                        }
                    }
                    Column {
                        width: parent.width - 55
                        spacing: 3
                        Text {
                            text: "A JamLink update is ready"
                            color: "#f5f6f7"
                            font.family: "Segoe UI"
                            font.pixelSize: 18
                            font.weight: Font.DemiBold
                        }
                        Text {
                            text: "Update both musicians before joining"
                            color: "#9aa5ab"
                            font.family: "Segoe UI"
                            font.pixelSize: 11
                        }
                    }
                }

                Text {
                    width: parent.width
                    text: window.controller.updateStatus
                    color: "#d7dde0"
                    wrapMode: Text.WordWrap
                    font.family: "Segoe UI"
                    font.pixelSize: 12
                }

                Rectangle {
                    visible: window.controller.updateBusy
                    width: parent.width
                    height: 7
                    radius: 4
                    color: "#202a31"
                    Rectangle {
                        width: Math.max(7, parent.width * window.controller.updateProgress)
                        height: parent.height
                        radius: 4
                        color: "#8e4dde"
                        Behavior on width { NumberAnimation { duration: 120 } }
                    }
                }

                JamButton {
                    width: parent.width
                    height: 40
                    primary: true
                    enabled: !window.controller.updateBusy && !window.controller.roomActive
                    text: window.controller.roomActive ? "Leave room to update"
                        : window.controller.updateBusy ? "Updating…" : "Update & Restart"
                    Accessible.name: text
                    onClicked: window.controller.installUpdate()
                }
                JamButton {
                    visible: !window.controller.updateBusy
                    width: parent.width
                    height: 34
                    text: "Not now"
                    Accessible.name: "Continue without updating"
                    onClicked: window.updatePromptDismissed = true
                }
            }
        }
    }

    component ResizeHandle: MouseArea {
        required property int edges
        acceptedButtons: Qt.LeftButton
        onPressed: mouse => {
            if (!window.startSystemResize(edges))
                mouse.accepted = false
        }
    }

    ResizeHandle {
        z: 100
        edges: Qt.LeftEdge
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 6
        cursorShape: Qt.SizeHorCursor
    }
    ResizeHandle {
        z: 100
        edges: Qt.RightEdge
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 6
        cursorShape: Qt.SizeHorCursor
    }
    ResizeHandle {
        z: 100
        edges: Qt.TopEdge
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 6
        cursorShape: Qt.SizeVerCursor
    }
    ResizeHandle {
        z: 100
        edges: Qt.BottomEdge
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 6
        cursorShape: Qt.SizeVerCursor
    }
    ResizeHandle {
        z: 101
        edges: Qt.LeftEdge | Qt.TopEdge
        anchors.left: parent.left
        anchors.top: parent.top
        width: 10
        height: 10
        cursorShape: Qt.SizeFDiagCursor
    }
    ResizeHandle {
        z: 101
        edges: Qt.RightEdge | Qt.TopEdge
        anchors.right: parent.right
        anchors.top: parent.top
        width: 10
        height: 10
        cursorShape: Qt.SizeBDiagCursor
    }
    ResizeHandle {
        z: 101
        edges: Qt.LeftEdge | Qt.BottomEdge
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        width: 10
        height: 10
        cursorShape: Qt.SizeBDiagCursor
    }
    ResizeHandle {
        z: 101
        edges: Qt.RightEdge | Qt.BottomEdge
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: 10
        height: 10
        cursorShape: Qt.SizeFDiagCursor
    }
}
