import QtQuick
import MiniCastMonitor

Row {
    property string text: "正常"
    property color statusColor: Theme.green
    spacing: 6
    Rectangle {
        width: 9; height: 9; radius: 5
        anchors.verticalCenter: parent.verticalCenter
        color: statusColor
        border.color: statusColor
    }
    Text {
        text: parent.text
        color: Theme.textPrimary
        font.pixelSize: 14
        font.family: Theme.fontFamily
        anchors.verticalCenter: parent.verticalCenter
    }
}
