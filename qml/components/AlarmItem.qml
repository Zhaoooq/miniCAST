import QtQuick
import MiniCastMonitor

Rectangle {
    id: root
    required property var alarm
    required property int index
    color: alarm.severity === 2 ? Theme.criticalRow : (alarm.severity === 1 ? Theme.warningRow : (index % 2 ? "#F9FBFC" : Theme.surface))
    implicitHeight: 39
    Row {
        anchors.fill: parent
        component Cell: Item {
            property string value: "—"
            property color textColor: Theme.textPrimary
            Text { anchors.fill: parent; anchors.leftMargin: 4; anchors.rightMargin: 4; text: parent.value; color: parent.textColor; font.pixelSize: 10; font.family: parent.value.match(/^[+\-0-9.—% ]+$/) ? "monospace" : Theme.fontFamily; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight }
        }
        Cell { width: parent.width * .12; height: parent.height; value: alarm.time || "—" }
        Cell { width: parent.width * .09; height: parent.height; value: alarm.severityText || "提示"; textColor: alarm.severity === 2 ? Theme.red : (alarm.severity === 1 ? Theme.yellow : Theme.primary) }
        Cell { width: parent.width * .15; height: parent.height; value: alarm.channelName || "系统" }
        Cell { width: parent.width * .27; height: parent.height; value: alarm.message || "—" }
        Cell { width: parent.width * .13; height: parent.height; value: alarm.parameter ? Number(alarm.realValue).toFixed(2) + " " + alarm.unit : "—"; textColor: alarm.severity === 2 ? Theme.red : Theme.textPrimary }
        Cell { width: parent.width * .12; height: parent.height; value: alarm.parameter ? Number(alarm.setValue).toFixed(2) : "—" }
        Cell { width: parent.width * .12; height: parent.height; value: alarm.recovered ? "已恢复" : "未恢复"; textColor: alarm.recovered ? Theme.textSecondary : Theme.red }
    }
    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.divider }
}
