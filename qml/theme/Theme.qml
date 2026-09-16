pragma Singleton
import QtQuick

QtObject {
    readonly property color background: "#EEF3F6"
    readonly property color surface: "#FFFFFF"
    readonly property color surfaceRaised: "#FFFFFF"
    readonly property color secondaryBackground: "#F5F8FA"
    readonly property color tableHeader: "#E9EFF3"
    readonly property color header: tableHeader
    readonly property color border: "#CDD9E1"
    readonly property color divider: "#DFE7EC"
    readonly property color textPrimary: "#17334D"
    readonly property color textSecondary: "#667C8D"
    // 北航蓝（CMYK C100 M60 Y0 K0）在屏幕界面的 RGB 主色。
    // 所有品牌和交互强调色均从这里派生，避免页面各自定义蓝色。
    readonly property color primary: "#0066FF"
    readonly property color primaryHover: "#0052CC"
    readonly property color primaryDark: "#0047B3"
    readonly property color primarySelection: "#E6F0FF"
    readonly property color primaryPressedBackground: "#D6E6FF"
    // Gauges use a restrained derivative of the primary colour.  The solid
    // primary colour is reserved for the live-flow marker and interactions.
    readonly property color gaugeFill: "#660066FF"
    readonly property color gaugeTargetBand: "#1A0066FF"
    readonly property color gaugeTargetBorder: "#400066FF"
    readonly property color targetFlow: "#F28C18"
    readonly property color green: "#159564"
    readonly property color yellow: "#E58A16"
    readonly property color red: "#D83C3C"
    readonly property color offline: "#8A99A4"
    readonly property color warningRow: "#FFF8E9"
    readonly property color criticalRow: "#FFF0EF"
    readonly property var channelPalette: [primary, "#F28C18", "#2DAA72", "#F0A51A",
                                           "#7358D8", "#008B95", "#C24C94", "#667C8D"]
    readonly property int radius: 5
    readonly property string fontFamily: "Noto Sans CJK SC"
    function channelColor(id) {
        var hash = 0
        for (var i = 0; i < id.length; ++i) hash = (hash * 31 + id.charCodeAt(i)) >>> 0
        return channelPalette[hash % channelPalette.length]
    }
    function statusColor(code) {
        if (code === 1) return yellow
        if (code === 2) return red
        if (code === 3) return offline
        if (code === 4 || code === 5) return yellow
        if (code === 6) return red
        return green
    }
}
