import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MiniCastMonitor

HmiDialog {
    id: control
    signal valueAccepted(var value)

    property string fieldLabel: "数值"
    property string unit: ""
    property string inputText: ""
    property real minimumValue: 0
    property real maximumValue: Number.MAX_VALUE
    property int decimals: 2
    property bool optional: false
    property var quickSteps: []

    function openFor(label, value, minimum, maximum, valueUnit, allowEmpty, precision, steps) {
        fieldLabel = label
        unit = valueUnit || ""
        minimumValue = minimum
        maximumValue = maximum
        optional = allowEmpty
        decimals = precision
        quickSteps = steps || []
        inputText = value === null || value === undefined ? "" : Number(value).toFixed(decimals)
        title = "设置" + fieldLabel
        open()
    }

    function appendKey(key) {
        if (key === ".") {
            if (inputText.indexOf(".") >= 0) return
            inputText = inputText.length ? inputText + "." : "0."
            return
        }
        if (inputText === "0") inputText = key
        else inputText += key
    }

    function adjusted(delta) {
        var base = validNumber ? Number(inputText) : minimumValue
        var next = Math.max(minimumValue, Math.min(maximumValue, base + delta))
        var factor = Math.pow(10, decimals)
        inputText = String(Math.round(next * factor) / factor)
    }

    readonly property bool validNumber: {
        if (!inputText.length || inputText === ".") return false
        var value = Number(inputText)
        return isFinite(value) && value >= minimumValue && value <= maximumValue
    }

    width: Math.min(430, Overlay.overlay ? Overlay.overlay.width - 24 : 430)
    padding: 8
    acceptText: "确定"
    acceptEnabled: validNumber
    onAccepted: valueAccepted(Number(inputText))

    contentItem: ColumnLayout {
        spacing: 4

        Rectangle {
            Layout.fillWidth: true; Layout.preferredHeight: 46
            color: Theme.secondaryBackground; border.color: control.validNumber ? Theme.primary : Theme.border; radius: 3
            RowLayout {
                anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 12
                Text { text: control.inputText.length ? control.inputText : "---"; color: Theme.textPrimary; font.family: "monospace"; font.pixelSize: 25; font.weight: Font.DemiBold; Layout.fillWidth: true; horizontalAlignment: Text.AlignRight }
                Text { text: control.unit; visible: text.length > 0; color: Theme.textSecondary; font.pixelSize: 14 }
            }
        }

        RowLayout {
            Layout.fillWidth: true; spacing: 4; visible: control.quickSteps.length > 0
            Repeater {
                model: control.quickSteps
                delegate: HmiButton {
                    required property var modelData
                    Layout.fillWidth: true; implicitHeight: 48
                    text: Number(modelData) > 0 ? "+" + modelData : String(modelData)
                    onClicked: control.adjusted(Number(modelData))
                }
            }
        }

        GridLayout {
            Layout.fillWidth: true; columns: 3; columnSpacing: 4; rowSpacing: 3
            Repeater {
                model: ["1", "2", "3", "4", "5", "6", "7", "8", "9", ".", "0", "←"]
                delegate: HmiButton {
                    required property string modelData
                    Layout.fillWidth: true; implicitHeight: 48
                    text: modelData; font.pixelSize: 18
                    onClicked: {
                        if (modelData === "←") control.inputText = control.inputText.slice(0, -1)
                        else control.appendKey(modelData)
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true; spacing: 4
            HmiButton { text: "清空输入"; Layout.fillWidth: true; implicitHeight: 48; onClicked: control.inputText = "" }
            HmiButton {
                text: "清除数据"; Layout.fillWidth: true; implicitHeight: 48
                visible: control.optional
                onClicked: { control.valueAccepted(null); control.close() }
            }
        }
    }
}
