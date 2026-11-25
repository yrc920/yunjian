import QtQuick
import XiguChat

// 设置索引：文字分组标签 + 单行条目 + 底部固定退出。
// 不再堆小字：每行只留标题和右箭头，账号 ID、外观值、版本号都收进各自的页面。
// 窄窗收成 72 头像条：入口贴顶，退出贴底。
Rectangle {
    id: root
    property var controller
    property bool compact: false
    signal aboutRequested()

    color: Theme.indexBg
    implicitWidth: Theme.indexWidth
    clip: true

    readonly property int currentPane: root.controller ? root.controller.pane : -1
    readonly property bool profileSelected: currentPane === ChatBridge.UserInfoPage
    readonly property bool generalSelected: currentPane === ChatBridge.GeneralPage

    // 完整设置索引
    Column {
        x: 8
        y: 0
        // 收起时保留完整排版宽度（内容透明且被裁），展开时跟随实际栏宽，避免 200 宽溢出
        width: root.compact ? 220 : Math.max(0, parent.width - 16)
        spacing: 0
        opacity: root.compact ? 0 : 1
        enabled: !root.compact
        Behavior on opacity { NumberAnimation { duration: 120 } }

        Item {
            width: parent.width
            height: Theme.toolbarHeight

            Text {
                x: 12
                y: 10
                text: "设置"
                color: Theme.textPrimary
                font.pixelSize: Theme.fontDisplay
                font.weight: Font.DemiBold
            }
            Text {
                x: 12
                y: 39
                width: parent.width - 24
                text: "账号资料与应用偏好"
                textFormat: Text.PlainText
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSmall
                elide: Text.ElideRight
            }
        }

        Item { width: parent.width; height: 8 }

        // 分组标签和联系人页的「全部联系人」同一种写法：只留一行小字，不加线
        Item {
            width: parent.width
            height: 26
            Text {
                id: accountLabel
                x: 12
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 6
                text: "账号"
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSmall
                font.weight: Font.Medium
            }
        }

        SettingsRailRow {
            width: parent.width
            selected: root.profileSelected
            showAvatar: true
            avatarSource: root.controller ? root.controller.myIcon : ""
            avatarName: root.controller ? root.controller.myName : ""
            title: root.controller ? root.controller.myName : ""
            caption: "查看并编辑资料"
            onClicked: {
                root.forceActiveFocus()
                if (root.controller)
                    root.controller.openMyProfile()
            }
        }

        Item { width: parent.width; height: 12 }

        Item {
            width: parent.width
            height: 26
            Text {
                id: appLabel
                x: 12
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 6
                text: "应用"
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSmall
                font.weight: Font.Medium
            }
        }

        SettingsRailRow {
            width: parent.width
            selected: root.generalSelected
            icon: "settings"
            title: "通用"
            caption: "外观与本地数据"
            onClicked: {
                root.forceActiveFocus()
                if (root.controller)
                    root.controller.openGeneralSettings()
            }
        }

        Item { width: parent.width; height: 12 }

        Item {
            width: parent.width
            height: 26
            Text {
                x: 12
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 6
                text: "关于"
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSmall
                font.weight: Font.Medium
            }
        }

        SettingsRailRow {
            width: parent.width
            icon: "brand"
            title: "关于信笺"
            caption: "版本与作者的话"
            onClicked: {
                root.forceActiveFocus()
                root.aboutRequested()
            }
        }
    }

    // 底部：退出固定在栏底，不跟着内容走
    Item {
        anchors.fill: parent
        visible: !root.compact

        Rectangle {
            x: 16
            width: Math.max(0, parent.width - 32)
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 60
            height: 1
            color: Theme.lineSoft
        }

        // 退出是收尾动作：从分隔线到栏底整块做悬停反馈，不留上下边距
        Item {
            anchors.left: parent.left
            anchors.right: parent.right
            height: 59
            anchors.bottom: parent.bottom

            Rectangle {
                anchors.fill: parent
                color: Theme.selected
                opacity: logoutArea.containsMouse ? 1 : 0
                Behavior on opacity { NumberAnimation { duration: 120 } }
            }

            Row {
                anchors.centerIn: parent
                spacing: 6

                AppIcon {
                    anchors.verticalCenter: parent.verticalCenter
                    name: "logout"
                    size: 16
                    color: Theme.danger
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "退出登录"
                    color: Theme.danger
                    font.pixelSize: Theme.fontBody
                    font.weight: Font.Medium
                }
            }

            MouseArea {
                id: logoutArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: if (root.controller) root.controller.askLogout()
            }
        }
    }

    // 窄窗：头像条。顶部两个入口和聊天/联系人的头像起点对齐，退出贴底
    Item {
        anchors.fill: parent
        opacity: root.compact ? 1 : 0
        enabled: root.compact
        Behavior on opacity { NumberAnimation { duration: 120 } }

        Item {
            id: compactProfile
            y: Theme.toolbarHeight + 6
            anchors.horizontalCenter: parent.horizontalCenter
            width: 48
            height: 48

            Rectangle {
                anchors.fill: parent
                radius: Theme.panelRadius
                color: root.profileSelected ? Theme.panelBg
                     : (compactProfileArea.containsMouse ? Theme.selected : "transparent")
                border.width: root.profileSelected ? 1 : 0
                border.color: Theme.line
            }
            Avatar {
                anchors.centerIn: parent
                width: 34
                height: 34
                radius: 10
                source: root.controller ? root.controller.myIcon : ""
                name: root.controller ? root.controller.myName : ""
                textSize: 12
            }
            MouseArea {
                id: compactProfileArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: if (root.controller) root.controller.openMyProfile()
            }
            AppToolTip {
                text: "个人资料"
                hovered: compactProfileArea.containsMouse
            }
        }

        Item {
            id: compactGeneral
            anchors.top: compactProfile.bottom
            anchors.topMargin: 14
            anchors.horizontalCenter: parent.horizontalCenter
            width: 48
            height: 48

            Rectangle {
                anchors.fill: parent
                radius: Theme.panelRadius
                color: root.generalSelected ? Theme.panelBg
                     : (compactGeneralArea.containsMouse ? Theme.selected : "transparent")
                border.width: root.generalSelected ? 1 : 0
                border.color: Theme.line
            }
            AppIcon {
                anchors.centerIn: parent
                name: "settings"
                size: 22
                color: root.generalSelected ? Theme.primary : Theme.textSecondary
            }
            MouseArea {
                id: compactGeneralArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: if (root.controller) root.controller.openGeneralSettings()
            }
            AppToolTip {
                text: "通用设置"
                hovered: compactGeneralArea.containsMouse
            }
        }

        // 收起时也要能到「关于」，否则窄窗里这一页就丢了
        Item {
            anchors.top: compactGeneral.bottom
            anchors.topMargin: 14
            anchors.horizontalCenter: parent.horizontalCenter
            width: 48
            height: 48

            Rectangle {
                anchors.fill: parent
                radius: Theme.panelRadius
                color: compactAboutArea.containsMouse ? Theme.selected : "transparent"
            }
            AppIcon {
                anchors.centerIn: parent
                name: "brand"
                size: 22
                color: Theme.textSecondary
            }
            MouseArea {
                id: compactAboutArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.aboutRequested()
            }
            AppToolTip {
                text: "关于信笺"
                hovered: compactAboutArea.containsMouse
            }
        }

        // 底部与书脊的本人头像同一条水平带：上面是页面入口，下面是退出
        Item {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 30
            width: 40
            height: 40

            Rectangle {
                anchors.fill: parent
                radius: Theme.panelRadius
                color: compactLogoutArea.containsMouse ? Theme.selected : "transparent"
            }
            AppIcon {
                anchors.centerIn: parent
                name: "logout"
                size: 18
                color: Theme.danger
            }
            MouseArea {
                id: compactLogoutArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: if (root.controller) root.controller.askLogout()
            }
            AppToolTip {
                text: "退出登录"
                hovered: compactLogoutArea.containsMouse
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
