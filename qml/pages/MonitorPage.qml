import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import MiniCastMonitor

Item {
    id: root
    property var selectedChannel: ({})

    function openDetails(channel) {
        selectedChannel = channel
        detailDialog.open()
    }

    RowLayout {
        id: columns
        anchors.fill: parent
        anchors.margins: 8
        spacing: 6
        Repeater {
            model: appController.gasChannels
            delegate: MfcMonitorColumn {
                required property var modelData
                // Identical preferred widths make the five device columns
                // exactly equal on the fixed 800 px screen.
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredWidth: 1
                Layout.minimumWidth: 0
                channel: modelData
                onDetailsRequested: root.openDetails(modelData)
            }
        }
    }

    Dialog {
        id: detailDialog
        modal: true
        anchors.centerIn: Overlay.overlay
        width: Math.min(430, Overlay.overlay ? Overlay.overlay.width - 28 : 430)
        padding: 14
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle { color: Theme.surface; border.color: Theme.border; border.width: 1; radius: Theme.radius }
        header: Text {
            leftPadding: 14; rightPadding: 14; topPadding: 12; bottomPadding: 8
            text: "MFC" + (root.selectedChannel.address || "—") + " 设备详情"
            color: Theme.textPrimary; font.pixelSize: 17; font.weight: Font.DemiBold
        }
        contentItem: GridLayout {
            columns: 2
            columnSpacing: 14
            rowSpacing: 8
            Repeater {
                model: [
                    ["气体名称", root.selectedChannel.name || "—"],
                    ["目标流量", root.selectedChannel.targetFlowAvailable
                        ? Number(root.selectedChannel.targetFlow).toFixed(3) + " " + root.selectedChannel.flowUnit : "—"],
                    ["实际流量", root.selectedChannel.actualFlowFresh
                        ? Number(root.selectedChannel.actualFlow).toFixed(3) + " " + root.selectedChannel.flowUnit
                        : (root.selectedChannel.lastValidFlowTimestamp
                           ? "—（最后有效 " + Number(root.selectedChannel.lastValidActualFlow).toFixed(3) + " " + root.selectedChannel.flowUnit + "）" : "—")],
                    ["最后有效时间", root.selectedChannel.lastValidFlowTimestamp || "—"],
                    ["偏差", root.selectedChannel.deviationAvailable
                        ? Number(root.selectedChannel.deviation).toFixed(3) + " " + root.selectedChannel.flowUnit : "—"],
                    ["偏差率", root.selectedChannel.deviationAvailable
                        ? (Number(root.selectedChannel.deviationPercent) >= 0 ? "+" : "") + Number(root.selectedChannel.deviationPercent).toFixed(1) + " %" : "—"],
                    ["量程", Number(root.selectedChannel.fullScaleValue || 0) > 0
                        ? Number(root.selectedChannel.fullScaleValue).toString() + " " + (root.selectedChannel.flowUnit || "") : "—"],
                    ["当前 %FS", root.selectedChannel.actualValueAvailable
                        ? Number(root.selectedChannel.percentFullScale || 0).toFixed(2) + " %FS" : "—"],
                    ["通信", root.selectedChannel.communicationState || "离线"],
                    ["状态", root.selectedChannel.comparisonStatusText || "通信异常"]
                ]
                delegate: Item {
                    required property var modelData
                    Layout.fillWidth: true
                    Layout.preferredHeight: 23
                    Text { anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter; text: modelData[0]; color: Theme.textSecondary; font.pixelSize: 12 }
                    Text { anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; text: modelData[1]; color: Theme.textPrimary; font.pixelSize: 12; font.weight: Font.DemiBold; elide: Text.ElideLeft; width: Math.max(80, parent.width - 86); horizontalAlignment: Text.AlignRight }
                }
            }
        }
        footer: Item {
            implicitHeight: 50
            HmiButton { anchors.right: parent.right; anchors.rightMargin: 12; anchors.verticalCenter: parent.verticalCenter; text: "关闭"; implicitWidth: 80; onClicked: detailDialog.close() }
        }
    }
}
