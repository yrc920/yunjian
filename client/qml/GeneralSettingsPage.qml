import QtQuick

// 通用设置只管理这台设备的数据；清理操作统一走 ConfirmDialog。
Item {
    id: root
    property var controller
    property string pending: ""

    // 与个人资料共用同一套留白：左右至少各 48
    readonly property real contentWidth: Math.min(width - 96, 640)
    readonly property real inset: Math.max(48, (width - contentWidth) / 2)

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
                text: "本地数据"
                color: Theme.textHint
                font.pixelSize: Theme.fontCaption
                font.weight: Font.Medium
            }

            SettingsRow {
                title: "清理图片与文件"
                subtitle: "删除这台设备保存的图片、视频和文件；再次查看会重新下载"
                value: "清理"
                onClicked: {
                    root.pending = "media"
                    confirm.ask()
                }
            }

            SettingsRow {
                title: "清除聊天记录"
                subtitle: "删除这台设备保存的消息与草稿；服务器与其他设备不受影响"
                value: "清理"
                onClicked: {
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
                text: "信笺 " + Qt.application.version
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
