pragma ComponentBehavior: Bound
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls.Basic
import "pages"

ApplicationWindow {
    id: window
    required property AppController controller
    visible: true
    width: window.controller.preferredWindowWidth
    height: window.controller.preferredWindowHeight
    minimumWidth: 532
    minimumHeight: 480
    color: "transparent"
    flags: Qt.Window | Qt.FramelessWindowHint
    title: "JamLink"

    Component.onCompleted: {
        if (window.controller.hasPreferredWindowPosition) {
            x = window.controller.preferredWindowX
            y = window.controller.preferredWindowY
        }
    }

    onClosing: window.controller.updateWindowPlacement(x, y, width, height)

    background: Rectangle {
        color: "#070b0e"
        radius: 18
        border.color: "#303940"
        border.width: 1
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
        anchors.fill: parent
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
}
