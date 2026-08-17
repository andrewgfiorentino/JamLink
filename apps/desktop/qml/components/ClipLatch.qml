pragma ComponentBehavior: Bound
// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls.Basic

Button {
    id: root
    property bool clipped: false
    property bool testable: false

    implicitWidth: 66
    implicitHeight: 24
    text: clipped ? "● CLIP" : (testable ? "○ Test" : "○ Clean")
    Accessible.name: clipped
        ? "Clipping detected. Activate to reset the clip indicator."
        : testable ? "Test the clipping indicator" : "No clipping detected"
    Accessible.description: clipped
        ? "The warning remains until reset and will return immediately if clipping continues."
        : testable
            ? "Runs a silent indicator test. No test sound is monitored, recorded, or sent."
            : "The clipping latch is clear."

    contentItem: Text {
        text: root.text
        color: root.clipped ? "#ff6d63" : "#748087"
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        font.family: "Segoe UI"
        font.pixelSize: 9
        font.weight: root.clipped ? Font.Bold : Font.Medium
    }
    background: Rectangle {
        radius: 7
        color: root.clipped
            ? (root.down ? "#6a1c19" : "#401715")
            : (root.hovered ? "#141d22" : "transparent")
        border.width: 1
        border.color: root.clipped ? "#ff5148" : "#334047"
    }
}
