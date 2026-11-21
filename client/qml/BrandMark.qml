import QtQuick

// 邮戳式印记：墨蓝圆底、内圈细线与纸白信封；20–32px 之间都能认出。
// 与桌面图标同源但不同形：图标是应用的脸，界面里这枚是落款。
Item {
    id: root
    property real size: 28
    // 外圆、内圈、信封都取整数像素且同奇偶：半像素会让圆环一侧看起来更粗，圆心像是偏的
    readonly property int side: Math.max(16, Math.round(size / 2) * 2)
    readonly property int ringSize: side - 6
    readonly property int glyphSize: Math.max(10, Math.round(side * 0.55 / 2) * 2)
    implicitWidth: side
    implicitHeight: side

    Rectangle {
        anchors.fill: parent
        radius: root.side / 2
        color: Theme.primary
    }
    Rectangle {
        anchors.centerIn: parent
        width: root.ringSize
        height: root.ringSize
        radius: root.ringSize / 2
        color: "transparent"
        border.width: 1
        border.color: Theme.primaryText
        opacity: 0.35
    }
    AppIcon {
        anchors.centerIn: parent
        name: "mail"
        size: root.glyphSize
        color: Theme.primaryText
    }
}
