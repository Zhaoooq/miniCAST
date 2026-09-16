pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import MiniCastMonitor

Item {
    id: root
    property int currentIndex: 0
    signal pageRequested(int index)
    implicitHeight: 54

    Rectangle {
        id: navigationContainer
        objectName: "bottomNavigationContainer"
        anchors.centerIn: parent
        // Keep the navigation compact while preserving proportional margins.
        // Align the width to four physical pixels so equal columns do not pick
        // up one-pixel rounding differences at narrower window sizes.
        width: 4 * Math.floor(Math.max(0, Math.min(root.width * 0.9,
                                                   root.width - 32)) / 4)
        height: 48
        radius: 6
        color: Theme.surface
        border.width: 1
        border.color: Theme.border

        RowLayout {
            anchors.fill: parent
            anchors.margins: 3
            spacing: 4

            Repeater {
                model: [
                    { label: "监测", icon: "monitor" },
                    { label: "运行点", icon: "layers" },
                    { label: "报警", icon: "alarm" },
                    { label: "设置", icon: "settings" }
                ]

                delegate: HmiNavButton {
                    required property var modelData
                    required property int index
                    objectName: "bottomNavigationButton" + index
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    // Identical preferred widths make RowLayout divide all
                    // available space equally, independent of label metrics.
                    Layout.preferredWidth: 1
                    Layout.minimumWidth: 0
                    text: modelData.label
                    iconName: modelData.icon
                    selected: root.currentIndex === index
                    onClicked: root.pageRequested(index)
                }
            }
        }
    }
}
