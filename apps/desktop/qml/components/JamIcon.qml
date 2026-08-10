// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Effects

Item {
    id: root
    property url source
    property color color: "#d8dde2"
    implicitWidth: 20
    implicitHeight: 20

    Image {
        id: image
        anchors.fill: parent
        source: root.source
        sourceSize.width: width * Screen.devicePixelRatio
        sourceSize.height: height * Screen.devicePixelRatio
        fillMode: Image.PreserveAspectFit
        visible: true
        mipmap: true
    }

    MultiEffect {
        anchors.fill: image
        source: image
        colorization: 1.0
        colorizationColor: root.color
    }
}
