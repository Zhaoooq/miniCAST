import QtQuick
import MiniCastMonitor

Row {
    id: root
    property string label: ""
    property string value: ""
    property color statusColor: Theme.green
    spacing: 5
    Rectangle { width: 9; height: 9; radius: 4.5; color: root.statusColor; anchors.verticalCenter: parent.verticalCenter }
    Text { text: root.label + root.value; color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: 13; anchors.verticalCenter: parent.verticalCenter }
}
