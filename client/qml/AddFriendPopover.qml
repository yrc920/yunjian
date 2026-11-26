import QtQuick
import YunJian

// 联系人页的加好友浮层：说明、搜索按钮和结果都在这里，输入框仍是上方那一条。
// 搜索、已是好友、已发申请、未找到都在浮层内表达，只有填写申请时才打开表单。
Rectangle {
    id: root

    required property ChatBridge controller
    signal applyRequested(int uid, string name)
    signal retryRequested()

    readonly property var result: controller.searchResult
    readonly property bool hasResult: controller.searchState === ChatBridge.SearchStranger
        || controller.searchState === ChatBridge.SearchAlreadyFriend
        || controller.searchState === ChatBridge.SearchAlreadyApplied
    readonly property bool sending: controller.applyState === ChatBridge.ApplySending
    readonly property bool sent: controller.applyState === ChatBridge.ApplySent
    readonly property bool failed: controller.applyState === ChatBridge.ApplyFailed

    implicitHeight: content.implicitHeight + 24
    height: implicitHeight
    radius: 12
    color: Theme.surfaceFloating
    border.width: 1
    border.color: Theme.line
    clip: true

    // 空白区域也吃掉点击，避免穿透到下面的列表遮罩
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
    }

    Column {
        id: content
        x: 14
        y: 12
        width: parent.width - 28
        spacing: 10

        Item {
            width: parent.width
            height: 28

            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: "添加朋友"
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSmall
                font.weight: Font.DemiBold
            }
            Rectangle {
                id: closeButton
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: 28
                height: 28
                radius: Theme.controlRadius
                color: closeArea.containsMouse || activeFocus ? Theme.selected : "transparent"
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: "关闭添加朋友"
                Accessible.onPressAction: root.controller.closeAddFriend()
                Keys.onSpacePressed: root.controller.closeAddFriend()
                Keys.onReturnPressed: root.controller.closeAddFriend()
                AppIcon {
                    anchors.centerIn: parent
                    name: "close"
                    size: 14
                    color: Theme.textSecondary
                }
                MouseArea {
                    id: closeArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.controller.closeAddFriend()
                }
            }
        }

        // 一行说明：跟着占位符走，告诉用户名字或 ID 都能搜
        Text {
            width: parent.width
            text: "用户名或 ID 都行"
            color: Theme.textSecondary
            font.pixelSize: Theme.fontCaption
            elide: Text.ElideRight
        }

        AppButton {
            width: parent.width
            height: 34
            text: root.controller.searchState === ChatBridge.SearchSearching ? "正在搜索…" : "搜索"
            busy: root.controller.searchState === ChatBridge.SearchSearching
            enabled: root.controller.searchText.trim().length > 0
            onClicked: root.controller.search()
        }

        Text {
            width: parent.width
            text: "回车也能找 · Esc 收起"
            color: Theme.textHint
            font.pixelSize: Theme.fontCaption
            horizontalAlignment: Text.AlignHCenter
        }

        // 结果卡片：头像、名字和 ID 固定，下面的操作区随搜索/申请状态切换
        Rectangle {
            width: parent.width
            height: resultColumn.implicitHeight + 24
            radius: 8
            color: Theme.panelAlt
            visible: root.hasResult

            Column {
                id: resultColumn
                x: 12
                y: 12
                width: parent.width - 24
                spacing: 10

                Row {
                    id: resultRow
                    width: parent.width
                    spacing: 10
                    // 陌生人还没有备注，显示名取昵称，没有昵称才用用户名
                    readonly property string displayName: root.result.nick || root.result.name || ""

                    Avatar {
                        width: 38
                        height: 38
                        radius: 12
                        source: root.result.icon || ""
                        name: resultRow.displayName
                        textSize: 15
                    }
                    Column {
                        width: parent.width - 48
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 3

                        Text {
                            width: parent.width
                            text: resultRow.displayName
                            textFormat: Text.PlainText
                            color: Theme.textPrimary
                            font.pixelSize: Theme.fontSmall
                            font.weight: Font.DemiBold
                            elide: Text.ElideRight
                        }
                        // 昵称展示在上方，这里补用户名与 ID，避免同名时认错人
                        Text {
                            text: "@" + (root.result.name || "") + "  ·  ID  " + (root.result.uid || "")
                            textFormat: Text.PlainText
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontCaption
                            elide: Text.ElideRight
                        }
                    }
                }

                Text {
                    width: parent.width
                    visible: (root.result.desc || "").length > 0
                    text: root.result.desc || ""
                    textFormat: Text.PlainText
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontCaption
                    wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                    maximumLineCount: 2
                    elide: Text.ElideRight
                }

                // 陌生人但服务端给了额外说明（曾经是好友 / 对方已申请你），
                // 让用户知道为什么这里还能点"添加"
                Text {
                    width: parent.width
                    visible: root.controller.searchState === ChatBridge.SearchStranger
                        && root.controller.searchMessage.length > 0
                    text: root.controller.searchMessage
                    textFormat: Text.PlainText
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontCaption
                    wrapMode: Text.Wrap
                }

                // 陌生人：打开申请表单
                AppButton {
                    width: parent.width
                    height: 32
                    visible: root.controller.searchState === ChatBridge.SearchStranger
                        && !root.sending && !root.sent && !root.failed
                    text: "添加到通讯录"
                    onClicked: root.applyRequested(root.result.uid || 0, resultRow.displayName)
                }

                // 已经是好友：直接进入会话
                AppButton {
                    width: parent.width
                    height: 32
                    visible: root.controller.searchState === ChatBridge.SearchAlreadyFriend
                        && !root.sending && !root.sent
                    text: "发消息"
                    onClicked: root.controller.openChat(root.result.uid || 0)
                }

                // 发送中 / 已发送：按钮位置换成状态，不再重复点击
                Row {
                    width: parent.width
                    height: 32
                    spacing: 6
                    visible: root.sending || root.sent

                    AppIcon {
                        name: root.sent ? "check" : "mail"
                        size: 14
                        color: root.sent ? Theme.primary : Theme.textSecondary
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - 20
                        text: root.sent
                            ? (root.controller.applyMessage || "申请已发送，等待对方通过")
                            : "正在发送申请…"
                        textFormat: Text.PlainText
                        color: root.sent ? Theme.primary : Theme.textSecondary
                        font.pixelSize: Theme.fontCaption
                        elide: Text.ElideRight
                    }
                }

                Column {
                    width: parent.width
                    spacing: 6
                    visible: root.failed

                    Text {
                        width: parent.width
                        text: root.controller.applyMessage || "申请没有发出去，稍后再试。"
                        textFormat: Text.PlainText
                        color: Theme.danger
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                    }
                    AppButton {
                        width: parent.width
                        height: 30
                        text: "重新发送"
                        onClicked: root.controller.retryApply()
                    }
                }

                Text {
                    width: parent.width
                    visible: root.controller.searchState === ChatBridge.SearchAlreadyApplied
                    text: "已经发过申请，等待对方通过"
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontCaption
                }
            }
        }

        Text {
            width: parent.width
            visible: root.controller.searchState === ChatBridge.SearchSelf
            text: root.controller.searchMessage || "这是你自己的账号"
            color: Theme.textSecondary
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.WrapAtWordBoundaryOrAnywhere
        }

        Column {
            width: parent.width
            visible: root.controller.searchState === ChatBridge.SearchNotFound
            spacing: 6

            Text {
                width: parent.width
                text: root.controller.searchMessage || "没有找到这位用户"
                color: Theme.textSecondary
                font.pixelSize: Theme.fontCaption
            }
            Text {
                width: parent.width
                text: "名字或 ID，再核对一下"
                color: Theme.textHint
                font.pixelSize: Theme.fontCaption
            }
            AppLink {
                text: "重新搜索"
                onClicked: root.retryRequested()
            }
        }
    }
}
