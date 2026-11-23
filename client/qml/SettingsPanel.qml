import QtQuick
import XiguChat

// 设置索引：账号卡、外观三段选择、通用入口，底部退出登录。
// 窄窗时收成 60 的头像条：头像进资料，滑块进通用；外观仍可在展开后调整。
Rectangle {
    id: root
    property var controller
    property bool compact: false

    color: Theme.indexBg
    implicitWidth: Theme.indexWidth
    clip: true

    readonly property int currentPane: root.controller ? root.controller.pane : -1
    readonly property bool profileSelected: currentPane === ChatBridge.UserInfoPage
    readonly property bool generalSelected: currentPane === ChatBridge.GeneralPage

    readonly property var appearanceOptions: [
        { value: "light", label: "浅色" },
        { value: "dark", label: "深色" },
        { value: "system", label: "系统" }
    ]

    // 完整设置索引
    Column {
        x: 12
        y: 14
        // 窄窗时保留完整排版宽度，只靠裁剪和透明度隐藏，避免负宽度告警
        width: Math.max(220, parent.width - 24)
        spacing: 4
        opacity: root.compact ? 0 : 1
        enabled: !root.compact
        Behavior on opacity { NumberAnimation { duration: 120 } }

        Item {
            width: parent.width
            height: 62

            Text {
                x: 8
                y: 4
                text: "设置"
                color: Theme.textPrimary
                font.pixelSize: Theme.fontDisplay
                font.weight: Font.DemiBold
            }
            Text {
                x: 8
                y: 34
                text: root.profileSelected ? "账号与头像"
                    : root.generalSelected ? "主题与本地数据" : "账号与应用"
                color: Theme.textSecondary
                font.pixelSize: Theme.fontCaption
            }
        }

        // 账号卡：头像与资料入口合一
        Rectangle {
            width: parent.width
            height: 64
            radius: 10
            color: accountArea.containsMouse ? Theme.panelAlt : Theme.panelBg
            border.width: 1
            border.color: Theme.line

            Rectangle {
                visible: root.profileSelected
                x: 0
                y: 10
                width: 2
                height: parent.height - 20
                radius: 1
                color: Theme.primary
            }

            Avatar {
                id: accountAvatar
                x: 10
                anchors.verticalCenter: parent.verticalCenter
                width: 36
                height: 36
                radius: 10
                source: root.controller ? root.controller.myIcon : ""
                name: root.controller ? root.controller.myName : ""
                textSize: 13
            }

            Column {
                x: 58
                width: parent.width - 84
                anchors.verticalCenter: parent.verticalCenter
                spacing: 3
                Text {
                    width: parent.width
                    text: root.controller ? root.controller.myName : ""
                    textFormat: Text.PlainText
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontBody
                    font.weight: Font.Medium
                    elide: Text.ElideRight
                }
                Text {
                    text: "查看资料"
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontCaption
                }
            }

            AppIcon {
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                name: "chevronRight"
                size: 14
                color: Theme.textHint
            }

            MouseArea {
                id: accountArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: if (root.controller) root.controller.openMyProfile()
            }
        }

        Item {
            width: parent.width
            height: 34
            Text {
                x: 16
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 8
                text: "应用"
                color: Theme.textHint
                font.pixelSize: Theme.fontCaption
                font.weight: Font.Medium
            }
        }

        // 外观：行内三段，点一下立即生效，不再弹带勾选的面板
        Rectangle {
            width: parent.width
            height: 48
            radius: 8
            color: "transparent"

            Text {
                x: 16
                anchors.verticalCenter: parent.verticalCenter
                text: "外观"
                color: Theme.textPrimary
                font.pixelSize: Theme.fontBody
            }

            Rectangle {
                id: appearanceSwitch
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                width: 138
                height: 34
                radius: 8
                color: Theme.panelAlt
                clip: true

                Row {
                    anchors.fill: parent

                    Repeater {
                        model: root.appearanceOptions
                        delegate: Rectangle {
                            id: segment
                            readonly property bool chosen: Theme.mode === modelData.value
                            width: 46
                            height: parent.height
                            radius: 7
                            color: chosen ? Theme.panelBg
                                 : segmentArea.containsMouse ? Theme.selected : "transparent"
                            border.width: chosen || activeFocus ? 1 : 0
                            border.color: activeFocus ? Theme.focusRing : Theme.line
                            activeFocusOnTab: true
                            Accessible.role: Accessible.Button
                            Accessible.name: "外观：" + (modelData.value === "system" ? "跟随系统" : modelData.label)
                            Accessible.onPressAction: Theme.setMode(modelData.value)
                            Keys.onReturnPressed: Theme.setMode(modelData.value)
                            Keys.onSpacePressed: Theme.setMode(modelData.value)

                            Text {
                                anchors.centerIn: parent
                                text: modelData.label
                                color: segment.chosen ? Theme.textPrimary : Theme.textSecondary
                                font.pixelSize: Theme.fontCaption
                                font.weight: segment.chosen ? Font.Medium : Font.Normal
                            }

                            MouseArea {
                                id: segmentArea
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: Theme.setMode(modelData.value)
                            }
                        }
                    }
                }
            }
        }

        SettingsRow {
            title: "通用"
            subtitle: "主题、缓存与聊天记录"
            selected: root.generalSelected
            showChevron: true
            onClicked: if (root.controller) root.controller.openGeneralSettings()
        }
    }

    // 窄窗：只留头像与通用两个入口，和聊天/联系人的头像条同宽
    Column {
        y: 20
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: 14
        opacity: root.compact ? 1 : 0
        enabled: root.compact
        Behavior on opacity { NumberAnimation { duration: 120 } }

        Item {
            width: 48
            height: 48

            Rectangle {
                anchors.fill: parent
                radius: 12
                color: root.profileSelected ? Theme.panelBg
                     : (compactProfileArea.containsMouse ? Theme.panelAlt : "transparent")
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
            width: 48
            height: 48

            Rectangle {
                anchors.fill: parent
                radius: 12
                color: root.generalSelected ? Theme.panelBg
                     : (compactGeneralArea.containsMouse ? Theme.panelAlt : "transparent")
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
    }

    Rectangle {
        visible: !root.compact
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: 16
        anchors.rightMargin: 16
        anchors.bottomMargin: 64
        height: 1
        color: Theme.lineSoft
    }

    Item {
        width: parent.width
        height: 46
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 10
        visible: !root.compact

        Rectangle {
            x: 12
            width: parent.width - 24
            height: 40
            anchors.verticalCenter: parent.verticalCenter
            radius: 8
            color: logoutArea.containsMouse ? Theme.panelAlt : "transparent"
        }
        Text {
            anchors.centerIn: parent
            text: "退出登录"
            color: Theme.danger
            font.pixelSize: Theme.fontSmall
            font.weight: Font.Medium
        }
        MouseArea {
            id: logoutArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: if (root.controller) root.controller.askLogout()
        }
    }

    Rectangle {
        anchors.right: parent.right
        width: 1
        height: parent.height
        color: Theme.line
    }
}
