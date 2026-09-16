import QtQuick
import MiniCastMonitor

Rectangle {
    property string text: ""
    implicitHeight: 32
    color: Theme.tableHeader
    Text {
        anchors.fill: parent; anchors.leftMargin: 3; anchors.rightMargin: 3
        text: parent.text; color: Theme.textPrimary
        font.family: Theme.fontFamily; font.pixelSize: 13; font.weight: Font.Medium
        horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
}
