import QtQuick
import XiguChat

// 设置索引沿用聊天列表的宽度，当前选项对应右侧个人资料。
// 「个人资料」是唯一分栏，外观切换作为本机快捷开关固定在其下方。
Rectangle {
    id: root
    property var controller

    color: Theme.panelBg
    implicitWidth: 280

    Text {
        x: 22
        y: 21
        text: "设置"
        color: Theme.textPrimary
        font.pixelSize: 22
        font.bold: true
    }
    Text {
        x: 23
        y: 51
        text: "资料与外观"
        color: Theme.textSecondary
        font.pixelSize: Theme.fontSmall
    }

    // 设置分栏：资料与通用。选中态由 pane 决定，切换后在会话/联系人之间来回也不丢
    readonly property int currentPane: root.controller ? root.controller.pane : -1
    readonly property bool profileSelected: currentPane === ChatBridge.UserInfoPage
    readonly property bool generalSelected: currentPane === ChatBridge.GeneralPage
    readonly property string appearanceLabel: Theme.mode === "system" ? "跟随系统"
        : Theme.mode === "dark" ? "深色" : "浅色"

    Rectangle {
        id: profileRow
        x: 12
        y: 96
        width: parent.width - 24
        height: 58
        radius: 10
        color: profileSelected ? Theme.selected
                               : (profileArea.containsMouse ? Theme.panelAlt : "transparent")

        AppIcon {
            x: 17
            anchors.verticalCenter: parent.verticalCenter
            name: "user"
            size: 22
            color: profileSelected ? Theme.primary : Theme.textSecondary
        }
        Column {
            x: 54
            width: parent.width - x - 28
            anchors.verticalCenter: parent.verticalCenter
            spacing: 4
            Text {
                text: "个人资料"
                color: Theme.textPrimary
                font.pixelSize: Theme.fontBody
                font.weight: profileSelected ? Font.DemiBold : Font.Normal
            }
            Text {
                text: "头像与账号信息"
                color: Theme.textSecondary
                font.pixelSize: 11
            }
        }
        MouseArea {
            id: profileArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: if (root.controller) root.controller.showSettings()
        }
    }

    Rectangle {
        id: generalRow
        x: 12
        y: 160
        width: parent.width - 24
        height: 58
        radius: 10
        color: generalSelected ? Theme.selected
                               : (generalArea.containsMouse ? Theme.panelAlt : "transparent")

        AppIcon {
            x: 17
            anchors.verticalCenter: parent.verticalCenter
            name: "settings"
            size: 21
            color: generalSelected ? Theme.primary : Theme.textSecondary
        }
        Column {
            x: 54
            width: parent.width - x - 28
            anchors.verticalCenter: parent.verticalCenter
            spacing: 4
            Text {
                text: "通用"
                color: Theme.textPrimary
                font.pixelSize: Theme.fontBody
                font.weight: generalSelected ? Font.DemiBold : Font.Normal
            }
            Text {
                text: "本地存储与缓存"
                color: Theme.textSecondary
                font.pixelSize: 11
            }
        }
        MouseArea {
            id: generalArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: if (root.controller) root.controller.openGeneralSettings()
        }
    }

    Rectangle {
        x: 22
        y: 230
        width: parent.width - 44
        height: 1
        color: Theme.line
    }

    // 外观入口：三选一（跟随系统 / 浅色 / 深色），直接写本机配置，不经过服务器
    Rectangle {
        id: appearance
        x: 12
        y: 247
        width: parent.width - 24
        height: 58
        radius: 10
        color: appearanceArea.containsMouse || activeFocus || appearanceMenu.opened ? Theme.panelAlt : "transparent"
        border.width: activeFocus ? 1 : 0
        border.color: Theme.primary
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: "外观"
        Accessible.onPressAction: appearanceMenu.open()
        Keys.onSpacePressed: appearanceMenu.open()
        Keys.onReturnPressed: appearanceMenu.open()

        AppIcon {
            x: 17
            anchors.verticalCenter: parent.verticalCenter
            name: Theme.dark ? "moon" : "sun"
            size: 21
            color: Theme.textSecondary
        }
        Text {
            x: 54
            anchors.verticalCenter: parent.verticalCenter
            text: "外观"
            color: Theme.textPrimary
            font.pixelSize: Theme.fontBody
        }
        Text {
            anchors.right: parent.right
            anchors.rightMargin: 17
            anchors.verticalCenter: parent.verticalCenter
            text: root.appearanceLabel
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSmall
        }
        MouseArea {
            id: appearanceArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: {
                appearance.forceActiveFocus()
                appearanceMenu.open()
            }
        }
        AppearanceMenu {
            id: appearanceMenu
            x: appearance.width - width - 12
            y: appearance.height + 4
        }
    }

    Rectangle {
        x: 22
        y: 313
        width: parent.width - 44
        height: 1
        color: Theme.line
    }

    // 退出登录：断开连接、清空账号数据后回到登录页，不会弹"连接已断开"的提示
    Rectangle {
        id: logoutRow
        x: 12
        y: 330
        width: parent.width - 24
        height: 54
        radius: 10
        color: logoutArea.containsMouse ? Theme.panelAlt : "transparent"
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: "退出登录"
        Accessible.onPressAction: root.controller ? root.controller.askLogout() : undefined
        Keys.onSpacePressed: if (root.controller) root.controller.askLogout()
        Keys.onReturnPressed: if (root.controller) root.controller.askLogout()

        AppIcon {
            x: 17
            anchors.verticalCenter: parent.verticalCenter
            name: "back"
            size: 20
            color: Theme.danger
        }
        Text {
            x: 54
            anchors.verticalCenter: parent.verticalCenter
            text: "退出登录"
            color: Theme.danger
            font.pixelSize: Theme.fontBody
        }
        MouseArea {
            id: logoutArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: {
                logoutRow.forceActiveFocus()
                if (root.controller)
                    root.controller.askLogout()
            }
        }
    }

    Rectangle {
        anchors.right: parent.right
        width: 1
        height: parent.height
        color: Theme.line
    }
}
