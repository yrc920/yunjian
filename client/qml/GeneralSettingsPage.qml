import QtQuick

// 通用设置集中管理本机数据，清理操作均在确认后执行。
Item {
    id: root
    property var controller
    property string pending: ""

    Rectangle {
        anchors.fill: parent
        color: Theme.chatBg
    }

    PageHeader {
        id: header
        title: "通用"
        trailingIcon: "settings"
    }

    Flickable {
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        width: parent.width
        contentWidth: width
        contentHeight: details.height + 48
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        enabled: root.pending.length === 0

        Column {
            id: details
            x: Math.max(24, (parent.width - 600) / 2)
            y: 28
            width: Math.min(parent.width - 48, 600)
            spacing: 4

            Text {
                x: 12
                bottomPadding: 8
                text: "本地数据"
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSmall
                font.weight: Font.Medium
            }

            SettingsRow {
                iconName: "chat"
                title: "清除本地聊天记录"
                subtitle: "删除这台设备保存的消息与草稿"
                showChevron: true
                onClicked: root.pending = "history"
            }

            SettingsRow {
                iconName: "folder"
                title: "清除媒体缓存"
                subtitle: "已下载的图片、视频和文件会在下次查看时重新下载"
                showChevron: true
                onClicked: root.pending = "media"
            }

            Item { width: 1; height: 12 }

            Text {
                width: parent.width - 24
                x: 12
                text: "只影响这台设备，不会删除服务器或其他设备上的记录。"
                color: Theme.textSecondary
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.WordWrap
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        visible: root.pending.length > 0
        focus: visible
        color: Theme.overlay
        z: 20
        Keys.onEscapePressed: root.pending = ""
        onVisibleChanged: if (visible) forceActiveFocus()

        MouseArea { anchors.fill: parent; onClicked: root.pending = "" }

        Rectangle {
            anchors.centerIn: parent
            width: Math.min(420, parent.width - 48)
            height: confirmContent.implicitHeight + 48
            radius: Theme.dialogRadius
            color: Theme.surfaceFloating
            border.width: 1
            border.color: Theme.line

            MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons }
            Column {
                id: confirmContent
                x: 24
                y: 24
                width: parent.width - 48
                spacing: 14

                Text {
                    text: root.pending === "history" ? "清除本地聊天记录？" : "清除媒体缓存？"
                    color: Theme.textPrimary
                    font.pixelSize: 18
                    font.weight: Font.DemiBold
                }
                Text {
                    width: parent.width
                    text: root.pending === "history"
                        ? "消息和草稿会从这台设备删除。下次登录不会恢复已清除的记录；其他设备不受影响。"
                        : "下载的图片、视频和文件会被删除；聊天记录保留，再次查看时会重新下载。"
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontBody
                    wrapMode: Text.WordWrap
                }
                Row {
                    width: parent.width
                    spacing: 12

                    CodeButton {
                        width: (parent.width - 12) / 2
                        height: 42
                        text: "取消"
                        onClicked: root.pending = ""
                    }
                    AppButton {
                        width: (parent.width - 12) / 2
                        height: 42
                        variant: "danger"
                        text: root.pending === "history" ? "清除聊天记录" : "清除媒体缓存"
                        onClicked: {
                            const action = root.pending
                            root.pending = ""
                            if (!root.controller)
                                return
                            if (action === "history")
                                root.controller.clearAllHistory()
                            else
                                root.controller.clearMediaCache()
                        }
                    }
                }
            }
        }
    }
}
