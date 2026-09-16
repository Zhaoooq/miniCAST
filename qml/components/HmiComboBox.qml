import QtQuick
import QtQuick.Controls
import MiniCastMonitor

ComboBox {
    id: control
    implicitHeight: 40
    implicitWidth: 156
    leftPadding: 10; rightPadding: 30; topPadding: 0; bottomPadding: 0
    font.family: Theme.fontFamily; font.pixelSize: 13

    contentItem: Text {
        leftPadding: 0; rightPadding: 0
        text: control.displayText
        color: control.enabled ? Theme.textPrimary : "#9AA6AE"
        font: control.font
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    indicator: Canvas {
        x: control.width - width - 9; y: (control.height - height) / 2
        width: 12; height: 8
        onPaint: {
            var c=getContext("2d"); c.reset(); c.strokeStyle="#52636E"; c.lineWidth=1.5; c.lineCap="round"
            c.beginPath(); c.moveTo(2,2); c.lineTo(6,6); c.lineTo(10,2); c.stroke()
        }
    }
    background: Rectangle {
        color: control.pressed ? "#F0F4F6" : "white"
        border.width: 1; border.color: control.activeFocus ? Theme.primary : Theme.border
        radius: 2
    }
    delegate: ItemDelegate {
        required property var modelData
        required property int index
        width: control.width; height: 38
        highlighted: control.highlightedIndex === index
        contentItem: Text {
            text: control.textRole ? modelData[control.textRole] : modelData
            color: Theme.textPrimary; font: control.font
            verticalAlignment: Text.AlignVCenter; leftPadding: 8; elide: Text.ElideRight
        }
        background: Rectangle { color: parent.highlighted ? Theme.primarySelection : "white" }
    }
    popup: Popup {
        y: control.height + 1; width: control.width
        implicitHeight: Math.min(contentItem.implicitHeight + 2, 210)
        padding: 1
        contentItem: ListView {
            clip: true; implicitHeight: contentHeight
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            ScrollIndicator.vertical: ScrollIndicator {}
        }
        background: Rectangle { color: "white"; border.color: Theme.border; radius: 2 }
    }
}
