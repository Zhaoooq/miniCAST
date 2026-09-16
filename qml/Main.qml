import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MiniCastMonitor

ApplicationWindow {
    id: root
    width: 800; height: 480; minimumWidth: 800; minimumHeight: 450
    visible: true
    visibility: appController.windowMode === 2 ? Window.FullScreen : Window.Windowed
    flags: appController.windowMode === 1 ? (Qt.Window | Qt.FramelessWindowHint) : Qt.Window
    title: "高浓度碳烟发生器监测系统"
    color: Theme.background
    property int pageIndex: 0
    property bool forceClose: false
    property bool exitAfterControlStop: false
    property bool captureMode: false

    font.family: Theme.fontFamily
    onClosing: function(close) {
        if (appController.monitoring && !forceClose) { close.accepted = false; exitDialog.open() }
    }

    palette.window: Theme.background
    palette.windowText: Theme.textPrimary
    palette.button: "#DDE3E7"
    palette.buttonText: Theme.textPrimary
    palette.base: "white"
    palette.text: Theme.textPrimary
    palette.highlight: Theme.primary

    ColumnLayout {
        anchors.fill: parent; spacing: 0
        TopStatusBar {
            Layout.fillWidth: true
            onStopRequested: stopDialog.open()
            onOperatingPointsRequested: root.pageIndex = 1
        }
        StackLayout {
            currentIndex: root.pageIndex; Layout.fillWidth: true; Layout.fillHeight: true
            Layout.minimumHeight: 0
            MonitorPage {}
            OperatingPointPage {}
            AlarmPage {}
            SettingsPage {
                onOperatingPointsRequested: root.pageIndex = 1
            }
        }
        DeviceStatusBar {
            Layout.fillWidth: true
            onAlarmRequested: root.pageIndex = 2
        }
        BottomNavigation {
            Layout.fillWidth: true
            currentIndex: root.pageIndex
            onPageRequested: function(index) { root.pageIndex = index }
        }
    }

    Rectangle {
        visible: appController.notification.length > 0 && !root.captureMode
        anchors.horizontalCenter: parent.horizontalCenter; anchors.bottom: parent.bottom; anchors.bottomMargin: 64
        width: Math.min(parent.width - 40, messageText.implicitWidth + 40); height: 42; radius: 3
        color: "#33424D"; border.color: Theme.primary; z: 50
        Text { id: messageText; anchors.centerIn: parent; text: appController.notification; color: "white"; font.family: Theme.fontFamily; font.pixelSize: 13 }
        Timer { interval: 4200; running: parent.visible; onTriggered: appController.clearNotification() }
    }

    ConfirmDialog {
        id: stopDialog
        title: "停止控制"
        message: "将把当前运行点的 5 路 MFC 目标流量设为 0，并继续监测实际流量。"
        detail: "目标流量归零不等同于外部气源已安全隔离。"
        acceptText: "停止控制"
        acceptDanger: true
        onAccepted: appController.stopControl()
    }
    ConfirmDialog {
        id: exitDialog; title: "退出程序"; message: appController.controlling ? "当前仍在控制流量。将先执行停止控制，确认目标归零后再退出。" : "确定退出程序吗？"
        detail: appController.controlling ? "若有 MFC 停止状态无法确认，软件将保留在当前界面并明确提示。" : ""
        acceptText: appController.controlling ? "先停止控制并退出" : "退出"
        onAccepted: {
            if (appController.controlling) { root.exitAfterControlStop = true; appController.stopControl() }
            else { root.forceClose = true; root.close() }
        }
    }
    Connections {
        target: appController
        function onDeviceInfoChanged() {
            if (root.exitAfterControlStop && !appController.controlling && !appController.controlStopping) {
                root.forceClose = true
                root.close()
            }
        }
    }
}
