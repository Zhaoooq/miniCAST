import QtQuick
import QtQuick.Controls
import MiniCastMonitor

Button {
    id: control
    property string iconName: "monitor"
    property bool selected: false
    implicitHeight: 42
    padding: 0
    spacing: 0

    background: Rectangle {
        radius: 4
        color: control.selected ? Theme.primary : (control.pressed ? "#D2DCE2" : Theme.tableHeader)
        border.width: 1
        border.color: control.selected ? Theme.primaryHover : Theme.border
    }

    contentItem: Item {
        Row {
            anchors.centerIn: parent
            spacing: 8

            Canvas {
                width: 24
                height: 24
                anchors.verticalCenter: parent.verticalCenter
                property color ink: control.selected ? "white" : "#52636E"
                onInkChanged: requestPaint()
                onPaint: {
                    var c = getContext("2d")
                    c.reset(); c.strokeStyle = ink; c.fillStyle = ink; c.lineWidth = 2; c.lineCap = "round"; c.lineJoin = "round"
                    if (control.iconName === "monitor") {
                        c.beginPath(); c.moveTo(3,10); c.lineTo(11,3); c.lineTo(19,10); c.stroke()
                        c.strokeRect(5,9,12,10); c.fillRect(9,13,4,6)
                    } else if (control.iconName === "layers") {
                        c.strokeRect(4,4,15,4); c.strokeRect(4,10,15,4); c.strokeRect(4,16,15,4)
                    } else if (control.iconName === "alarm") {
                        c.beginPath(); c.moveTo(6,16); c.lineTo(7,9); c.quadraticCurveTo(7,4,11,4); c.quadraticCurveTo(15,4,15,9); c.lineTo(16,16); c.stroke()
                        c.beginPath(); c.moveTo(4,16); c.lineTo(18,16); c.stroke(); c.fillRect(10,18,2,2)
                    } else {
                        c.beginPath(); c.arc(11,11,4,0,Math.PI*2); c.stroke()
                        for (var i=0;i<8;i++) { var a=i*Math.PI/4; c.beginPath(); c.moveTo(11+6*Math.cos(a),11+6*Math.sin(a)); c.lineTo(11+9*Math.cos(a),11+9*Math.sin(a)); c.stroke() }
                    }
                }
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: control.text
                color: control.selected ? "white" : "#374651"
                font.family: Theme.fontFamily
                font.pixelSize: 16
                font.weight: Font.DemiBold
            }
        }
    }
}
