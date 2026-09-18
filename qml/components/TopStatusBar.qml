import QtQuick
import QtQuick.Layouts
import MiniCastMonitor

Rectangle {
    id: root
    signal stopRequested()
    signal operatingPointsRequested()
    implicitHeight: 54
    color: Theme.surface

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        spacing: 10

        Text {
            text: "高浓度碳烟发生器监测系统"
            color: Theme.textPrimary
            font.pixelSize: 16
            font.weight: Font.DemiBold
            Layout.preferredWidth: 200
            elide: Text.ElideRight
        }
        Row {
            spacing: 5
            Rectangle { width: 8; height: 8; radius: 4; anchors.verticalCenter: parent.verticalCenter; color: appController.controlStopping ? Theme.yellow : (appController.controlling ? Theme.green : (appController.monitoring ? Theme.yellow : Theme.offline)) }
            Text { anchors.verticalCenter: parent.verticalCenter; text: appController.controlStopping ? "正在停止控制…" : (appController.controlling ? "控制运行" : (appController.monitoring ? "监测中 · 控制已停止" : "未连接")); color: Theme.textPrimary; font.pixelSize: 12; font.weight: Font.DemiBold }
        }
        Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 22; color: Theme.divider }
        ColumnLayout {
            Layout.preferredWidth: 142
            spacing: 1
            Text { text: "当前运行点"; color: Theme.textSecondary; font.pixelSize: 10 }
            Text { Layout.fillWidth: true; text: appController.currentPointName || "未选择运行点"; color: Theme.textPrimary; font.pixelSize: 12; font.weight: Font.DemiBold; elide: Text.ElideRight }
        }
        Text {
            text: "活动报警 " + appController.activeAlarmCount
            color: appController.activeAlarmCount > 0 ? Theme.red : Theme.textSecondary
            font.pixelSize: 12
            font.weight: Font.DemiBold
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignRight
        }
        HmiButton {
            visible: !appController.controlling && !appController.controlStopping
            text: "选择运行点"
            implicitWidth: 96
            implicitHeight: 34
            onClicked: root.operatingPointsRequested()
        }
        HmiButton {
            text: appController.controlStopping ? "正在停止…" : (appController.controlling ? "停止控制" : "开始控制")
            implicitWidth: 92
            implicitHeight: 34
            enabled: !appController.controlStopping && (appController.controlling || appController.currentPointId.length > 0)
            primary: !appController.controlling
            danger: appController.controlling
            onClicked: appController.controlling ? root.stopRequested() : appController.startControl()
        }
    }
    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.divider }
}
