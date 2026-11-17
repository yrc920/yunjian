import QtQuick

// 联系人资料按字段排列，所有可见信息来自当前好友。
// profile 是 ChatBridge 生成的快照，字段缺失时用占位文案，不在这里补业务数据。
//
// 字段统一「左侧标签 + 右侧内容」：标签固定在 x=0，内容从 x=82 开始并占满剩余宽度，
// 长内容换行时整行高度跟着长高。签名与标签也走同一套排版，不再单独占一块。
Item {
    id: root

    property var profile: ({})
    signal chatRequested()
    signal editRelationRequested()
    // 删除确认由外层弹层统一处理，页面只负责发起
    signal deleteRequested()
    // 从聊天里点进来时，看完资料要能回到原来的会话
    signal backRequested()

    readonly property int sex: profile.sex === undefined ? 0 : profile.sex
    // 备注 > 昵称 > 用户名，与列表、聊天标题保持同一条规则
    readonly property string display: profile.displayName || profile.name || ""
    readonly property var tags: profile.tags || []
    readonly property bool hasDesc: (profile.desc || "").length > 0

    Rectangle {
        anchors.fill: parent
        color: Theme.chatBg
    }

    // 标题栏
    Item {
        id: header
        width: parent.width
        height: 74

        Rectangle {
            id: backButton
            x: 24
            anchors.verticalCenter: parent.verticalCenter
            width: 30
            height: 30
            radius: 15
            color: backArea.containsMouse || activeFocus ? Theme.panelAlt : "transparent"
            activeFocusOnTab: true
            Accessible.role: Accessible.Button
            Accessible.name: "返回"
            Accessible.onPressAction: root.backRequested()
            Keys.onSpacePressed: root.backRequested()
            Keys.onReturnPressed: root.backRequested()
            AppIcon {
                anchors.centerIn: parent
                name: "back"
                size: 17
                color: Theme.textSecondary
            }
            MouseArea {
                id: backArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.backRequested()
            }
        }

        Text {
            x: 64
            anchors.verticalCenter: parent.verticalCenter
            text: "联系人资料"
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

    // 资料区限宽 480 并居中，窗口变窄时先减两侧留白
    Flickable {
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        width: parent.width
        contentWidth: width
        contentHeight: details.height + 84
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: details
            x: Math.max(30, (parent.width - 480) / 2)
            y: 42
            width: Math.min(parent.width - 60, 480)
            spacing: 0

            Row {
                width: parent.width
                spacing: 20

                Avatar {
                    width: 76
                    height: 76
                    source: root.profile.icon || ""
                    name: root.display
                    textSize: 26
                }

                Column {
                    width: parent.width - 96
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 9

                    Text {
                        width: parent.width
                        text: root.display
                        textFormat: Text.PlainText
                        color: Theme.textPrimary
                        font.pixelSize: 25
                        font.weight: Font.DemiBold
                        wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                    }
                    Text {
                        text: root.profile.uid ? "ID  " + root.profile.uid : ""
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSmall
                        font.letterSpacing: 1
                    }
                }
            }

            Spacer { gap: 32 }
            Rectangle {
                width: parent.width
                height: 1
                color: Theme.line
                Rectangle {
                    width: 28
                    height: 2
                    color: Theme.accent
                }
            }
            Spacer { gap: 12 }

            // 固定资料字段；值过长时换行，行高跟着内容增长
            Repeater {
                model: [
                    { label: "昵称", value: root.profile.nick || "未设置" },
                    { label: "性别", value: root.sex === 1 ? "男" : (root.sex === 2 ? "女" : "未设置") }
                ]

                delegate: Item {
                    width: details.width
                    height: Math.max(48, valueText.implicitHeight + 26)

                    Text {
                        x: 0
                        y: 13
                        text: modelData.label
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontBody
                    }
                    Text {
                        id: valueText
                        x: 82
                        y: 13
                        width: parent.width - x
                        text: modelData.value
                        textFormat: Text.PlainText
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontBody
                        wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                    }
                }
            }

            // 备注只对自己生效，与其他字段同一行排版
            Item {
                width: details.width
                height: Math.max(48, remarkValue.implicitHeight + 26)
                Text {
                    x: 0
                    y: 13
                    text: "备注"
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontBody
                }
                Text {
                    id: remarkValue
                    x: 82
                    y: 13
                    width: parent.width - x
                    text: root.profile.remark || "未设置"
                    textFormat: Text.PlainText
                    color: root.profile.remark ? Theme.textPrimary : Theme.textHint
                    font.pixelSize: Theme.fontBody
                    wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                }
            }

            // 个性签名：好友没写时给一句轻默认文案，只做展示不写库；长文本整段换行
            Item {
                width: details.width
                height: Math.max(48, descValue.implicitHeight + 26)
                Text {
                    x: 0
                    y: 13
                    text: "个性签名"
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontBody
                }
                Text {
                    id: descValue
                    x: 82
                    y: 13
                    width: parent.width - x
                    text: root.hasDesc ? root.profile.desc : "这一页还空着。"
                    textFormat: Text.PlainText
                    color: root.hasDesc ? Theme.textPrimary : Theme.textHint
                    font.pixelSize: Theme.fontBody
                    lineHeight: 1.4
                    wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                }
            }

            // 分组标签：与备注一起在「编辑资料」里维护，只对自己可见
            Item {
                width: details.width
                height: Math.max(48, tagFlow.height + 24)
                Text {
                    x: 0
                    y: 13
                    text: "标签"
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontBody
                }
                Flow {
                    id: tagFlow
                    x: 82
                    y: 12
                    width: parent.width - x
                    spacing: 7
                    Repeater {
                        model: root.tags
                        delegate: Rectangle {
                            width: tagLabel.implicitWidth + 22
                            height: 26
                            radius: 5
                            color: Theme.selected
                            Text {
                                id: tagLabel
                                anchors.centerIn: parent
                                text: modelData
                                color: Theme.textPrimary
                                font.pixelSize: Theme.fontSmall
                            }
                        }
                    }
                }
                Text {
                    x: 82
                    y: 13
                    visible: root.tags.length === 0
                    text: "未设置"
                    color: Theme.textHint
                    font.pixelSize: Theme.fontBody
                }
            }

            Spacer { gap: 12 }
            Rectangle { width: parent.width; height: 1; color: Theme.line }
            Spacer { gap: 28 }

            // 资料页只留两个真实动作：发消息与编辑资料；语音、视频等能力就绪后再加回来
            Row {
                width: parent.width
                spacing: 12

                AppButton {
                    width: Math.min(168, parent.width - 132)
                    height: 46
                    text: "发消息"
                    iconName: "chat"
                    onClicked: root.chatRequested()
                }
                CodeButton {
                    width: 120
                    height: 46
                    text: "编辑资料"
                    // 关系已经断了：备注与标签编辑没有意义，只保留发消息（进去补验证）
                    visible: root.profile.friend !== false
                    onClicked: root.editRelationRequested()
                }
            }

            // 删除好友：做成次要按钮而不是一行小字，误点风险由确认弹层兜住。
            // 不是好友时这里换成"清除聊天记录"由会话菜单处理，页面不再提供删除入口
            Rectangle {
                width: parent.width
                height: 42
                visible: root.profile.friend !== false
                radius: 8
                color: deleteArea.containsMouse ? Theme.panelAlt : "transparent"
                border.width: 1
                border.color: Theme.line

                AppIcon {
                    x: 14
                    anchors.verticalCenter: parent.verticalCenter
                    name: "minus"
                    size: 16
                    color: Theme.danger
                }
                Text {
                    x: 38
                    anchors.verticalCenter: parent.verticalCenter
                    text: "删除好友"
                    color: Theme.danger
                    font.pixelSize: Theme.fontSmall
                }
                MouseArea {
                    id: deleteArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.deleteRequested()
                }
            }
        }
    }
}
