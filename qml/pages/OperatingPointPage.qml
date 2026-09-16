import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MiniCastMonitor

Item {
    id: root
    property var selectedPoint: ({})
    property var pendingPoint: ({})

    function newPoint() {
        editor.draft = { id: "", name: appController.nextCustomerPointName,
            description: "", mfcTargetFlows: {} }
        editor.open()
    }
    function editPoint(point) {
        if (appController.monitoring && point.id === appController.currentPointId) return
        editor.draft = { id: point.id, name: point.name, description: point.description || "",
            mfcTargetFlows: Object.assign({}, point.mfcTargetFlows || {}) }
        editor.open()
    }
    Connections {
        target: appController
        function onOperatingPointsChanged() {
            if (root.selectedPoint.id)
                root.selectedPoint = appController.operatingPoint(root.selectedPoint.id)
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 8

        Rectangle {
            Layout.preferredWidth: 232
            Layout.fillHeight: true
            color: Theme.surface; border.color: Theme.border; radius: Theme.radius
            ColumnLayout {
                anchors.fill: parent; anchors.margins: 7; spacing: 5
                Text { text: "运行点列表"; color: Theme.textPrimary; font.pixelSize: 15; font.weight: Font.DemiBold }
                HmiButton { text: "+ 新建运行点"; primary: true; Layout.fillWidth: true; implicitHeight: 36; onClicked: root.newPoint() }
                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }
                ListView {
                    Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                    model: appController.operatingPoints
                    delegate: ItemDelegate {
                        required property var modelData
                        width: ListView.view.width; height: 42
                        onClicked: root.selectedPoint = modelData
                        background: Rectangle { color: root.selectedPoint.id === modelData.id ? Theme.primarySelection : "transparent"; radius: 3 }
                        contentItem: Column {
                            anchors.verticalCenter: parent.verticalCenter; width: parent.width
                            Text { width: parent.width; text: modelData.name; color: Theme.textPrimary; font.pixelSize: 12; font.weight: Font.DemiBold; elide: Text.ElideRight }
                            Text { width: parent.width; text: modelData.id === appController.currentPointId ? "当前运行点" : (modelData.readOnly ? "系统预设" : "客户运行点"); color: modelData.id === appController.currentPointId ? Theme.green : Theme.textSecondary; font.pixelSize: 10; elide: Text.ElideRight }
                        }
                    }
                    Text { visible: parent.count === 0; anchors.centerIn: parent; text: "暂无运行点"; color: Theme.textSecondary; font.pixelSize: 12 }
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true; Layout.fillHeight: true
            color: Theme.surface; border.color: Theme.border; radius: Theme.radius
            Item {
                anchors.fill: parent; anchors.margins: 12
                visible: !root.selectedPoint.id
                Column {
                    anchors.centerIn: parent; width: Math.min(parent.width - 30, 380); spacing: 8
                    Text { width: parent.width; text: "尚未选择运行点"; color: Theme.textPrimary; font.pixelSize: 18; font.weight: Font.DemiBold; horizontalAlignment: Text.AlignHCenter }
                    Text { width: parent.width; text: "请选择已有运行点或创建新的客户运行点"; color: Theme.textSecondary; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.Wrap }
                    HmiButton { anchors.horizontalCenter: parent.horizontalCenter; text: "新建运行点"; primary: true; implicitWidth: 118; implicitHeight: 38; onClicked: root.newPoint() }
                }
            }
            ColumnLayout {
                anchors.fill: parent; anchors.margins: 12; spacing: 5
                visible: !!root.selectedPoint.id
                RowLayout {
                    Layout.fillWidth: true
                    Text { Layout.fillWidth: true; text: root.selectedPoint.name || "未选择运行点"; color: Theme.textPrimary; font.pixelSize: 17; font.weight: Font.Bold; elide: Text.ElideRight }
                    Text { text: root.selectedPoint.id === appController.currentPointId ? "当前运行点" : ""; color: Theme.green; font.pixelSize: 11; font.weight: Font.DemiBold }
                }
                Text { text: "5 路 MFC 的目标流量（开始控制时按 %F.S. 下发）"; color: Theme.textSecondary; font.pixelSize: 11 }
                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }
                Repeater {
                    model: appController.mfcDevices
                    delegate: Item {
                        required property var modelData
                        required property int index
                        readonly property var target: root.selectedPoint.mfcTargetFlows ? root.selectedPoint.mfcTargetFlows[String(modelData.address)] : undefined
                        Layout.fillWidth: true; Layout.preferredHeight: 29
                        Rectangle { anchors.fill: parent; color: index % 2 ? Theme.secondaryBackground : "transparent" }
                        Text { anchors.left: parent.left; anchors.leftMargin: 9; anchors.verticalCenter: parent.verticalCenter; text: modelData.displayName + " · 地址 " + modelData.address; color: Theme.textPrimary; font.pixelSize: 12 }
                        Text { anchors.right: parent.right; anchors.rightMargin: 9; anchors.verticalCenter: parent.verticalCenter; text: target === undefined ? "—" : Number(target).toFixed(3) + (modelData.unit ? " " + modelData.unit : ""); color: Theme.targetFlow; font.pixelSize: 12; font.weight: Font.DemiBold }
                    }
                }
                Item { Layout.fillHeight: true }
                Text { visible: appController.monitoring && root.selectedPoint.id === appController.currentPointId; text: "请先停止监测，再编辑当前运行点目标参数"; color: Theme.yellow; font.pixelSize: 11 }
                RowLayout {
                    Layout.fillWidth: true; spacing: 6
                    HmiButton { text: "编辑"; implicitWidth: 74; implicitHeight: 38; visible: !root.selectedPoint.readOnly; enabled: !(appController.monitoring && root.selectedPoint.id === appController.currentPointId); onClicked: root.editPoint(root.selectedPoint) }
                    HmiButton { text: "复制"; implicitWidth: 70; implicitHeight: 38; enabled: !appController.customerPointFull; onClicked: appController.duplicateOperatingPoint(root.selectedPoint.id) }
                    HmiButton { text: "删除"; danger: true; implicitWidth: 70; implicitHeight: 38; visible: !root.selectedPoint.readOnly; onClicked: { root.pendingPoint = root.selectedPoint; deleteDialog.open() } }
                    Item { Layout.fillWidth: true }
                    HmiButton { text: appController.controlling ? "切换运行点" : "开始控制"; primary: true; implicitWidth: 132; implicitHeight: 38; enabled: !appController.controlStopping && !appController.fullScaleDiagnosticsRunning; onClicked: { root.pendingPoint = root.selectedPoint; applyDialog.open() } }
                }
            }
        }
    }

    HmiDialog {
        id: editor
        property var draft: ({})
        property string addressKey: ""
        title: draft.id ? "编辑运行点" : "新建运行点"
        width: Math.min(590, Overlay.overlay.width - 24)
        padding: 10
        acceptText: "保存"
        contentItem: ColumnLayout {
            spacing: 4
            Text { text: editor.draft.name; color: Theme.textPrimary; font.pixelSize: 15; font.weight: Font.DemiBold }
            Text { text: "目标值经量程校验后，仅在明确开始控制时下发到 MFC"; color: Theme.textSecondary; font.pixelSize: 11 }
            Repeater {
                model: appController.mfcDevices
                delegate: RowLayout {
                    required property var modelData
                    readonly property string key: String(modelData.address)
                    readonly property real maximum: modelData.maxSetpoint > 0 ? modelData.maxSetpoint : modelData.fullScale
                    Layout.fillWidth: true; Layout.preferredHeight: 37
                    Text { text: modelData.displayName + "  地址 " + modelData.address; Layout.fillWidth: true; color: Theme.textPrimary; font.pixelSize: 12 }
                    HmiButton {
                        implicitWidth: 145; implicitHeight: 34
                        text: Number(editor.draft.mfcTargetFlows && editor.draft.mfcTargetFlows[key] !== undefined ? editor.draft.mfcTargetFlows[key] : 0).toFixed(3) + "  ›"
                        enabled: maximum > 0
                        onClicked: { editor.addressKey = key; keypad.openFor(modelData.displayName + "目标流量", editor.draft.mfcTargetFlows[key] || 0, modelData.minSetpoint, maximum, modelData.unit || "", false, 3, []) }
                    }
                    Text { text: modelData.unit || "—"; color: Theme.textSecondary; font.pixelSize: 11; Layout.preferredWidth: 52; elide: Text.ElideRight }
                }
            }
        }
        onAccepted: appController.saveOperatingPoint(draft)
    }
    TouchNumericKeypadDialog {
        id: keypad
        onValueAccepted: function(value) {
            var next = Object.assign({}, editor.draft)
            next.mfcTargetFlows = Object.assign({}, editor.draft.mfcTargetFlows || {})
            next.mfcTargetFlows[editor.addressKey] = value
            editor.draft = next
        }
    }
    ConfirmDialog { id: applyDialog; title: appController.controlling ? "切换运行点" : "开始控制"; message: appController.controlling ? "将按 Hold → 装载 → 核验 → Follow 切换运行点。" : "将预检设备配置、切换为数字模式并下发运行点。"; detail: "只允许写 Current CM、Hold/Follow、Digital Setpoint。"; acceptText: "确认"; onAccepted: { if (!appController.fullScaleDiagnosticsRunning) { appController.selectOperatingPoint(root.pendingPoint.id); appController.startControl() } } }
    ConfirmDialog { id: deleteDialog; title: "删除客户运行点"; message: "确认删除此客户运行点？"; acceptText: "删除"; acceptDanger: true; onAccepted: { appController.deleteOperatingPoint(root.pendingPoint.id); root.selectedPoint = ({}) } }
}
