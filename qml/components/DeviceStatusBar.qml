import QtQuick
import QtQuick.Layouts
import MiniCastMonitor

// The only bottom information strip. It deliberately contains alarm data
// only; global monitoring controls live exclusively in TopStatusBar.
Rectangle {
    id: root
    signal alarmRequested()
    implicitHeight: 42
    color: Theme.surface
    border.color: Theme.border
    readonly property var activeAlarms: appController.activeAlarms
    readonly property var currentAlarm: activeAlarms.length ? activeAlarms[0] : ({})

    MouseArea {
        anchors.fill: parent
        enabled: root.activeAlarms.length > 0
        onClicked: root.alarmRequested()
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 14
            anchors.rightMargin: 14
            spacing: 8
            Text { text: root.activeAlarms.length ? "⚠" : "✓"; color: root.activeAlarms.length ? Theme.red : Theme.green; font.pixelSize: 16; font.weight: Font.Bold }
            Text {
                Layout.fillWidth: true
                text: root.activeAlarms.length ? root.currentAlarm.message : "当前无报警"
                color: root.activeAlarms.length ? Theme.textPrimary : Theme.green
                font.pixelSize: 12
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            Text { visible: root.activeAlarms.length > 0; text: "当前报警 " + root.activeAlarms.length + "  ›"; color: Theme.red; font.pixelSize: 12; font.weight: Font.DemiBold }
        }
    }
}
