import QtQuick
import QtQuick.Layouts
import MiniCastMonitor

Rectangle {
    id: root
    property var channel: ({})
    signal detailsRequested()
    signal targetEditRequested()

    readonly property real fullScale: Number(channel.fullScaleValue || 0)
    // Actual presentation has exactly one telemetry source: actualFlow.  Do
    // not use the legacy currentFlow compatibility field for the bar or text.
    readonly property bool actualAvailable: Boolean(channel.actualFlowFresh)
    readonly property bool targetAvailable: Boolean(channel.targetFlowAvailable)
    readonly property real targetFraction: fullScale > 0 && targetAvailable ? Math.max(0, Math.min(1, Number(channel.targetFlow || 0) / fullScale)) : 0
    readonly property real actualFraction: fullScale > 0 && actualAvailable ? Math.max(0, Math.min(1, Number(channel.actualFlow || 0) / fullScale)) : 0
    readonly property int commState: channel.communicationStateCode === undefined ? 0 : channel.communicationStateCode
    // Every address owns this lamp. Green is allowed only after its own
    // consecutive-response state machine reaches Normal.
    readonly property bool commNormal: commState === 1
    readonly property bool scanning: commState === 0
    readonly property color commColor: commNormal ? Theme.green : Theme.red
    readonly property string displayUnit: channel.flowUnit || ""
    readonly property string statusText: channel.comparisonStatusText || "正在确认通信"
    readonly property color statusColor: statusText === "正常" ? Theme.green
        : statusText === "偏高" ? Theme.red
        : statusText === "偏低" ? Theme.yellow
        : statusText === "未监测" || statusText === "请选择运行点" ? Theme.textSecondary
        : Theme.red

    color: Theme.surface
    radius: Theme.radius
    border.color: Theme.border
    border.width: 1
    clip: true

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 6
        spacing: 2

        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 18
            spacing: 4
            Rectangle {
                width: 9; height: 9; radius: 5; color: root.commColor
                SequentialAnimation on opacity {
                    running: root.scanning; loops: Animation.Infinite
                    NumberAnimation { to: 0.28; duration: 700 }
                    NumberAnimation { to: 1.0; duration: 700 }
                }
            }
            Text {
                Layout.fillWidth: true
                text: channel.gasType && channel.gasType !== "待确认" ? channel.gasType : "气体未配置"
                color: Theme.textPrimary; font.pixelSize: 12; font.weight: Font.DemiBold; elide: Text.ElideRight
            }
        }

        Text {
            Layout.fillWidth: true; Layout.preferredHeight: 15
            text: channel.name || "MFC —"
            color: Theme.textSecondary; font.pixelSize: 10; elide: Text.ElideRight
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 28
            spacing: 4
            ColumnLayout {
                id: targetFlowDisplay
                Layout.fillWidth: true; spacing: -2
                Text { Layout.fillWidth: true; text: root.targetAvailable ? "目标 " + Number(channel.targetFlow).toFixed(1) : "目标 —"; color: Theme.targetFlow; font.pixelSize: 11; font.weight: Font.DemiBold; horizontalAlignment: Text.AlignHCenter; elide: Text.ElideRight }
                Text { Layout.fillWidth: true; text: root.displayUnit; color: Theme.textSecondary; font.pixelSize: 9; horizontalAlignment: Text.AlignHCenter; elide: Text.ElideRight }
                // Keep the target area separate from the card-wide details
                // gesture, so a single touch edits the customer run point.
                TapHandler {
                    onTapped: root.targetEditRequested()
                }
            }
            ColumnLayout {
                Layout.fillWidth: true; spacing: -2
                Text { Layout.fillWidth: true; text: root.actualAvailable ? "实际 " + Number(channel.actualFlow).toFixed(1) : (channel.lastValidFlowTimestamp ? "实际 —" : "实际 —"); color: Theme.primary; font.pixelSize: 11; font.weight: Font.DemiBold; horizontalAlignment: Text.AlignHCenter; elide: Text.ElideRight }
                Text { Layout.fillWidth: true; text: root.actualAvailable ? root.displayUnit : ""; color: Theme.textSecondary; font.pixelSize: 9; horizontalAlignment: Text.AlignHCenter; elide: Text.ElideRight }
            }
        }

        Item {
            id: gauges
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 118
            // The removed address row leaves its height to this fill item,
            // making the gauges taller.  A wider track keeps the values
            // legible on the five-column main view.
            readonly property real trackWidth: Math.min(60, Math.max(24, width * 0.40))
            readonly property real trackY: 14
            readonly property real trackHeight: Math.max(1, height - trackY - 9)
            readonly property real targetX: width * 0.25 - trackWidth / 2
            readonly property real actualX: width * 0.75 - trackWidth / 2
            // Identical full scale, zero origin, height and formula for both bars.
            function fillY(fraction) { return trackY + trackHeight * (1 - fraction) }
            function markerY(fraction) { return Math.max(trackY + 1, Math.min(trackY + trackHeight - 3, fillY(fraction) - 1)) }

            // Wide tracks leave too little space at their left edge for
            // values such as 1.0 and 4.0.  Keep the shared scale legible by
            // anchoring it above and below the target-flow track instead.
            Text { x: gauges.targetX; y: 1; width: gauges.trackWidth; text: root.fullScale > 0 ? Number(root.fullScale).toFixed(root.fullScale < 10 ? 1 : 0) : "—"; color: Theme.textSecondary; font.pixelSize: 9; horizontalAlignment: Text.AlignHCenter; elide: Text.ElideNone }
            Text { x: gauges.targetX; y: gauges.trackY + gauges.trackHeight; width: gauges.trackWidth; text: "0"; color: Theme.textSecondary; font.pixelSize: 9; horizontalAlignment: Text.AlignHCenter }

            Repeater {
                model: [ { x: gauges.targetX, fraction: root.targetFraction, available: root.targetAvailable, color: Theme.targetFlow }, { x: gauges.actualX, fraction: root.actualFraction, available: root.actualAvailable, color: Theme.gaugeFill } ]
                delegate: Item {
                    required property var modelData
                    Rectangle { x: modelData.x; y: gauges.trackY; width: gauges.trackWidth; height: gauges.trackHeight; radius: 3; color: Theme.secondaryBackground; border.color: Theme.border; border.width: 1 }
                    Rectangle { visible: modelData.available && root.fullScale > 0; x: modelData.x + 1; y: gauges.fillY(modelData.fraction); width: gauges.trackWidth - 2; height: gauges.trackHeight * modelData.fraction; radius: 2; color: modelData.color }
                    Rectangle { visible: modelData.available && root.fullScale > 0; x: modelData.x - 3; y: gauges.markerY(modelData.fraction); width: gauges.trackWidth + 6; height: 3; color: modelData.color }
                }
            }
        }

        // Keep only the two flow-comparison values beneath the gauges. This
        // reserves the rest of the fixed-height card for the bar display.
        Text {
            Layout.fillWidth: true; Layout.preferredHeight: 15
            text: "偏差 " + (channel.deviationAvailable && root.actualAvailable ? (Number(channel.deviation) >= 0 ? "+" : "") + Number(channel.deviation).toFixed(2) : "—")
                + "  ·  " + (channel.deviationAvailable && root.actualAvailable ? (Number(channel.deviationPercent) >= 0 ? "+" : "") + Number(channel.deviationPercent).toFixed(1) + "%" : "偏差率 —")
            color: root.statusColor; font.pixelSize: 10
            horizontalAlignment: Text.AlignHCenter; elide: Text.ElideRight
        }
    }

    TapHandler {
        // Pointer handlers can both observe a tap.  Exclude the target area
        // explicitly so it never also opens the device-details dialog.
        onTapped: function(eventPoint) {
            var targetOrigin = targetFlowDisplay.mapToItem(root, 0, 0)
            var inTargetArea = eventPoint.position.x >= targetOrigin.x
                && eventPoint.position.x <= targetOrigin.x + targetFlowDisplay.width
                && eventPoint.position.y >= targetOrigin.y
                && eventPoint.position.y <= targetOrigin.y + targetFlowDisplay.height
            if (!inTargetArea) root.detailsRequested()
        }
    }
}
