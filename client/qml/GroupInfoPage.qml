import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import XiguChat

// 群资料页：群名、公告、我的群昵称、免打扰、成员与退群/解散。
// 业务动作全部交给 ChatBridge，页面只负责表单与确认弹层。
Item {
    id: root
    property var controller: null

    readonly property var info: controller ? controller.groupInfo : ({})
    readonly property var members: controller ? controller.groupMembers : []
    readonly property var candidates: controller ? controller.groupCandidates : []
    readonly property bool canManage: controller ? controller.canManageGroup : false
    readonly property bool isOwner: controller ? controller.isGroupOwner : false
    readonly property int memberCount: info.memberCount || members.length
    readonly property string notice: String(info.notice || "")
    readonly property bool muted: info.muted === true
    //成员卡与移出确认：普通成员也能看到成员资料，不再点了没反应
    property var cardMember: ({})
    property int pendingRemoveUid: 0
    property string pendingRemoveName: ""

    property string iconError: "" //群头像上传失败的提示，成功或重新选图时清掉

    signal backRequested()

    function memberExists(uid) {
        for (let i = 0; i < members.length; ++i) {
            if (members[i].uid === uid)
                return true
        }
        return false
    }

    //邀请候选：已经在群里的好友不再出现
    function inviteCandidates() {
        const list = []
        for (let i = 0; i < candidates.length; ++i) {
            if (!memberExists(candidates[i].uid))
                list.push(candidates[i])
        }
        return list
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.chatBg
    }

    // 顶部：返回会话 + 页面标题
    Item {
        id: header
        width: parent.width
        height: 78

        Rectangle {
            id: backButton
            x: 24
            anchors.verticalCenter: parent.verticalCenter
            width: 30
            height: 30
            radius: 15
            color: backArea.containsMouse ? Theme.panelAlt : "transparent"
            activeFocusOnTab: true
            Accessible.role: Accessible.Button
            Accessible.name: "返回会话"
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
            text: "群聊资料"
            color: Theme.textPrimary
            font.pixelSize: 20
            font.bold: true
        }
        Rectangle {
            x: 28
            anchors.bottom: parent.bottom
            width: parent.width - 56
            height: 1
            color: Theme.line
        }
    }

    Flickable {
        id: body
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        anchors.margins: 24
        contentWidth: width
        contentHeight: content.height
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        Column {
            id: content
            width: body.width

            // 群名：群主与管理员可以直接改
            Item {
                width: parent.width
                height: 72

                // 群头像与列表同一套规则：名字前两字 + 派生底色
                Avatar {
                    id: groupAvatar
                    x: 0
                    width: 56
                    height: 56
                    radius: 14
                    anchors.verticalCenter: parent.verticalCenter
                    group: true
                    showGroupBadge: !root.canManage
                    source: String(root.info.iconSource || "")
                    name: String(root.info.name || "群聊")
                    textSize: 20
                }

                // 常驻的相机角标：不悬停也知道点头像能换图（管理员才可见）
                Rectangle {
                    visible: root.canManage
                    anchors.right: groupAvatar.right
                    anchors.bottom: groupAvatar.bottom
                    anchors.rightMargin: -3
                    anchors.bottomMargin: -3
                    width: 20
                    height: 20
                    radius: 10
                    color: Theme.panelBg
                    border.width: 1
                    border.color: Theme.line

                    AppIcon {
                        anchors.centerIn: parent
                        name: "image"
                        size: 11
                        color: Theme.primary
                    }
                }

                // 悬停再压一层遮罩，强化"可换图"
                Rectangle {
                    anchors.fill: groupAvatar
                    radius: groupAvatar.radius
                    color: "#59000000"
                    visible: root.canManage && avatarArea.containsMouse

                    AppIcon {
                        anchors.centerIn: parent
                        name: "image"
                        size: 18
                        color: "#FFFFFF"
                    }
                }

                // 头像就是入口：管理员点一下直接换图
                MouseArea {
                    id: avatarArea
                    anchors.fill: groupAvatar
                    enabled: root.canManage
                    hoverEnabled: true
                    cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onClicked: iconPicker.open()

                    ToolTip.visible: containsMouse && enabled
                    ToolTip.text: "更换群头像"
                    ToolTip.delay: 300
                }

                Text {
                    id: groupName
                    anchors.left: groupAvatar.right
                    anchors.leftMargin: 14
                    anchors.top: parent.top
                    anchors.topMargin: 10
                    width: parent.width - 164
                    text: String(root.info.name || "群聊")
                    textFormat: Text.PlainText
                    color: Theme.textPrimary
                    font.pixelSize: 22
                    font.bold: true
                    elide: Text.ElideRight
                }
                // 铅笔常驻：跟文字的 contentWidth 走，短名字也紧贴名字后面；
                // 悬停时加深颜色，点击进改名弹层
                AppIcon {
                    id: nameEdit
                    anchors.left: groupName.left
                    anchors.leftMargin: Math.min(groupName.contentWidth, groupName.width - 24) + 8
                    anchors.verticalCenter: groupName.verticalCenter
                    name: "edit"
                    size: 15
                    color: nameHover.containsMouse ? Theme.primary : Theme.textHint
                    visible: root.canManage
                    Behavior on color { ColorAnimation { duration: 120 } }
                }

                MouseArea {
                    id: nameHover
                    anchors.fill: groupName
                    anchors.rightMargin: -30
                    anchors.topMargin: -4
                    anchors.bottomMargin: -4
                    enabled: root.canManage
                    hoverEnabled: true
                    cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onClicked: nameEditor.open()

                    ToolTip.visible: containsMouse && enabled
                    ToolTip.text: "修改群名"
                    ToolTip.delay: 300
                }

                Text {
                    id: memberLabel
                    anchors.left: groupName.left
                    anchors.top: groupName.bottom
                    anchors.topMargin: 4
                    text: root.memberCount + " 位成员"
                    color: Theme.textSecondary
                    font.pixelSize: 11
                }
                AppButton {
                    visible: root.canManage
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: 92
                    height: 32
                    text: "编辑资料"
                    onClicked: editProfile.begin()
                }
            }

            // 群公告：成员都能看，管理能改
            Rectangle {
                width: parent.width
                height: noticeColumn.implicitHeight + 28
                radius: 10
                color: Theme.panelBg
                border.width: 1
                border.color: Theme.line

                Column {
                    id: noticeColumn
                    x: 16
                    y: 14
                    width: parent.width - 32
                    spacing: 8
                    Item {
                        width: parent.width
                        height: noticeTitle.implicitHeight
                        Text {
                            id: noticeTitle
                            text: "群公告"
                            color: Theme.textPrimary
                            font.pixelSize: 13
                            font.bold: true
                        }
                        Text {
                            visible: root.canManage
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            text: "编辑"
                            color: Theme.primary
                            font.pixelSize: 11
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: noticeEditor.open()
                            }
                        }
                    }
                    Text {
                        width: parent.width
                        text: root.notice.length > 0 ? root.notice : "还没有群公告"
                        textFormat: Text.PlainText
                        color: root.notice.length > 0 ? Theme.textSecondary : Theme.textHint
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }
                }
            }

            Item { width: parent.width; height: 14 }

            // 我在本群的昵称
            Rectangle {
                width: parent.width
                height: 52
                radius: 10
                color: Theme.panelBg
                border.width: 1
                border.color: Theme.line
                Text {
                    x: 16
                    anchors.verticalCenter: parent.verticalCenter
                    text: "我在本群的昵称"
                    color: Theme.textPrimary
                    font.pixelSize: 13
                }
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    text: String(root.info.myNick || "") || "未设置"
                    color: Theme.textSecondary
                    font.pixelSize: 12
                    elide: Text.ElideRight
                    width: Math.min(implicitWidth, parent.width - 180)
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: nickEditor.open()
                }
            }

            Item { width: parent.width; height: 8 }

            // 免打扰：只影响本地提示，不改变服务端推送
            Rectangle {
                width: parent.width
                height: 52
                radius: 10
                color: Theme.panelBg
                border.width: 1
                border.color: Theme.line
                Text {
                    x: 16
                    anchors.verticalCenter: parent.verticalCenter
                    text: "消息免打扰"
                    color: Theme.textPrimary
                    font.pixelSize: 13
                }
                Rectangle {
                    id: muteSwitch
                    anchors.right: parent.right
                    anchors.rightMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    width: 42
                    height: 24
                    radius: 12
                    color: root.muted ? Theme.primary : Theme.fieldBg
                    border.width: 1
                    border.color: root.muted ? Theme.primary : Theme.fieldBorder
                    Rectangle {
                        width: 18
                        height: 18
                        radius: 9
                        y: 2
                        x: root.muted ? parent.width - width - 2 : 2
                        color: root.muted ? Theme.primaryText : Theme.textSecondary
                        Behavior on x { NumberAnimation { duration: 120 } }
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: controller.toggleGroupMute()
                    }
                }
            }

            Item { width: parent.width; height: 20 }

            // 成员区：头像 + 名字，群主/管理员可点开管理菜单
            Item {
                width: parent.width
                height: memberTitle.implicitHeight
                Text {
                    id: memberTitle
                    text: "群成员 · " + root.memberCount
                    color: Theme.textPrimary
                    font.pixelSize: 13
                    font.bold: true
                }
                Text {
                    visible: root.memberCount > 0
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    text: "邀请好友"
                    color: Theme.primary
                    font.pixelSize: 12
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: invitePopup.open()
                    }
                }
            }

            Item { width: parent.width; height: 10 }

            Flow {
                width: parent.width
                spacing: 4

                Repeater {
                    model: root.members
                    delegate: Item {
                        width: (content.width - 12) / 4
                        height: 82
                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: 2
                            radius: 8
                            color: memberArea.containsMouse ? Theme.panelAlt : "transparent"
                        }
                        Avatar {
                            id: memberAvatar
                            anchors.horizontalCenter: parent.horizontalCenter
                            y: 8
                            width: 40
                            height: 40
                            radius: 11
                            source: modelData.icon
                            name: modelData.name
                        }
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.top: memberAvatar.bottom
                            anchors.topMargin: 6
                            width: parent.width - 8
                            text: modelData.name + (modelData.role === 2 ? "（群主）"
                                : modelData.role === 1 ? "（管理员）" : "")
                            textFormat: Text.PlainText
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontCaption
                            horizontalAlignment: Text.AlignHCenter
                            elide: Text.ElideRight
                        }
                        MouseArea {
                            id: memberArea
                            anchors.fill: parent
                            hoverEnabled: true
                            //只有能管理的人点成员才会弹管理菜单，其他人点开成员资料
                            cursorShape: (root.canManage && !modelData.isSelf
                                && modelData.role !== 2) ? Qt.PointingHandCursor
                                                         : Qt.ArrowCursor
                            onClicked: {
                                if (root.canManage && !modelData.isSelf && modelData.role !== 2) {
                                    const point = mapToItem(root, 10, 10)
                                    memberMenu.open(point.x, point.y, [
                                        { text: modelData.role === 1 ? "取消管理员" : "设为管理员",
                                          action: () => controller.setGroupAdmin(modelData.uid,
                                              modelData.role !== 1) },
                                        { text: "移出群聊",
                                          action: () => {
                                              root.pendingRemoveUid = modelData.uid
                                              root.pendingRemoveName = modelData.name
                                              removeConfirm.open()
                                          } }
                                    ])
                                } else {
                                    root.cardMember = modelData
                                    memberCard.open()
                                }
                            }
                        }
                    }
                }
            }

            // 操作提示：邀请成功、权限不足等都在这里出现
            Text {
                width: parent.width
                topPadding: 12
                visible: controller && controller.groupMessage.length > 0
                text: controller ? controller.groupMessage : ""
                textFormat: Text.PlainText
                color: Theme.textSecondary
                font.pixelSize: 11
                wrapMode: Text.Wrap
            }

            Item { width: parent.width; height: 22 }

            // 底部：退出群聊常驻；群主多一个解散入口
            Row {
                spacing: 10
                AppButton {
                    width: 116
                    height: 36
                    variant: "secondary"
                    text: "退出群聊"
                    enabled: !root.isOwner
                    onClicked: leaveConfirm.open()
                }
                AppButton {
                    width: 116
                    height: 36
                    visible: root.isOwner
                    variant: "danger"
                    text: "解散群聊"
                    onClicked: dismissConfirm.open()
                }
            }
            Text {
                width: parent.width
                topPadding: 8
                visible: root.isOwner
                text: "群主不能直接退出，需要先解散群聊。"
                color: Theme.textHint
                font.pixelSize: 11
                wrapMode: Text.Wrap
            }
            Item { width: parent.width; height: 24 }
        }
    }

    ContextMenu { id: memberMenu; anchors.fill: parent }

    // 成员卡：普通成员点头像也能看到是谁，不再是一个"手型但没反应"的死区
    Popup {
        id: memberCard
        anchors.centerIn: parent
        modal: true
        focus: true
        width: 300
        height: 286
        padding: 0
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            radius: 12
            color: Theme.panelBg
            border.width: 1
            border.color: Theme.line
        }

        Column {
            x: 24
            y: 24
            width: parent.width - 48
            spacing: 10

            Avatar {
                anchors.horizontalCenter: parent.horizontalCenter
                width: 64
                height: 64
                radius: 16
                source: root.cardMember.icon || ""
                name: root.cardMember.name || ""
                textSize: 24
            }
            Text {
                width: parent.width
                text: root.cardMember.name || ""
                textFormat: Text.PlainText
                color: Theme.textPrimary
                font.pixelSize: 18
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
            }
            Text {
                width: parent.width
                text: root.cardMember.role === 2 ? "群主"
                    : root.cardMember.role === 1 ? "管理员" : "群成员"
                color: Theme.textSecondary
                font.pixelSize: 11
                horizontalAlignment: Text.AlignHCenter
            }
            Text {
                width: parent.width
                visible: String(root.cardMember.groupNick || "").length > 0
                text: "群昵称：" + String(root.cardMember.groupNick || "")
                textFormat: Text.PlainText
                color: Theme.textHint
                font.pixelSize: 11
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
            }
            AppButton {
                width: parent.width
                height: 36
                text: "关闭"
                onClicked: memberCard.close()
            }
        }
    }

    // 移出群聊：不可撤销，先确认成员名字，避免在头像旁点错一行
    Popup {
        id: removeConfirm
        anchors.centerIn: parent
        modal: true
        focus: true
        width: 340
        height: 176
        padding: 0
        closePolicy: Popup.CloseOnEscape
        onClosed: {
            root.pendingRemoveUid = 0
            root.pendingRemoveName = ""
        }
        background: Rectangle {
            radius: 10
            color: Theme.panelBg
            border.width: 1
            border.color: Theme.line
        }

        Column {
            x: 18
            y: 18
            width: parent.width - 36
            spacing: 14
            Text {
                text: "移出群聊"
                color: Theme.textPrimary
                font.pixelSize: 15
                font.bold: true
            }
            Text {
                width: parent.width
                text: "将「" + root.pendingRemoveName + "」移出这个群？"
                    + "对方会收到移出通知，之后不能再在这里发言。"
                textFormat: Text.PlainText
                color: Theme.textSecondary
                font.pixelSize: 12
                wrapMode: Text.WordWrap
            }
            Row {
                spacing: 10
                anchors.right: parent.right
                AppButton {
                    width: 84
                    height: 34
                    text: "取消"
                    onClicked: removeConfirm.close()
                }
                AppButton {
                    width: 84
                    height: 34
                    text: "移出"
                    onClicked: {
                        controller.removeGroupMember(root.pendingRemoveUid)
                        removeConfirm.close()
                    }
                }
            }
        }
    }

    // 通用编辑弹层：标题 + 输入框 + 取消/保存，群名与公告共用一套样式
    Popup {
        id: nameEditor
        anchors.centerIn: parent
        modal: true
        focus: true
        width: 340
        height: 172
        padding: 0
        closePolicy: Popup.CloseOnEscape
        Overlay.modal: Rectangle { color: Theme.overlay }
        background: Rectangle {
            radius: 10
            color: Theme.panelBg
            border.width: 1
            border.color: Theme.line
        }
        Column {
            x: 18
            y: 16
            width: parent.width - 36
            spacing: 12
            Text { text: "修改群名"; color: Theme.textPrimary; font.pixelSize: 15; font.bold: true }
            AppInput {
                id: nameInput
                width: parent.width
                maximumLength: 30
                placeholder: "群名称"
                text: String(root.info.name || "")
            }
            Row {
                spacing: 10
                anchors.right: parent.right
                AppButton {
                    width: 84
                    height: 34
                    text: "取消"
                    onClicked: nameEditor.close()
                }
                AppButton {
                    width: 84
                    height: 34
                    text: "保存"
                    onClicked: {
                        controller.updateGroupField("name", nameInput.text.trim())
                        nameEditor.close()
                    }
                }
            }
        }
        onOpened: nameInput.forceInputFocus()
    }

    Popup {
        id: noticeEditor
        anchors.centerIn: parent
        modal: true
        focus: true
        width: 400
        height: 320
        padding: 0
        closePolicy: Popup.CloseOnEscape
        Overlay.modal: Rectangle { color: Theme.overlay }
        background: Rectangle {
            radius: 10
            color: Theme.panelBg
            border.width: 1
            border.color: Theme.line
        }
        Column {
            x: 18
            y: 16
            width: parent.width - 36
            spacing: 12
            Text { text: "编辑群公告"; color: Theme.textPrimary; font.pixelSize: 15; font.bold: true }
            AppTextArea {
                id: noticeInput
                width: parent.width
                height: 150
                maxLength: 200
                placeholder: "写点群规、活动安排，最多 200 个字符"
                text: root.notice
            }
            Row {
                spacing: 10
                anchors.right: parent.right
                AppButton {
                    width: 84
                    height: 34
                    text: "取消"
                    onClicked: noticeEditor.close()
                }
                AppButton {
                    width: 84
                    height: 34
                    text: "保存"
                    enabled: !noticeInput.overLimit
                    onClicked: {
                        controller.updateGroupField("notice", noticeInput.text.trim())
                        noticeEditor.close()
                    }
                }
            }
        }
        onOpened: noticeInput.forceInputFocus()
    }

    Popup {
        id: nickEditor
        anchors.centerIn: parent
        modal: true
        focus: true
        width: 340
        height: 172
        padding: 0
        closePolicy: Popup.CloseOnEscape
        Overlay.modal: Rectangle { color: Theme.overlay }
        background: Rectangle {
            radius: 10
            color: Theme.panelBg
            border.width: 1
            border.color: Theme.line
        }
        Column {
            x: 18
            y: 16
            width: parent.width - 36
            spacing: 12
            Text { text: "我在本群的昵称"; color: Theme.textPrimary; font.pixelSize: 15; font.bold: true }
            AppInput {
                id: nickInput
                width: parent.width
                maximumLength: 16
                placeholder: "留空则使用账号昵称"
                text: String(root.info.myNick || "")
            }
            Row {
                spacing: 10
                anchors.right: parent.right
                AppButton {
                    width: 84
                    height: 34
                    text: "取消"
                    onClicked: nickEditor.close()
                }
                AppButton {
                    width: 84
                    height: 34
                    text: "保存"
                    onClicked: {
                        controller.updateGroupField("nickname", nickInput.text.trim())
                        nickEditor.close()
                    }
                }
            }
        }
        onOpened: nickInput.forceInputFocus()
    }

    // 邀请成员：列出还没进群的好友，勾选后一次提交
    Popup {
        id: invitePopup
        anchors.centerIn: parent
        modal: true
        focus: true
        width: 360
        height: 420
        padding: 0
        closePolicy: Popup.CloseOnEscape
        Overlay.modal: Rectangle { color: Theme.overlay }
        background: Rectangle {
            radius: 10
            color: Theme.panelBg
            border.width: 1
            border.color: Theme.line
        }
        property var selected: []

        function toggle(uid) {
            const next = []
            let found = false
            for (let i = 0; i < selected.length; ++i) {
                if (selected[i] === uid)
                    found = true
                else
                    next.push(selected[i])
            }
            if (!found)
                next.push(uid)
            selected = next
        }

        Column {
            x: 18
            y: 16
            width: parent.width - 36
            height: parent.height - 32
            spacing: 10
            Text { text: "邀请好友加入"; color: Theme.textPrimary; font.pixelSize: 15; font.bold: true }
            ListView {
                width: parent.width
                height: parent.height - 102
                clip: true
                model: root.inviteCandidates()
                spacing: 2
                boundsBehavior: Flickable.StopAtBounds
                delegate: Item {
                    width: ListView.view.width
                    height: 46
                    Rectangle {
                        anchors.fill: parent
                        radius: 6
                        color: inviteArea.containsMouse ? Theme.panelAlt : "transparent"
                    }
                    Avatar {
                        x: 6
                        width: 30
                        height: 30
                        radius: 8
                        anchors.verticalCenter: parent.verticalCenter
                        source: modelData.icon
                        name: modelData.name
                    }
                    Text {
                        x: 46
                        width: parent.width - 90
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData.name
                        textFormat: Text.PlainText
                        color: Theme.textPrimary
                        font.pixelSize: 12
                        elide: Text.ElideRight
                    }
                    Rectangle {
                        anchors.right: parent.right
                        anchors.rightMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        width: 18
                        height: 18
                        radius: 5
                        color: invitePopup.selected.indexOf(modelData.uid) >= 0
                            ? Theme.primary : "transparent"
                        border.width: 1
                        border.color: invitePopup.selected.indexOf(modelData.uid) >= 0
                            ? Theme.primary : Theme.fieldBorder
                    }
                    MouseArea {
                        id: inviteArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: invitePopup.toggle(modelData.uid)
                    }
                }
            }
            Row {
                spacing: 10
                anchors.right: parent.right
                AppButton {
                    width: 84
                    height: 34
                    text: "取消"
                    onClicked: invitePopup.close()
                }
                AppButton {
                    width: 84
                    height: 34
                    text: "邀请"
                    enabled: invitePopup.selected.length > 0
                    onClicked: {
                        controller.inviteGroupMembers(invitePopup.selected)
                        invitePopup.selected = []
                        invitePopup.close()
                    }
                }
            }
        }
    }

    // 退出与解散都不可撤销，先确认
    Popup {
        id: leaveConfirm
        anchors.centerIn: parent
        modal: true
        width: 320
        height: 150
        padding: 0
        background: Rectangle { radius: 10; color: Theme.panelBg; border.width: 1; border.color: Theme.line }
        Column {
            x: 18
            y: 18
            width: parent.width - 36
            spacing: 14
            Text { text: "退出后不再接收这个群的消息"; color: Theme.textPrimary; font.pixelSize: 13; wrapMode: Text.Wrap; width: parent.width }
            Row {
                spacing: 10
                anchors.right: parent.right
                AppButton { width: 84; height: 34; text: "取消"; onClicked: leaveConfirm.close() }
                AppButton {
                    width: 84
                    height: 34
                    text: "退出"
                    onClicked: { controller.leaveGroup(); leaveConfirm.close() }
                }
            }
        }
    }

    Popup {
        id: dismissConfirm
        anchors.centerIn: parent
        modal: true
        width: 320
        height: 150
        padding: 0
        background: Rectangle { radius: 10; color: Theme.panelBg; border.width: 1; border.color: Theme.line }
        Column {
            x: 18
            y: 18
            width: parent.width - 36
            spacing: 14
            Text { text: "解散后所有成员都会失去这个群，消息无法恢复"; color: Theme.textPrimary; font.pixelSize: 13; wrapMode: Text.Wrap; width: parent.width }
            Row {
                spacing: 10
                anchors.right: parent.right
                AppButton { width: 84; height: 34; text: "取消"; onClicked: dismissConfirm.close() }
                AppButton {
                    width: 84
                    height: 34
                    text: "解散"
                    onClicked: { controller.dismissGroup(); dismissConfirm.close() }
                }
            }
        }
    }

    // 编辑群资料：头像、群名、公告一次改完；单项仍可在页面上就地改
    Popup {
        id: editProfile
        anchors.centerIn: parent
        modal: true
        focus: true
        width: 380
        height: 362
        padding: 0
        closePolicy: Popup.CloseOnEscape
        Overlay.modal: Rectangle { color: Theme.overlay }
        background: Rectangle {
            radius: 10
            color: Theme.panelBg
            border.width: 1
            border.color: Theme.line
        }

        property string draftName: ""
        property string draftNotice: ""

        function begin() {
            draftName = String(root.info.name || "")
            draftNotice = root.notice
            root.iconError = ""
            open()
        }

        Column {
            x: 18
            y: 16
            width: parent.width - 36
            spacing: 12
            Text { text: "编辑群资料"; color: Theme.textPrimary; font.pixelSize: 15; font.bold: true }

            Row {
                spacing: 12
                Avatar {
                    width: 48
                    height: 48
                    radius: 12
                    group: true
                    source: String(root.info.iconSource || "")
                    name: editProfile.draftName.length > 0 ? editProfile.draftName : "群聊"
                    textSize: 16
                }
                AppLink {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "更换头像"
                    onClicked: iconPicker.open()
                }
            }

            AppInput {
                width: parent.width
                maximumLength: 30
                placeholder: "群名称"
                text: editProfile.draftName
                onTextEdited: editProfile.draftName = text
            }

            AppTextArea {
                width: parent.width
                height: 84
                maxLength: 200
                placeholder: "群公告（可留空）"
                text: editProfile.draftNotice
                onTextEdited: editProfile.draftNotice = text
            }

            Text {
                width: parent.width
                visible: root.iconError.length > 0
                text: root.iconError
                color: Theme.danger
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
            }

            Row {
                spacing: 10
                anchors.right: parent.right
                AppButton {
                    width: 84
                    height: 34
                    variant: "secondary"
                    text: "取消"
                    onClicked: editProfile.close()
                }
                AppButton {
                    width: 84
                    height: 34
                    text: "保存"
                    enabled: editProfile.draftName.trim().length > 0
                    onClicked: {
                        const name = editProfile.draftName.trim()
                        if (name.length > 0)
                            controller.updateGroupField("name", name)
                        controller.updateGroupField("notice", editProfile.draftNotice.trim())
                        editProfile.close()
                    }
                }
            }
        }
    }

    FileDialog {
        id: iconPicker
        title: "选择群头像"
        nameFilters: ["图片 (*.png *.jpg *.jpeg *.webp *.bmp)"]
        onAccepted: {
            root.iconError = ""
            controller.updateGroupIcon(selectedFile)
        }
    }

    Connections {
        target: root.controller
        function onGroupIconFailed(message) { console.log("group icon failed:", message); root.iconError = message }
    }
}
