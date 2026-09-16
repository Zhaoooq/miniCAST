import QtQuick
import QtQuick.Controls
import MiniCastMonitor

Dialog {
    id: control
    property string message: ""
    property string detail: ""
    property string acceptText: "确认"
    property string rejectText: "取消"
    property bool acceptEnabled: true
    property bool acceptDanger: false
    property bool monitoringWhenOpened: false
    modal: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(440, Overlay.overlay ? Overlay.overlay.width - 32 : 440)
    padding: 16
    closePolicy: Popup.NoAutoClose
    onOpened: monitoringWhenOpened = appController.monitoring

    Connections {
        target: appController
        function onMonitoringChanged() {
            if (control.monitoringWhenOpened && !appController.monitoring) control.close()
        }
    }

    background: Rectangle { color: "white"; border.color: Theme.border; border.width: 1; radius: 3 }
    header: Rectangle {
        implicitHeight: 42; height: 42; color: Theme.secondaryBackground
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.border }
        Text { anchors.left: parent.left; anchors.leftMargin: 16; anchors.right: parent.right; anchors.rightMargin: 16; anchors.top: parent.top; anchors.bottom: parent.bottom; text: control.title; color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: 16; font.weight: Font.Medium; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight }
    }
    contentItem: Column {
        spacing: control.detail.length > 0 ? 12 : 0
        Text {
            width: parent.width
            text: control.message
            color: Theme.textPrimary
            font.family: Theme.fontFamily
            font.pixelSize: 14
            font.weight: Font.Medium
            wrapMode: Text.Wrap
        }
        Text {
            width: parent.width
            visible: control.detail.length > 0
            text: control.detail
            color: Theme.textSecondary
            font.family: Theme.fontFamily
            font.pixelSize: 13
            wrapMode: Text.Wrap
        }
    }
    footer: Rectangle {
        implicitHeight: 52; height: 52; color: "white"
        Row {
            anchors.right: parent.right; anchors.rightMargin: 12; anchors.verticalCenter: parent.verticalCenter; spacing: 8
            HmiButton { text: control.rejectText; implicitWidth: 96; onClicked: control.reject() }
            HmiButton { text: control.acceptText; implicitWidth: 104; primary: !control.acceptDanger; danger: control.acceptDanger; enabled: control.acceptEnabled; onClicked: control.accept() }
        }
    }
}
