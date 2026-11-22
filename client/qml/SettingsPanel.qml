import QtQuick
import XiguChat

// 设置索引与联系人/会话列表共用同一列宽和行节奏。
Rectangle {
    id: root
    property var controller

    color: Theme.panelBg
    implicitWidth: Theme.indexWidth

    Text {
        x: 20
        y: 10
        text: "设置"
        color: Theme.textPrimary
        font.pixelSize: Theme.fontDisplay
        font.weight: Font.DemiBold
    }
    Text {
        x: 20
        y: 39
        text: "账号与应用"
        color: Theme.textSecondary
        font.pixelSize: Theme.fontCaption
    }

    readonly property int currentPane: root.controller ? root.controller.pane : -1
    readonly property bool profileSelected: currentPane === ChatBridge.UserInfoPage
    readonly property bool generalSelected: currentPane === ChatBridge.GeneralPage
    readonly property string appearanceLabel: Theme.mode === "system" ? "跟随系统"
        : Theme.mode === "dark" ? "深色" : "浅色"

    Column {
        x: 12
        y: 80
        width: parent.width - 24
        spacing: 4

        SettingsRow {
            iconName: "user"
            title: "个人资料"
            subtitle: "头像与账号信息"
            selected: root.profileSelected
            onClicked: if (root.controller) root.controller.openMyProfile()
        }
        SettingsRow {
            iconName: "settings"
            title: "通用"
            subtitle: "本地记录与媒体缓存"
            selected: root.generalSelected
            onClicked: if (root.controller) root.controller.openGeneralSettings()
        }
    }

    Rectangle {
        x: 22
        y: 220
        width: parent.width - 44
        height: 1
        color: Theme.lineSoft
    }

    SettingsRow {
        id: appearanceRow
        x: 12
        y: 236
        width: parent.width - 24
        iconName: Theme.dark ? "moon" : "sun"
        title: "外观"
        value: root.appearanceLabel
        showChevron: true
        onClicked: {
            appearanceRow.forceActiveFocus()
            appearanceMenu.open()
        }

        AppearanceMenu {
            id: appearanceMenu
            x: appearanceRow.width - width - 10
            y: appearanceRow.height + 4
        }
    }

    Rectangle {
        x: 22
        y: 304
        width: parent.width - 44
        height: 1
        color: Theme.lineSoft
    }

    SettingsRow {
        id: logoutRow
        x: 12
        y: 320
        width: parent.width - 24
        iconName: "back"
        title: "退出登录"
        danger: true
        onClicked: if (root.controller) root.controller.askLogout()
    }

    Rectangle {
        anchors.right: parent.right
        width: 1
        height: parent.height
        color: Theme.line
    }
}
