import QtQuick
import QtQuick.Controls

// 侧栏入口，选中状态用浅底色与主色图标区分；不再贴边画竖线，避免被窗口圆角裁成碎线。
Rectangle {
    id: root

    property bool active: false
    property int badge: 0
    property string kind: "chat"
    readonly property string label: kind === "settings" ? "设置" : kind === "chat" ? "聊天" : "联系人"
    signal clicked()

    width: 40
    height: 40
    radius: Theme.controlRadius
    activeFocusOnTab: true
    color: active ? Theme.selected : (area.containsMouse ? Theme.panelAlt : "transparent")
    border.width: activeFocus ? 1 : 0
    border.color: Theme.focusRing
    Accessible.role: Accessible.Button
    Accessible.name: label
    Accessible.onPressAction: root.clicked()
    Keys.onSpacePressed: root.clicked()
    Keys.onReturnPressed: root.clicked()

    Behavior on color { ColorAnimation { duration: 150 } }

    AppIcon {
        anchors.centerIn: parent
        size: 22
        name: root.kind === "contact" ? "contacts" : root.kind
        color: root.active ? Theme.primary : Theme.textSecondary
    }

    ToolTip.visible: area.containsMouse || activeFocus
    ToolTip.text: label
    ToolTip.delay: 450

    Rectangle {
        visible: root.badge > 0
        x: parent.width - width + 1
        y: 3
        width: Math.max(16, countLabel.implicitWidth + 8)
        height: 16
        radius: 8
        color: Theme.accent
        Text {
            id: countLabel
            anchors.centerIn: parent
            text: root.badge > 99 ? "99+" : root.badge
            color: Theme.primaryText
            font.pixelSize: 9
            font.bold: true
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
}
