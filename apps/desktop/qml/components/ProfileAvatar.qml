pragma ComponentBehavior: Bound
// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick

Item {
    id: root
    property string avatarId: "avatar:guitar-electric"
    property url customSource
    property color ringColor: "#8b56df"

    readonly property url builtInSource: {
        const names = {
            "avatar:guitar-electric": "avatar_guitar_electric.svg",
            "avatar:guitar-acoustic": "avatar_guitar_acoustic.svg",
            "avatar:bass": "avatar_bass.svg",
            "avatar:drums": "avatar_drums.svg",
            "avatar:keys": "avatar_keys.svg",
            "avatar:vocals": "avatar_vocals.svg",
            "avatar:synth": "avatar_synth.svg",
            "avatar:listener": "avatar_listener.svg"
        }
        return Qt.resolvedUrl("../../assets/" + (names[root.avatarId]
            || "avatar_guitar_electric.svg"))
    }

    Rectangle {
        anchors.fill: parent
        radius: width / 2
        color: "#10171c"
        border.color: root.ringColor
        border.width: 2
        clip: true
        Image {
            anchors.fill: parent
            anchors.margins: 3
            source: root.avatarId === "avatar:custom"
                && root.customSource.toString().length > 0
                ? root.customSource : root.builtInSource
            fillMode: Image.PreserveAspectCrop
            smooth: true
            mipmap: true
        }
    }
}
