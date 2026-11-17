import QtQuick

// 通用设置：目前只有本地存储相关的清理动作。
// 这里所有操作都只影响本机：消息仍然在服务端与其他设备上，清完重新登录还能同步回来。
Item {
    id: root
    property var controller
    // 待确认的动作：空串表示没有弹层，"history" 清聊天记录，"media" 清媒体缓存
    property string pending: ""

    Rectangle {
        anchors.fill: parent
        color: Theme.chatBg
    }

    Item {
        id: header
        width: parent.width
        height: 74
        Text {
            x: 30
            anchors.verticalCenter: parent.verticalCenter
            text: "通用"
            color: Theme.textPrimary
            font.pixelSize: 16
            font.weight: Font.DemiBold
        }
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: Theme.line
        }
    }

    Flickable {
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        width: parent.width
        contentWidth: width
        contentHeight: details.height + 84
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        enabled: root.pending.length === 0

        Column {
            id: details
            x: Math.max(30, (parent.width - 560) / 2)
            y: 34
            width: Math.min(parent.width - 60, 560)
            spacing: 14

            Text {
                text: "本地存储"
                color: Theme.textSecondary
                font.pixelSize: 12
            }

            // 清空消息但是保留好友、群与会话；游标不重置，下次登录不会又拉回来
            Rectangle {
                width: parent.width
                height: 74
                radius: 10
                color: Theme.panelBg
                border.width: 1
                border.color: Theme.line

                Column {
                    x: 18
                    width: parent.width - 140
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 5
                    Text {
                        text: "清除本地聊天记录"
                        color: Theme.textPrimary
                        font.pixelSize: 14
                    }
                    Text {
                        width: parent.width
                        text: "删除本机保存的全部消息与草稿，好友、群聊与服务器上的记录都保留"
                        color: Theme.textSecondary
                        font.pixelSize: 12
                        wrapMode: Text.WordWrap
                    }
                }
                CodeButton {
                    anchors.right: parent.right
                    anchors.rightMargin: 18
                    anchors.verticalCenter: parent.verticalCenter
                    width: 84
                    height: 36
                    text: "清除"
                    onClicked: root.pending = "history"
                }
            }

            // 只删缓存目录，消息里的资源名还在，重新打开会话会按需重新下载
            Rectangle {
                width: parent.width
                height: 74
                radius: 10
                color: Theme.panelBg
                border.width: 1
                border.color: Theme.line

                Column {
                    x: 18
                    width: parent.width - 140
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 5
                    Text {
                        text: "清除已下载的图片、视频与文件"
                        color: Theme.textPrimary
                        font.pixelSize: 14
                    }
                    Text {
                        width: parent.width
                        text: "只删除本机缓存的媒体文件，聊天记录保留；再次查看时会重新下载"
                        color: Theme.textSecondary
                        font.pixelSize: 12
                        wrapMode: Text.WordWrap
                    }
                }
                CodeButton {
                    anchors.right: parent.right
                    anchors.rightMargin: 18
                    anchors.verticalCenter: parent.verticalCenter
                    width: 84
                    height: 36
                    text: "清除"
                    onClicked: root.pending = "media"
                }
            }

            Text {
                width: parent.width
                text: "以上操作都只影响这台设备，不会删除服务器或其他设备上的记录。"
                color: Theme.textSecondary
                font.pixelSize: 11
                wrapMode: Text.WordWrap
            }
        }
    }

    // 二次确认：删除类操作都走同一层，避免误点
    Rectangle {
        anchors.fill: parent
        visible: root.pending.length > 0
        focus: visible
        color: Theme.overlay
        z: 20
        Keys.onEscapePressed: root.pending = ""
        onVisibleChanged: if (visible) forceActiveFocus()

        MouseArea {
            anchors.fill: parent
            onClicked: root.pending = ""
        }

        Rectangle {
            anchors.centerIn: parent
            width: Math.min(420, parent.width - 64)
            implicitHeight: confirmContent.implicitHeight + 48
            radius: 12
            color: Theme.panelBg
            border.width: 1
            border.color: Theme.line

            Column {
                id: confirmContent
                x: 24
                y: 24
                width: parent.width - 48
                spacing: 14

                Text {
                    text: root.pending === "history" ? "清除本地聊天记录" : "清除本地媒体文件"
                    color: Theme.textPrimary
                    font.pixelSize: 18
                    font.bold: true
                }
                Text {
                    width: parent.width
                    text: root.pending === "history"
                        ? "将删除本机保存的全部消息与草稿。好友、群聊和服务器上的记录不受影响，"
                          + "重新登录也不会把已清掉的历史重新拉回来。"
                        : "将删除本机缓存的图片、视频与文件。聊天记录保留，再次查看时会重新下载。"
                    color: Theme.textSecondary
                    font.pixelSize: 12
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
                        text: "确认清除"
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
