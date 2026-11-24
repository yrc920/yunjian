import QtQuick

// 好友申请只在服务端确认后变为已添加。
// 列表数据来自 ApplyListModel，点击「添加」只发出请求，不改本地状态。
Item {
    id: root

    property var applies: null
    signal authRequested(int uid, string name, string desc)
    signal chatRequested(int uid)

    Rectangle {
        anchors.fill: parent
        color: Theme.sheet
    }

    PageHeader {
        id: header
        title: "新的朋友"
        subtitle: root.applies && root.applies.pendingCount > 0
            ? root.applies.pendingCount + " 条申请待处理" : "好友申请"
    }

    // 一条申请占一行：头像、申请人、留言，右侧按钮在已处理时换成对勾
    ListView {
        id: requests
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        anchors.topMargin: 6
        anchors.bottomMargin: 16
        x: 10
        width: parent.width - 20
        model: root.applies
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        delegate: Rectangle {
            width: requests.width
            height: 80
            color: hover.hovered ? Theme.panelAlt : "transparent"
            HoverHandler { id: hover }

            Avatar {
                id: avatar
                x: 10
                anchors.verticalCenter: parent.verticalCenter
                width: 44
                height: 44
                source: model.icon
                name: model.displayName
                textSize: 16
            }
            Column {
                anchors.left: avatar.right
                anchors.leftMargin: 14
                anchors.right: action.left
                anchors.rightMargin: 18
                anchors.verticalCenter: parent.verticalCenter
                spacing: 6

                Text {
                    width: parent.width
                    text: model.displayName
                    textFormat: Text.PlainText
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontBody
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
                Text {
                    width: parent.width
                    // 收到的申请显示对方留言；发出的申请显示自己的留言和等待状态
                    text: model.direction === 1
                        ? (model.desc.length > 0 ? "我：" + model.desc : "等待对方通过")
                        : (model.desc.length > 0 ? model.desc : "请求添加你为好友")
                    textFormat: Text.PlainText
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSmall
                    wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                    maximumLineCount: 2
                    elide: Text.ElideRight
                }
            }
            Item {
                id: action
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                width: 78
                height: 34

                AppButton {
                    anchors.fill: parent
                    visible: model.direction === 0 && model.status === 0
                    text: "添加"
                    onClicked: root.authRequested(model.uid, model.displayName, model.desc)
                }
                Text {
                    anchors.centerIn: parent
                    visible: model.direction === 1 && model.status === 0
                    text: "等待验证"
                    color: Theme.textHint
                    font.pixelSize: Theme.fontSmall
                }
                Row {
                    anchors.centerIn: parent
                    visible: model.status !== 0
                    spacing: 5
                    AppIcon {
                        name: "check"
                        size: 14
                        color: Theme.textHint
                    }
                    Text {
                        text: model.direction === 1 ? "已通过" : "已添加"
                        color: Theme.textHint
                        font.pixelSize: Theme.fontSmall
                    }
                }
            }
            Rectangle {
                anchors.bottom: parent.bottom
                x: 10
                width: parent.width - 20
                height: 1
                color: Theme.line
            }

            // 已通过的行整行可点，直接进入会话；待处理的行仍由右侧按钮负责
            MouseArea {
                anchors.fill: parent
                enabled: model.status !== 0
                cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: root.chatRequested(model.uid)
            }
        }
    }

    // 空态：没有申请时给出角色插画，而不是留一块空白
    Column {
        anchors.centerIn: parent
        width: parent.width - 60
        spacing: 14
        visible: !root.applies || root.applies.count === 0
        PaperCompanion {
            anchors.horizontalCenter: parent.horizontalCenter
            width: 150
            height: 108
            compact: true
        }
        Text {
            width: parent.width
            text: "还没有好友申请"
            horizontalAlignment: Text.AlignHCenter
            color: Theme.textSecondary
            font.pixelSize: Theme.fontBody
        }
        Text {
            width: parent.width
            text: "有人想加你时，会先到这里。"
            horizontalAlignment: Text.AlignHCenter
            color: Theme.textHint
            font.pixelSize: Theme.fontSmall
        }
    }
}
