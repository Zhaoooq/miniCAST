import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MiniCastMonitor

Item {
    id: root
    property bool showingCurrent: true
    readonly property var tableModel: showingCurrent
        ? appController.activeAlarms
        : appController.alarms.filter(function(alarm) { return alarm.recovered })

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 7
        RowLayout {
            Layout.fillWidth: true; Layout.preferredHeight: 36; spacing: 5
            HmiButton { text: "当前报警 " + appController.activeAlarmCount; selected: root.showingCurrent; implicitHeight: 34; onClicked: root.showingCurrent = true }
            HmiButton { text: "历史记录"; selected: !root.showingCurrent; implicitHeight: 34; onClicked: root.showingCurrent = false }
            Item { Layout.fillWidth: true }
            HmiButton { text: "清除记录"; iconName: "trash"; implicitWidth: 94; implicitHeight: 34; onClicked: clearAlarmDialog.open() }
        }
        Rectangle {
            Layout.fillWidth: true; Layout.fillHeight: true
            color: Theme.surface; border.color: Theme.border; radius: Theme.radius; clip: true
            ColumnLayout {
                anchors.fill: parent; spacing: 0
                Rectangle {
                    Layout.fillWidth: true; Layout.preferredHeight: 30; color: Theme.tableHeader
                    Row {
                        anchors.fill: parent
                        Repeater {
                            model: [{t:"时间",w:.17},{t:"MFC",w:.16},{t:"类型",w:.18},{t:"实际值",w:.17},{t:"目标值",w:.17},{t:"状态",w:.15}]
                            delegate: HmiTableHeader { required property var modelData; width: parent.width * modelData.w; height: parent.height; text: modelData.t }
                        }
                    }
                }
                ListView {
                    id: alarmList
                    Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                    model: root.tableModel
                    delegate: Rectangle {
                        required property var modelData
                        required property int index
                        width: ListView.view.width; height: 39
                        color: modelData.severity === 2 ? Theme.criticalRow : (index % 2 ? Theme.secondaryBackground : Theme.surface)
                        Row {
                            anchors.fill: parent
                            component Cell: Item {
                                property string value: "—"
                                property color ink: Theme.textPrimary
                                Text { anchors.fill: parent; anchors.leftMargin: 3; anchors.rightMargin: 3; text: parent.value; color: parent.ink; font.pixelSize: 10; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight }
                            }
                            Cell { width: parent.width * .17; height: parent.height; value: modelData.time || "—" }
                            Cell { width: parent.width * .16; height: parent.height; value: modelData.channelName || "系统" }
                            Cell { width: parent.width * .18; height: parent.height; value: modelData.severityText || modelData.parameter || "—"; ink: modelData.severity === 2 ? Theme.red : Theme.yellow }
                            Cell { width: parent.width * .17; height: parent.height; value: modelData.parameter ? Number(modelData.realValue).toFixed(2) : "—"; ink: modelData.severity === 2 ? Theme.red : Theme.textPrimary }
                            Cell { width: parent.width * .17; height: parent.height; value: modelData.parameter ? Number(modelData.setValue).toFixed(2) : "—" }
                            Cell { width: parent.width * .15; height: parent.height; value: modelData.recovered ? "已恢复" : "活动中"; ink: modelData.recovered ? Theme.textSecondary : Theme.red }
                        }
                        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.divider }
                    }
                    Item {
                        visible: alarmList.count === 0
                        anchors.centerIn: parent; width: 320; height: 70
                        Column {
                            anchors.centerIn: parent; spacing: 7
                            Text { anchors.horizontalCenter: parent.horizontalCenter; text: root.showingCurrent ? "✓ 当前无活动报警" : "暂无历史记录"; color: root.showingCurrent ? Theme.green : Theme.textSecondary; font.pixelSize: 16; font.weight: Font.DemiBold }
                            Text { visible: root.showingCurrent; anchors.horizontalCenter: parent.horizontalCenter; text: "监测报警将在此处显示"; color: Theme.textSecondary; font.pixelSize: 11 }
                        }
                    }
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                }
            }
        }
    }
    ConfirmDialog { id: clearAlarmDialog; title: "清空报警记录"; message: "确认清空全部报警历史吗？"; acceptText: "清空记录"; acceptDanger: true; onAccepted: appController.clearAlarms() }
}
