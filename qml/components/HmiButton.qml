import QtQuick
import QtQuick.Controls
import MiniCastMonitor

Button {
    id: control
    property bool primary: false
    property bool danger: false
    property bool hmiFlat: false
    property bool selected: false
    property string iconName: ""
    implicitHeight: 40
    implicitWidth: Math.max(76, contentItem.implicitWidth + 24)
    leftPadding: 12; rightPadding: 12; topPadding: 0; bottomPadding: 0
    font.family: Theme.fontFamily
    font.pixelSize: 13
    font.weight: primary ? Font.Medium : Font.Normal

    contentItem: Item {
        implicitWidth: buttonContent.implicitWidth
        implicitHeight: buttonContent.implicitHeight
        Row {
            id: buttonContent
            anchors.centerIn: parent
            spacing: control.iconName.length > 0 ? 6 : 0
            layoutDirection: Qt.LeftToRight

            Canvas {
                visible: control.iconName.length > 0
                width: visible ? 16 : 0; height: 16
                anchors.verticalCenter: parent.verticalCenter
                property color ink: !control.enabled ? "#9AA6AE" : (control.primary || control.danger || control.selected ? "white" : Theme.textPrimary)
                onInkChanged: requestPaint()
                onPaint: {
                    var c = getContext("2d")
                    c.reset(); c.strokeStyle = ink; c.fillStyle = ink
                    c.lineWidth = 1.6; c.lineCap = "round"; c.lineJoin = "round"
                    if (control.iconName === "trash") {
                        c.beginPath(); c.moveTo(4,5); c.lineTo(12,5); c.lineTo(11.2,14); c.lineTo(4.8,14); c.closePath(); c.stroke()
                        c.beginPath(); c.moveTo(3,3); c.lineTo(13,3); c.moveTo(6,3); c.lineTo(6.7,1.5); c.lineTo(9.3,1.5); c.lineTo(10,3); c.stroke()
                        c.beginPath(); c.moveTo(7,7); c.lineTo(7,12); c.moveTo(9.5,7); c.lineTo(9.5,12); c.stroke()
                    }
                }
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: control.text
                color: !control.enabled ? "#9AA6AE" : (control.primary || control.danger || control.selected ? "white" : Theme.textPrimary)
                font: control.font
                elide: Text.ElideRight
            }
        }
    }
    background: Rectangle {
        radius: 2
        color: {
            if (!control.enabled) return "#E9EDF0"
            if (control.danger) return control.pressed ? "#AD3932" : Theme.red
            if (control.primary || control.selected) return control.pressed ? Theme.primaryHover : Theme.primary
            if (control.hmiFlat) return control.pressed ? Theme.primaryPressedBackground : "transparent"
            return control.pressed ? "#D5DDE2" : Theme.secondaryBackground
        }
        border.width: control.hmiFlat ? 0 : 1
        border.color: (control.primary || control.selected) ? Theme.primaryHover : (control.danger ? "#AD3932" : Theme.border)
    }
}
