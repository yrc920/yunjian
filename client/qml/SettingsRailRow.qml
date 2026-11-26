import QtQuick
import YunJian

// 设置栏的一行：40 前置块 + 标题/说明 + 右箭头。
// 选中铺纸卡 + 左侧 2px 墨线，悬停叠一层 selected；不画焦点边框。
Rectangle {
    id: root

    property bool selected: false
    //前置块二选一：头像或图标块
    property bool showAvatar: false
    property string avatarSource: ""
    property string avatarName: ""
    property string icon: ""
    property string title: ""
    //一行小字说明这个入口里有什么，产品上比只写标题更好判断
    property string caption: ""
    signal clicked()

    width: parent ? parent.width : 0
    height: 72
    //根是 Rectangle：不显式透明，未选中的行会露出整条白底
    color: "transparent"
    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Accessible.name: root.title
    Accessible.onPressAction: root.clicked()
    Keys.onSpacePressed: root.clicked()
    Keys.onReturnPressed: root.clicked()

    Rectangle {
        anchors.fill: parent
        anchors.topMargin: 2
        anchors.bottomMargin: 2
        radius: Theme.panelRadius
        color: root.selected ? Theme.panelBg : "transparent"
        border.width: root.selected ? 1 : 0
        border.color: Theme.line
    }

    Rectangle {
        anchors.fill: parent
        anchors.topMargin: 2
        anchors.bottomMargin: 2
        radius: Theme.panelRadius
        color: Theme.selected
        opacity: !root.selected && (area.containsMouse || root.activeFocus) ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 120 } }
    }

    Rectangle {
        visible: root.selected
        x: 0
        y: 12
        width: 2
        height: parent.height - 24
        radius: 1
        color: Theme.primary
    }

    Avatar {
        visible: root.showAvatar
        x: 12
        anchors.verticalCenter: parent.verticalCenter
        width: 40
        height: 40
        radius: 10
        source: root.avatarSource
        name: root.avatarName
        textSize: 13
    }

    Rectangle {
        visible: !root.showAvatar
        x: 12
        anchors.verticalCenter: parent.verticalCenter
        width: 40
        height: 40
        radius: 10
        //比索引底色深一档，未选中的行上也能看见
        color: Theme.selected

        AppIcon {
            anchors.centerIn: parent
            name: root.icon
            size: 20
            color: Theme.textSecondary
        }
    }

    Column {
        x: 64
        width: Math.max(0, root.width - 64 - 40)
        anchors.verticalCenter: parent.verticalCenter
        spacing: 4

        Text {
            width: parent.width
            text: root.title
            textFormat: Text.PlainText
            color: Theme.textPrimary
            font.pixelSize: Theme.fontBody
            font.weight: root.selected ? Font.Medium : Font.Normal
            elide: Text.ElideRight
        }
        Text {
            width: parent.width
            visible: root.caption.length > 0
            text: root.caption
            textFormat: Text.PlainText
            color: Theme.textSecondary
            font.pixelSize: Theme.fontCaption
            elide: Text.ElideRight
        }
    }

    AppIcon {
        anchors.right: parent.right
        anchors.rightMargin: 14
        anchors.verticalCenter: parent.verticalCenter
        name: "chevronRight"
        size: 14
        color: Theme.textHint
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        // 鼠标点击不把焦点留在行上；键盘 Tab 才显示焦点态
        onClicked: root.clicked()
    }
}
