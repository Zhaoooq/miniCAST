import QtQuick
import QtQuick.Controls
import MiniCastMonitor

Switch {
    id: control
    implicitWidth: 86; implicitHeight: 40
    spacing: 8; padding: 0
    indicator: Rectangle {
        x: 0; y: (control.height - height) / 2
        width: 38; height: 20; radius: 10
        color: control.checked ? Theme.primary : "#AEB9C0"
        Rectangle {
            width: 16; height: 16; radius: 8; y: 2
            x: control.checked ? parent.width - width - 2 : 2
            color: "white"
        }
    }
    contentItem: Text {
        leftPadding: 46
        text: control.text
        color: Theme.textPrimary
        font.family: Theme.fontFamily; font.pixelSize: 13
        verticalAlignment: Text.AlignVCenter
    }
}
