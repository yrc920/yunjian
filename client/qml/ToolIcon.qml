import QtQuick

// 编辑区工具入口，未实现的动作保持不可用。
Rectangle {
    id: root

    property string kind: "emoji"
    property bool usable: false
    signal clicked()

    readonly property string tipText: kind === "emoji" ? "表情"
        : kind === "image" ? "插入图片"
        : kind === "video" ? "发送视频"
        : kind === "folder" ? "发送文件"
        : kind === "close" ? "移除" : kind

    width: 32
    height: 32
    radius: Theme.controlRadius
    activeFocusOnTab: usable
    color: usable && (area.containsMouse || activeFocus) ? Theme.panelAlt : "transparent"
    opacity: usable ? 1 : 0.42
    Accessible.role: Accessible.Button
    Accessible.name: kind === "emoji" ? "表情" : "文件"
    Accessible.onPressAction: if (root.usable) root.clicked()
    Keys.onSpacePressed: if (usable) clicked()
    Keys.onReturnPressed: if (usable) clicked()

    AppToolTip {
        text: root.tipText
        hovered: area.containsMouse
    }

    AppIcon {
        anchors.centerIn: parent
        size: 19
        name: root.kind
        color: Theme.textSecondary
    }

    MouseArea {
        id: area
        anchors.fill: parent
        enabled: root.usable
        hoverEnabled: true
        cursorShape: root.usable ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked: root.clicked()
    }
}
