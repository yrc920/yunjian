import QtQuick

// 书脊入口：默认 52 的纯图标；书脊导航用 showLabel 变成 60×58 的图标 + 文字。
// 选中靠左侧 2px 纸线与轻微提亮表达，不铺整块纸色，避免顶部入口看起来像应用图标。
// 徽标只做未读，提示统一走 AppToolTip。
Rectangle {
    id: root

    property bool active: false
    property int badge: 0
    property string kind: "chat"
    property bool showLabel: false
    readonly property string label: kind === "settings" ? "设置" : kind === "chat" ? "聊天" : "联系人"
    signal clicked()

    width: showLabel ? 60 : 52
    height: showLabel ? 58 : 52
    radius: 12
    activeFocusOnTab: true
    color: active ? Theme.spineActive
                  : (area.containsMouse ? Theme.spineHover : "transparent")
    border.width: activeFocus ? 1 : 0
    border.color: Theme.spineText
    Accessible.role: Accessible.Button
    Accessible.name: label
    Accessible.onPressAction: root.clicked()
    Keys.onSpacePressed: root.clicked()
    Keys.onReturnPressed: root.clicked()

    Behavior on color { ColorAnimation { duration: 120 } }

    // 选中线：只占按钮左侧，和列表选中态是同一套语言
    Rectangle {
        visible: root.active
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        width: 2
        height: root.showLabel ? 28 : 22
        radius: 1
        color: Theme.spineText
    }

    AppIcon {
        anchors.horizontalCenter: parent.horizontalCenter
        y: root.showLabel ? 10 : (parent.height - height) / 2
        size: 24
        name: root.kind === "contact" ? "contacts" : root.kind
        color: root.active ? Theme.spineText : Theme.spineMuted
        opacity: root.active ? 1 : 0.92
    }

    Text {
        visible: root.showLabel
        anchors.horizontalCenter: parent.horizontalCenter
        y: 37
        text: root.label
        color: root.active ? Theme.spineText : Theme.spineMuted
        font.pixelSize: Theme.fontCaption
        font.weight: root.active ? Font.Medium : Font.Normal
    }

    Rectangle {
        visible: root.badge > 0
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.rightMargin: -3
        anchors.topMargin: -2
        width: Math.max(18, countLabel.implicitWidth + 10)
        height: 18
        radius: 9
        color: Theme.seal
        border.width: 1
        border.color: Theme.spine

        Text {
            id: countLabel
            anchors.centerIn: parent
            text: root.badge > 99 ? "99+" : root.badge
            color: Theme.onSeal
            font.pixelSize: Theme.fontCaption
            font.weight: Font.Medium
        }
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: {
            root.forceActiveFocus()
            root.clicked()
        }
    }

    AppToolTip {
        text: root.label
        hovered: !root.showLabel && area.containsMouse
    }
}
