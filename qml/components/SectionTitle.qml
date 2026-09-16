import QtQuick
import MiniCastMonitor

Row {
    property string title: ""
    property string subtitle: ""
    spacing: 9
    Rectangle { width: 3; height: 18; radius: 2; color: Theme.primary; anchors.verticalCenter: parent.verticalCenter }
    Text { text: title; color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: 16; font.weight: Font.DemiBold }
    Text { text: subtitle; color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: 11; anchors.baseline: parent.children[1].baseline }
}
