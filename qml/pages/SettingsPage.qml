pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import MiniCastMonitor

Item {
    id: root
    signal operatingPointsRequested()
    property int selectedCategory: 0
    property var categories: ["系统设置", "运行点管理", "设备设置", "显示设置", "数据管理", "关于软件"]
    property int pendingConfirmAddress: -1

    // Both the device page and the read-only dialogs consume the immutable
    // snapshot exposed by AppController.deviceInfo.devices.  In particular,
    // metadataResults is the field-level source of truth: metadataAvailable
    // only means every optional metadata register succeeded.
    function metadataResult(device, field) {
        return (device.metadataResults || {})[field] || ({})
    }
    function metadataValue(result, suffix) {
        return result.reportedAvailable ? String(result.reported) + (suffix || "")
                                      : (result.statusText || "读取失败")
    }
    function metadataStatus(result) {
        var status = result.statusText || "读取失败"
        return result.error ? status + "：" + result.error : status
    }
    FolderDialog {
        id: dataFolderDialog
        title: "选择数据保存目录"
        currentFolder: appController.dataRootUrl
        acceptLabel: "选择此目录"
        rejectLabel: "取消"
        onAccepted: appController.setDataRoot(selectedFolder)
    }

    component CategoryIcon: Canvas {
        required property int iconIndex
        property color ink: Theme.textPrimary
        width: 23
        height: 23
        onInkChanged: requestPaint()
        onIconIndexChanged: requestPaint()
        onPaint: {
            var c = getContext("2d")
            c.reset()
            c.strokeStyle = ink
            c.fillStyle = ink
            c.lineWidth = 1.8
            c.lineCap = "round"
            c.lineJoin = "round"
            if (iconIndex === 0) {
                c.beginPath(); c.arc(11.5, 11.5, 4, 0, Math.PI * 2); c.stroke()
                for (var i = 0; i < 8; ++i) {
                    var a = i * Math.PI / 4
                    c.beginPath(); c.moveTo(11.5 + 6 * Math.cos(a), 11.5 + 6 * Math.sin(a))
                    c.lineTo(11.5 + 8.5 * Math.cos(a), 11.5 + 8.5 * Math.sin(a)); c.stroke()
                }
            } else if (iconIndex === 1) {
                c.beginPath(); c.arc(11.5, 11.5, 2.2, 0, Math.PI * 2); c.stroke()
                var dots = [[11.5,3],[18.5,7],[18.5,16],[11.5,20],[4.5,16],[4.5,7]]
                for (var j = 0; j < dots.length; ++j) {
                    c.beginPath(); c.arc(dots[j][0], dots[j][1], 1.35, 0, Math.PI * 2); c.fill()
                }
                c.beginPath(); c.moveTo(11.5,5); c.lineTo(11.5,8.8); c.moveTo(16.5,8); c.lineTo(14,10)
                c.moveTo(16.5,15); c.lineTo(14,13); c.moveTo(11.5,18); c.lineTo(11.5,14.2)
                c.moveTo(6.5,15); c.lineTo(9,13); c.moveTo(6.5,8); c.lineTo(9,10); c.stroke()
            } else if (iconIndex === 2) {
                c.strokeRect(4, 5, 15, 12)
                c.beginPath(); c.moveTo(7,17); c.lineTo(7,20); c.lineTo(10,20); c.lineTo(10,17)
                c.moveTo(13,17); c.lineTo(13,20); c.lineTo(16,20); c.lineTo(16,17)
                c.moveTo(7,9); c.lineTo(10,9); c.moveTo(7,12); c.lineTo(10,12); c.stroke()
                c.beginPath(); c.arc(16,12,1.2,0,Math.PI*2); c.fill()
            } else if (iconIndex === 3) {
                c.strokeRect(3.5, 4.5, 16, 12)
                c.beginPath(); c.moveTo(8,20); c.lineTo(15,20); c.moveTo(10,16.5); c.lineTo(9,20)
                c.moveTo(13,16.5); c.lineTo(14,20); c.stroke()
                c.beginPath(); c.moveTo(7,13); c.lineTo(10,10); c.lineTo(12,12); c.lineTo(16,8); c.stroke()
            } else if (iconIndex === 4) {
                c.strokeRect(4, 5, 15, 15)
                c.beginPath(); c.moveTo(4,9); c.lineTo(19,9); c.moveTo(8,3); c.lineTo(8,7)
                c.moveTo(15,3); c.lineTo(15,7); c.stroke()
                for (var x = 0; x < 3; ++x) for (var y = 0; y < 2; ++y) c.fillRect(7 + x * 4, 12 + y * 4, 2, 2)
            } else {
                c.beginPath(); c.arc(11.5,11.5,8,0,Math.PI*2); c.fill()
                c.fillStyle = "white"; c.font = "bold 12px sans-serif"; c.textAlign = "center"; c.textBaseline = "middle"
                c.fillText("i", 11.5, 12)
            }
        }
    }

    component SettingsRow: Item {
        property string label: ""
        default property alias rowContent: valueArea.data
        Layout.fillWidth: true
        Layout.preferredHeight: 33

        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.divider }
        Rectangle { x: 153; width: 1; height: parent.height; color: Theme.divider }
        Text {
            x: 24; width: 120; anchors.verticalCenter: parent.verticalCenter
            text: parent.label; color: Theme.textPrimary
            font.pixelSize: 11; font.weight: Font.Medium
        }
        Item { id: valueArea; x: 154; width: parent.width - x; height: parent.height }
    }

    component ReadoutRow: Item {
        property string label: ""
        property string value: ""
        property color valueColor: Theme.textPrimary
        Layout.fillWidth: true; Layout.preferredHeight: 39
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.divider }
        Text { anchors.left: parent.left; anchors.leftMargin: 10; anchors.verticalCenter: parent.verticalCenter; text: parent.label; color: Theme.textPrimary; font.pixelSize: 11 }
        Text { anchors.right: parent.right; anchors.rightMargin: 10; anchors.verticalCenter: parent.verticalCenter; text: parent.value; color: parent.valueColor; font.pixelSize: 11; font.weight: Font.Medium; elide: Text.ElideLeft; width: Math.min(360, parent.width * .62); horizontalAlignment: Text.AlignRight }
    }

    RowLayout {
        anchors.fill: parent; anchors.margins: 7; spacing: 9
        Rectangle {
            Layout.preferredWidth: 180; Layout.fillHeight: true
            color: Theme.surface; border.color: Theme.border; radius: Theme.radius; clip: true
            ColumnLayout {
                anchors.fill: parent; spacing: 0
                Repeater {
                    model: root.categories
                    delegate: ItemDelegate {
                        required property string modelData
                        required property int index
                        Layout.fillWidth: true; Layout.preferredHeight: 45
                        onClicked: root.selectedCategory = index
                        background: Rectangle {
                            color: root.selectedCategory === index ? Theme.primary : (parent.pressed ? Theme.primaryPressedBackground : Theme.surface)
                            border.color: root.selectedCategory === index ? Theme.primaryHover : "transparent"
                            radius: root.selectedCategory === index ? 3 : 0
                            Rectangle {
                                visible: root.selectedCategory !== index
                                anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.divider
                            }
                        }
                        contentItem: Row {
                            leftPadding: 9; spacing: 12
                            CategoryIcon {
                                iconIndex: index
                                ink: root.selectedCategory === index ? "white" : Theme.textPrimary
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            Text {
                                text: modelData
                                color: root.selectedCategory === index ? "white" : Theme.textPrimary
                                font.family: Theme.fontFamily; font.pixelSize: 13; font.weight: Font.DemiBold
                                anchors.verticalCenter: parent.verticalCenter
                            }
                        }
                    }
                }
                Item { Layout.fillHeight: true }
            }
        }

        Rectangle {
            Layout.fillWidth: true; Layout.fillHeight: true
            color: Theme.surface; border.color: Theme.border; radius: Theme.radius; clip: true
            ColumnLayout {
                anchors.fill: parent; spacing: 0
                Rectangle {
                    Layout.fillWidth: true; Layout.preferredHeight: 40; color: Theme.secondaryBackground
                    Text { anchors.left: parent.left; anchors.leftMargin: 18; anchors.verticalCenter: parent.verticalCenter; text: root.categories[root.selectedCategory]; color: Theme.textPrimary; font.pixelSize: 16; font.weight: Font.Bold }
                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.border }
                }
                Loader {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    sourceComponent: [systemPane, pointsPane, devicePane, displayPane, dataPane, aboutPane][root.selectedCategory]
                }
            }
        }
    }

    Component {
        id: systemPane
        ColumnLayout {
            anchors.fill: parent; spacing: 0
            SettingsRow {
                label: "轮询周期"
                HmiComboBox {
                    anchors.left: parent.left; anchors.leftMargin: 14; anchors.verticalCenter: parent.verticalCenter
                    property var values: [200, 500, 1000, 2000]
                    model: ["200 ms", "500 ms", "1000 ms", "2000 ms"]
                    currentIndex: Math.max(0, values.indexOf(appController.sampleIntervalMs))
                    implicitWidth: 150; implicitHeight: 28; font.pixelSize: 11
                    onActivated: appController.sampleIntervalMs = values[index]
                }
            }
            SettingsRow {
                label: "流量偏差预警"
                HmiComboBox {
                    anchors.left: parent.left; anchors.leftMargin: 14; anchors.verticalCenter: parent.verticalCenter
                    property var values: [3, 5, 8]
                    model: ["±3 %", "±5 %", "±8 %"]
                    currentIndex: Math.max(0, values.indexOf(appController.warningThreshold))
                    implicitWidth: 150; implicitHeight: 28; font.pixelSize: 11
                    onActivated: appController.warningThreshold = values[index]
                }
            }
            SettingsRow {
                label: "流量偏差严重"
                HmiComboBox {
                    anchors.left: parent.left; anchors.leftMargin: 14; anchors.verticalCenter: parent.verticalCenter
                    property var values: [8, 10, 15]
                    model: ["±8 %", "±10 %", "±15 %"]
                    currentIndex: Math.max(0, values.indexOf(appController.criticalThreshold))
                    implicitWidth: 150; implicitHeight: 28; font.pixelSize: 11
                    onActivated: appController.criticalThreshold = values[index]
                }
            }
            SettingsRow {
                label: "数据保存路径"
                Text {
                    anchors.left: parent.left; anchors.leftMargin: 14; anchors.right: browseButton.left; anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter; text: appController.dataRoot
                    color: Theme.textPrimary; font.pixelSize: 11; font.weight: Font.Medium; elide: Text.ElideMiddle
                }
                HmiButton {
                    id: browseButton
                    anchors.right: parent.right; anchors.rightMargin: 19; anchors.verticalCenter: parent.verticalCenter
                    text: "浏览"; implicitWidth: 84; implicitHeight: 27; font.pixelSize: 11
                    onClicked: dataFolderDialog.open()
                }
            }
            SettingsRow {
                label: "数据保存周期"
                HmiComboBox {
                    anchors.left: parent.left; anchors.leftMargin: 14; anchors.verticalCenter: parent.verticalCenter
                    implicitWidth: 150; implicitHeight: 28; font.pixelSize: 11
                    model: ["1 秒", "5 秒", "10 秒", "30 秒"]
                }
            }
            SettingsRow {
                label: "软件操作"
                HmiButton {
                    anchors.right: parent.right; anchors.rightMargin: 19; anchors.verticalCenter: parent.verticalCenter
                    text: "重启软件"; danger: true; implicitWidth: 117; implicitHeight: 32; font.pixelSize: 12
                    onClicked: restartDialog.open()
                }
            }
            Item { Layout.fillHeight: true }
        }
    }

    ConfirmDialog {
        id: restartDialog
        title: "重启软件"
        message: "确定要重启高浓度碳烟发生器监测系统吗？"
        detail: "当前版本会安全退出，请由系统服务重新启动应用。"
        acceptText: "退出并重启"
        acceptDanger: true
        onAccepted: Qt.quit()
    }

    ConfirmDialog {
        id: confirmAddressDialog
        title: "确认真机地址映射"
        acceptText: "确认地址"
        onAccepted: appController.confirmMfcAddress(root.pendingConfirmAddress)
    }

    Dialog {
        id: verificationDialog
        modal: true
        anchors.centerIn: Overlay.overlay
        width: Math.min(620, Overlay.overlay.width - 24)
        height: Math.min(410, Overlay.overlay.height - 24)
        padding: 12
        title: "设备信息核验（只读）"
        background: Rectangle { color: Theme.surface; border.color: Theme.border; radius: Theme.radius }
        contentItem: ScrollView {
            clip: true
            contentWidth: availableWidth
            Column {
                width: parent.width
                spacing: 10
                Repeater {
                    model: appController.deviceInfo.devices || []
                    delegate: Rectangle {
                        required property var modelData
                        readonly property var targetName: root.metadataResult(modelData, "Target Gas Name")
                        readonly property var targetCode: root.metadataResult(modelData, "Target Gas Code")
                        readonly property var targetScale: root.metadataResult(modelData, "Target Full Scale")
                        readonly property var calibrationName: root.metadataResult(modelData, "Calibration Gas Name")
                        readonly property var calibrationCode: root.metadataResult(modelData, "Calibration Gas Code")
                        readonly property var calibrationScale: root.metadataResult(modelData, "Calibration Full Scale")
                        readonly property var addressResult: root.metadataResult(modelData, "RS485 Address")
                        readonly property var modelResult: root.metadataResult(modelData, "Model")
                        readonly property var serialResult: root.metadataResult(modelData, "Serial")
                        readonly property var baudResult: root.metadataResult(modelData, "Baud")
                        width: parent.width; height: modelData.metadataVerificationAttempted ? 236 : 82; radius: 4
                        color: Theme.secondaryBackground; border.color: Theme.border
                        Column {
                            anchors.fill: parent; anchors.margins: 8; spacing: 3
                            Text { text: modelData.displayName + " · 地址 " + modelData.protocolAddress; color: Theme.textPrimary; font.pixelSize: 13; font.weight: Font.DemiBold }
                            Text { visible: !modelData.metadataVerificationAttempted; text: "尚未读取设备信息"; color: Theme.textSecondary; font.pixelSize: 11 }
                            Text { visible: modelData.metadataVerificationAttempted; text: "现场配置：Gas " + modelData.gasType + " · Gas Code " + ("00" + String(modelData.expectedGasCode)).slice(-3) + " · Expected Full Scale " + modelData.expectedDeviceFullScaleSccm + " SCCM"; color: Theme.textSecondary; font.pixelSize: 10; elide: Text.ElideRight; width: parent.width }
                            Text { visible: modelData.metadataVerificationAttempted; text: "Target：Gas Name " + root.metadataValue(targetName) + "（" + root.metadataStatus(targetName) + "） · Gas Code " + root.metadataValue(targetCode) + "（" + root.metadataStatus(targetCode) + "）"; color: Theme.textPrimary; font.pixelSize: 10; elide: Text.ElideRight; width: parent.width }
                            Text { visible: modelData.metadataVerificationAttempted; text: "Target Full Scale：" + root.metadataValue(targetScale, " SCCM") + "（" + root.metadataStatus(targetScale) + "）"; color: targetScale.status === "MISMATCH" ? Theme.red : Theme.textPrimary; font.pixelSize: 10; elide: Text.ElideRight; width: parent.width }
                            Text { visible: modelData.metadataVerificationAttempted; text: "Calibration：Gas Name " + root.metadataValue(calibrationName) + " · Gas Code " + root.metadataValue(calibrationCode) + " · Full Scale " + root.metadataValue(calibrationScale, " SCCM"); color: Theme.textSecondary; font.pixelSize: 10; elide: Text.ElideRight; width: parent.width }
                            Text { visible: modelData.metadataVerificationAttempted; text: "RS485 Address：" + root.metadataValue(addressResult) + " · Model " + root.metadataValue(modelResult) + " · Serial " + root.metadataValue(serialResult) + " · Baud " + root.metadataValue(baudResult); color: Theme.textSecondary; font.pixelSize: 10; elide: Text.ElideRight; width: parent.width }
                            Text { visible: modelData.metadataVerificationAttempted; text: modelData.verificationMatches ? "✓ 状态：一致" : "⚠ 状态：" + (modelData.lastError || "设备配置与现场配置不一致"); color: modelData.verificationMatches ? Theme.green : Theme.red; font.pixelSize: 10; font.weight: Font.DemiBold; elide: Text.ElideRight; width: parent.width }
                        }
                    }
                }
            }
        }
        footer: Item { implicitHeight: 44; HmiButton { anchors.right: parent.right; anchors.rightMargin: 10; anchors.verticalCenter: parent.verticalCenter; text: "关闭"; implicitWidth: 80; onClicked: verificationDialog.close() } }
    }

    Component {
        id: pointsPane
        ColumnLayout {
            anchors.fill: parent; anchors.margins: 14; spacing: 9
            Text { text: "运行点管理"; color: Theme.textPrimary; font.pixelSize: 15; font.weight: Font.DemiBold }
            Text { Layout.fillWidth: true; text: "软件默认只监测。选择运行点后，只有明确点击“开始控制”且全部设备通过预检，才会下发受限数字控制。"; color: Theme.textSecondary; font.pixelSize: 11; wrapMode: Text.Wrap }
            Text { Layout.fillWidth: true; text: "允许的生产写命令仅为 Digital CM=1、Hold/Follow=0/1、Digital Setpoint；不会写 Default CM、EEPROM、阀门、气体、量程、地址或波特率。"; color: Theme.textSecondary; font.pixelSize: 10; wrapMode: Text.Wrap }
            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 65; color: Theme.secondaryBackground; border.color: Theme.border; radius: Theme.radius
                RowLayout { anchors.fill: parent; anchors.margins: 10
                    ColumnLayout { Layout.fillWidth: true; spacing: 1
                        Text { text: appController.currentPointName || "未选择运行点"; color: Theme.primary; font.pixelSize: 14; font.weight: Font.DemiBold }
                        Text { text: appController.currentPointId ? "选择后仍为监测；点击“开始控制”才会请求受限控制" : "请选择或创建一个客户运行点"; color: appController.currentPointId ? Theme.green : Theme.yellow; font.pixelSize: 10 }
                    }
                    HmiButton { text: "进入运行点管理"; primary: true; implicitWidth: 145; implicitHeight: 44; onClicked: root.operatingPointsRequested() }
                }
            }
            Item { Layout.fillHeight: true }
        }
    }

    Component {
        id: devicePane
        ScrollView {
            anchors.fill: parent; anchors.margins: 8; clip: true
            contentWidth: availableWidth
            ColumnLayout {
                width: parent.width; spacing: 0
                ReadoutRow { label: "设备类型"; value: "CS200 真机"; valueColor: Theme.green }
                ReadoutRow { label: "设备状态"; value: appController.deviceStatusText; valueColor: (appController.deviceInfo.communicationHealth || 0) <= 1 ? Theme.red : (appController.deviceInfo.communicationHealth === 2 ? Theme.yellow : Theme.green) }
                ReadoutRow { label: "USB 串口"; value: appController.deviceInfo.port || "--" }
                ReadoutRow { label: "通信 / 运行状态"; value: (appController.deviceInfo.baudRate || "--") + " baud / " + (appController.deviceInfo.operationStateText || "空闲") }
                ReadoutRow { label: "MFC 通信正常"; value: (appController.deviceInfo.onlineCount || 0) + " / " + (appController.deviceInfo.deviceCount || appController.mfcDevices.length) }
                Repeater {
                    model: appController.deviceInfo.devices || []
                    delegate: Item {
                        required property var modelData
                        Layout.fillWidth: true; Layout.preferredHeight: modelData.metadataVerificationAttempted ? 184 : 76
                        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.divider }
                        Column {
                            anchors.left: parent.left; anchors.leftMargin: 10; anchors.right: parent.right; anchors.rightMargin: 105; anchors.verticalCenter: parent.verticalCenter; spacing: 2
                            Text { width: parent.width; elide: Text.ElideRight; text: modelData.displayName + "  逻辑通道 " + modelData.logicalChannel + "  协议地址 " + modelData.protocolAddress; color: Theme.textPrimary; font.pixelSize: 11; font.weight: Font.DemiBold }
                            Text { width: parent.width; elide: Text.ElideRight; text: modelData.addressSource + " · " + modelData.configurationStateText; color: modelData.engineeringConfigured ? Theme.green : Theme.yellow; font.pixelSize: 9 }
                            Text { width: parent.width; elide: Text.ElideRight; text: "成功 " + (modelData.successCount || 0) + "  失败 " + (modelData.errorCount || 0) + "  响应 " + Number(modelData.responseTimeMs || 0).toFixed(0) + " ms"; color: Theme.textSecondary; font.pixelSize: 9 }
                            Text {
                                width: parent.width; visible: modelData.metadataVerificationAttempted; elide: Text.ElideRight
                                property var targetScale: (modelData.metadataResults || {})["Target Full Scale"] || ({})
                                text: !modelData.metadataVerificationComplete ? "设备信息核验：无法完成（控制保持锁定）"
                                      : modelData.verificationMatches ? "设备信息核验：一致"
                                      : targetScale.status === "MISMATCH" ? "Target Gas Full Scale 与现场配置不一致（控制锁定）"
                                      : "设备信息核验：配置不一致（控制锁定）"
                                color: !modelData.metadataVerificationComplete ? Theme.yellow : (modelData.verificationMatches ? Theme.green : Theme.red)
                                font.pixelSize: 9
                            }
                            Text {
                                width: parent.width; visible: modelData.metadataVerificationAttempted; elide: Text.ElideRight
                                property var targetName: (modelData.metadataResults || {})["Target Gas Name"] || ({})
                                property var targetCode: (modelData.metadataResults || {})["Target Gas Code"] || ({})
                                text: "Target Gas  Name: " + (targetName.reported || "--") + "  Code Expected/Reported: " + (targetCode.expected || "--") + "/" + (targetCode.reported || "--") + "  状态: " + (targetCode.statusText || "未读取")
                                color: targetCode.status === "MISMATCH" ? Theme.red : (targetCode.reportedAvailable ? Theme.green : Theme.yellow); font.pixelSize: 9
                            }
                            Text {
                                width: parent.width; visible: modelData.metadataVerificationAttempted; elide: Text.ElideRight
                                property var result: (modelData.metadataResults || {})["Target Full Scale"] || ({})
                                text: "Target Full Scale  Expected: " + (result.expected || "--") + " SCCM  Reported: " + (result.reported || "--") + " SCCM  状态: " + (result.statusText || "未读取")
                                color: result.status === "MISMATCH" ? Theme.red : (result.reportedAvailable ? Theme.green : Theme.yellow); font.pixelSize: 9
                            }
                            Text {
                                width: parent.width; visible: modelData.metadataVerificationAttempted; elide: Text.ElideRight
                                property var calibrationName: (modelData.metadataResults || {})["Calibration Gas Name"] || ({})
                                property var calibrationCode: (modelData.metadataResults || {})["Calibration Gas Code"] || ({})
                                text: "Calibration Gas  Name: " + (calibrationName.reported || "--") + "  Code: " + (calibrationCode.reported || "--") + "  状态: " + (calibrationCode.statusText || "未读取")
                                color: calibrationCode.reportedAvailable ? Theme.green : Theme.yellow; font.pixelSize: 9
                            }
                            Text {
                                width: parent.width; visible: modelData.metadataVerificationAttempted; elide: Text.ElideRight
                                property var target: (modelData.metadataResults || {})["Target Full Scale"] || ({})
                                property var calibration: (modelData.metadataResults || {})["Calibration Full Scale"] || ({})
                                text: "Calibration Full Scale  Reported: " + (calibration.reported || "--") + " SCCM  状态: " + (calibration.statusText || "未读取")
                                color: calibration.reportedAvailable ? Theme.green : Theme.yellow; font.pixelSize: 9
                            }
                            Text {
                                width: parent.width
                                property var target: (modelData.metadataResults || {})["Target Full Scale"] || ({})
                                property var calibration: (modelData.metadataResults || {})["Calibration Full Scale"] || ({})
                                visible: modelData.metadataVerificationAttempted && target.reportedAvailable && calibration.reportedAvailable && target.reported !== calibration.reported
                                // Target-gas range and calibration-gas range are independent
                                // device metadata.  A difference is expected for many gases,
                                // and is not a configuration or communication warning.
                                text: "设备标定信息：Target 与 Calibration 量程不同"
                                color: Theme.textSecondary; font.pixelSize: 9
                            }
                        }
                        Column {
                            anchors.right: parent.right; anchors.rightMargin: 10; anchors.verticalCenter: parent.verticalCenter; spacing: 3
                            Text { anchors.right: parent.right; text: modelData.linkStateText; color: modelData.communicationOnline ? Theme.green : (modelData.linkState === 3 || modelData.linkState === 4 ? Theme.offline : Theme.red); font.pixelSize: 11; font.weight: Font.DemiBold }
                            HmiButton {
                                visible: modelData.addressDetected && !modelData.addressConfirmed
                                text: "确认此地址"; implicitWidth: 82; implicitHeight: 27; font.pixelSize: 10
                                onClicked: {
                                    root.pendingConfirmAddress = modelData.protocolAddress
                                    confirmAddressDialog.message = "确认“" + modelData.displayName + "”（逻辑通道 " + modelData.logicalChannel + "）确实对应协议地址 " + modelData.protocolAddress + "？\n\n此操作只确认地址映射，不会向 MFC 写入目标流量。"
                                    confirmAddressDialog.open()
                                }
                            }
                        }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true; Layout.preferredHeight: 70; Layout.leftMargin: 10; Layout.rightMargin: 8
                    Text { Layout.fillWidth: true; text: "设备信息核验仅读取设备报告，不会修改气体、量程、地址或 EEPROM。"; color: Theme.textSecondary; font.pixelSize: 10; wrapMode: Text.Wrap }
                    HmiButton { text: "设备信息核验"; implicitWidth: 104; implicitHeight: 44; enabled: !appController.controlling; onClicked: { appController.verifyDeviceInformation(); verificationDialog.open() } }
                    HmiButton { text: "重新扫描"; primary: true; implicitWidth: 92; implicitHeight: 44; enabled: true; onClicked: appController.rescanMfc() }
                }
            }
        }
    }

    Component {
        id: displayPane
        ColumnLayout {
            anchors.fill: parent; anchors.margins: 8; spacing: 0
            Item {
                Layout.fillWidth: true; Layout.preferredHeight: 58
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.divider }
                Text { anchors.left: parent.left; anchors.leftMargin: 10; anchors.verticalCenter: parent.verticalCenter; text: "窗口模式"; color: Theme.textPrimary; font.pixelSize: 11 }
                Row {
                    anchors.right: parent.right; anchors.rightMargin: 8; anchors.verticalCenter: parent.verticalCenter
                    spacing: 6
                    HmiButton {
                        text: "普通窗口"; implicitWidth: 82; implicitHeight: 34
                        primary: appController.windowMode === 0
                        onClicked: appController.windowMode = 0
                    }
                    HmiButton {
                        text: "无边框"; implicitWidth: 76; implicitHeight: 34
                        primary: appController.windowMode === 1
                        onClicked: appController.windowMode = 1
                    }
                    HmiButton {
                        text: "全屏显示"; implicitWidth: 82; implicitHeight: 34
                        primary: appController.windowMode === 2
                        onClicked: appController.windowMode = 2
                    }
                }
            }
            Text {
                Layout.fillWidth: true; Layout.margins: 10
                text: "普通窗口保留系统标题栏；无边框模式隐藏标题栏；全屏模式占满当前屏幕。选择后立即生效，下次启动时仍会保留。"
                color: Theme.textSecondary; font.pixelSize: 10; wrapMode: Text.Wrap
            }
            Item { Layout.fillHeight: true }
        }
    }

    Component {
        id: dataPane
        ColumnLayout {
            anchors.fill: parent; anchors.margins: 14; spacing: 10
            Text { text: "数据与导出"; color: Theme.textPrimary; font.pixelSize: 15; font.weight: Font.DemiBold }
            Text { Layout.fillWidth: true; text: "流量、通信和报警日志由现有日志服务持续写入；可按需导出报警记录。"; color: Theme.textSecondary; font.pixelSize: 11; wrapMode: Text.Wrap }
            ReadoutRow { label: "数据根目录"; value: appController.dataRoot }
            RowLayout {
                Layout.fillWidth: true; Layout.preferredHeight: 52; spacing: 8
                HmiButton { text: "导出报警 CSV"; Layout.fillWidth: true; implicitHeight: 44; primary: true; onClicked: appController.exportAlarms() }
            }
            Item { Layout.fillHeight: true }
        }
    }

    Component {
        id: aboutPane
        ColumnLayout {
            anchors.fill: parent; anchors.margins: 16; spacing: 7
            Text { text: "高浓度碳烟发生器监测系统"; color: Theme.primary; font.pixelSize: 18; font.weight: Font.Bold }
            Text { text: "版本 1.0.0"; color: Theme.textPrimary; font.pixelSize: 12 }
            Text { Layout.fillWidth: true; text: "面向 Raspberry Pi 5 与 5 英寸触摸屏的工业流量监测与控制界面。"; color: Theme.textSecondary; font.pixelSize: 11; wrapMode: Text.Wrap }
            Item { Layout.fillHeight: true }
        }
    }
}
