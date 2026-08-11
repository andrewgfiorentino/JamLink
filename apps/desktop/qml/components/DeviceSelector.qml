pragma ComponentBehavior: Bound
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls.Basic

ComboBox {
    id: control
    implicitHeight: 34
    leftPadding: 11
    rightPadding: 28
    font.family: "Segoe UI"
    font.pixelSize: 10

    indicator: JamIcon {
        x: control.width - width - 9
        y: control.height / 2 - height / 2
        width: 15
        height: 15
        source: Qt.resolvedUrl("../../assets/expand_more.svg")
        color: control.enabled ? "#cdd3d7" : "#667078"
    }

    background: Rectangle {
        radius: 9
        color: control.enabled ? (control.down ? "#202930" : "#192127") : "#151b20"
        border.color: control.activeFocus ? "#8b56df" : "#202a30"
    }

    contentItem: Text {
        leftPadding: 0
        rightPadding: 0
        text: control.displayText
        font: control.font
        color: control.enabled ? "#e8ebed" : "#747d83"
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    popup: Popup {
        y: control.height + 4
        width: control.width
        implicitHeight: contentItem.implicitHeight + 8
        padding: 4
        background: Rectangle {
            color: "#151c21"
            border.color: "#344049"
            radius: 10
        }
        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            ScrollIndicator.vertical: ScrollIndicator {}
        }
    }

    delegate: ItemDelegate {
        id: delegateItem
        required property int index
        required property var modelData
        width: control.width - 8
        height: 32
        highlighted: control.highlightedIndex === delegateItem.index
        contentItem: Text {
            text: delegateItem.modelData
            color: "#edf0f2"
            font: control.font
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        background: Rectangle {
            radius: 7
            color: delegateItem.highlighted ? "#29343c" : "transparent"
        }
    }
}
