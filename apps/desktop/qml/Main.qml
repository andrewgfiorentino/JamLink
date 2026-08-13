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
        onActivated: window.controller.navigate(
            window.controller.currentPage === "tuner" ? "home" : "tuner")
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
        onActivated: window.controller.navigate("settings")
    }
    Shortcut {
        sequences: ["Esc"]
        enabled: window.controller.currentPage !== "home"
        onActivated: window.controller.navigate("home")
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
