pragma Singleton
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick

QtObject {
    readonly property color background: "#070b0e"
    readonly property color surface: "#10161b"
    readonly property color surfaceRaised: "#141c22"
    readonly property color surfaceNested: "#0b1115"
    readonly property color hover: "#1a242b"
    readonly property color border: "#273139"
    readonly property color borderSoft: "#1b252c"

    readonly property color text: "#f2f4f5"
    readonly property color textSecondary: "#a2abb1"
    readonly property color textMuted: "#707b82"

    readonly property color accent: "#7c3fd0"
    readonly property color accentBright: "#9c5ae6"
    readonly property color connected: "#38d565"
    readonly property color warning: "#e0b542"
    readonly property color error: "#f05b53"
    readonly property color recording: "#ef5148"

    readonly property string fontFamily: "Segoe UI"
    readonly property string numericFontFamily: "Cascadia Mono"
    readonly property int radiusSmall: 8
    readonly property int radiusControl: 10
    readonly property int radiusCard: 14
    readonly property int radiusPanel: 18
    readonly property int pagePadding: 22
}
