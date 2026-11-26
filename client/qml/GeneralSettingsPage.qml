import QtQuick

// 通用设置只管理这台设备的数据；清理操作统一走 ConfirmDialog。
Item {
    id: root
    property var controller
    property string pending: ""

    // 与个人资料共用同一套留白：左右至少各 48
    readonly property real contentWidth: Math.min(width - 96, 640)
    readonly property real inset: Math.max(48, (width - contentWidth) / 2)

    // 外观三选一：预览卡读 Theme 的两套色板，和真实界面同源
    readonly property var appearanceOptions: [
        { value: "light", label: "浅色" },
        { value: "dark", label: "深色" },
        { value: "system", label: "跟随系统" }
    ]

    Rectangle {
        anchors.fill: parent
        color: Theme.sheet
    }

    PageHeader {
        id: header
        title: "通用"
        inset: 24
    }

    Flickable {
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        width: parent.width
        contentWidth: width
        contentHeight: content.height + 64
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: content
            x: root.inset
            y: 32
            width: root.contentWidth
            spacing: 0

            Text {
                x: 16
                bottomPadding: 8
                text: "外观"
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSmall
                font.weight: Font.Medium
            }

            Row {
                id: themeRow
                width: parent.width
                spacing: 12

                Repeater {
                    model: root.appearanceOptions
                    delegate: Rectangle {
                        id: themeCard
                        readonly property bool chosen: Theme.mode === modelData.value
                        width: (themeRow.width - 24) / 3
                        height: 8 + preview.height + 12 + labelText.implicitHeight + 8
                        radius: Theme.panelRadius
                        color: Theme.panelBg
                        border.width: 1
                        border.color: cardArea.containsMouse ? Theme.fieldBorder : Theme.line
                        activeFocusOnTab: true
                        Accessible.role: Accessible.Button
                        Accessible.name: "外观：" + modelData.label
                        Accessible.onPressAction: Theme.setMode(modelData.value)
                        Keys.onReturnPressed: Theme.setMode(modelData.value)
                        Keys.onSpacePressed: Theme.setMode(modelData.value)
                        Behavior on border.color { ColorAnimation { duration: 120 } }

                        ThemePreview {
                            id: preview
                            x: 8
                            y: 8
                            width: parent.width - 16
                            height: width * (96 / 176)
                            mode: modelData.value
                        }

                        Text {
                            id: labelText
                            anchors.horizontalCenter: parent.horizontalCenter
                            y: preview.y + preview.height + 12
                            text: modelData.label
                            color: themeCard.chosen ? Theme.textPrimary : Theme.textSecondary
                            font.pixelSize: Theme.fontSmall
                            font.weight: themeCard.chosen ? Font.Medium : Font.Normal
                        }

                        // 选中环：2px 墨环压在卡边，不改卡片几何
                        Rectangle {
                            anchors.fill: parent
                            radius: parent.radius
                            color: "transparent"
                            border.width: 2
                            border.color: themeCard.activeFocus ? Theme.focusRing : Theme.primary
                            visible: themeCard.chosen || themeCard.activeFocus
                        }

                        // 印点：和列表选中线、蜡封同一种“标记”语言
                        Rectangle {
                            visible: themeCard.chosen
                            x: preview.x + preview.width - 26
                            y: preview.y + 8
                            width: 18
                            height: 18
                            radius: 9
                            color: Theme.primary
                            AppIcon {
                                anchors.centerIn: parent
                                name: "check"
                                size: 12
                                color: Theme.primaryText
                            }
                        }

                        MouseArea {
                            id: cardArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                themeCard.forceActiveFocus(Qt.MouseFocusReason)
                                Theme.setMode(modelData.value)
                            }
                        }
                    }
                }
            }

            Item { width: 1; height: 32 }

            Text {
                x: 16
                bottomPadding: 8
                text: "本地数据"
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSmall
                font.weight: Font.Medium
            }

            SettingsRow {
                title: "清理图片与文件"
                subtitle: "删除这台设备保存的图片、视频和文件；再次查看会重新下载"
                actionMode: true
                actionText: "清理"
                onActionTriggered: {
                    root.pending = "media"
                    confirm.ask()
                }
            }

            SettingsRow {
                title: "清除聊天记录"
                subtitle: "删除这台设备保存的消息与草稿；服务器与其他设备不受影响"
                actionMode: true
                actionText: "清理"
                onActionTriggered: {
                    root.pending = "history"
                    confirm.ask()
                }
            }

            Item { width: 1; height: 16 }

            Text {
                x: 16
                width: parent.width - 32
                text: "只影响这台设备。"
                color: Theme.textHint
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.WordWrap
            }

            Item { width: 1; height: 40 }

            Rectangle {
                width: parent.width
                height: 1
                color: Theme.lineSoft
            }

            Item { width: 1; height: 14 }

            Text {
                x: 16
                text: "云笺 " + Qt.application.version
                color: Theme.textHint
                font.pixelSize: Theme.fontCaption
            }
        }
    }

    ConfirmDialog {
        id: confirm
        title: root.pending === "history" ? "清除聊天记录" : "清理图片与文件"
        body: root.pending === "history"
            ? "这台设备上的消息和草稿会被删除，下次登录不会恢复；其他设备不受影响。"
            : "这台设备上下载的图片、视频和文件会被删除；聊天记录保留，再次查看时会重新下载。"
        confirmText: root.pending === "history" ? "清除聊天记录" : "清理文件"
        danger: true
        onConfirmed: {
            if (root.controller) {
                if (root.pending === "history")
                    root.controller.clearAllHistory()
                else
                    root.controller.clearMediaCache()
            }
            root.pending = ""
            close()
        }
        onCancelled: root.pending = ""
    }
}
