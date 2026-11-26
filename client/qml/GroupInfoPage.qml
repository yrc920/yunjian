import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import YunJian

// 群资料：信头式布局，内容列限宽 680 并居中，左右各留 48px 起。
// 群名、群公告、我在本群的昵称都能就地改；"编辑群资料"保留批量入口。
// 所有右对齐元素贴内容列右缘，不再比左缘多缩进，避免文字贴着边框。
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

    property var cardMember: ({})
    property int pendingRemoveUid: 0
    property string pendingRemoveName: ""

    property string iconError: ""
    property string savingField: ""

    property bool batchSaving: false
    property string batchError: ""

    signal backRequested()

    // 群资料单列居中，左右至少各留 48，和好友/个人资料同一套留白
    readonly property real contentWidth: Math.min(width - 96, 680)
    readonly property real inset: Math.max(48, (width - contentWidth) / 2)
    readonly property int memberColumns: contentWidth >= 560 ? 6 : 4

    function memberExists(uid) {
        for (let i = 0; i < members.length; ++i) {
            if (members[i].uid === uid)
                return true
        }
        return false
    }

    function inviteCandidates() {
        const list = []
        for (let i = 0; i < candidates.length; ++i) {
            if (!memberExists(candidates[i].uid))
                list.push(candidates[i])
        }
        return list
    }

    // 外部入口：例如从资料页顶部直接进入改名，不必先点一遍群名
    function editName() {
        nameField.begin()
    }

    function beginField(key) {
        if (controller)
            controller.clearGroupMessage()
    }

    function saveField(key, action, value) {
        if (!controller || !canManage)
            return
        savingField = key
        controller.updateGroupField(action, String(value).trim())
    }

    Connections {
        target: root.controller
        function onGroupFieldSaved(ok, action, message) {
            if (root.batchSaving) {
                if (!ok) {
                    root.batchSaving = false
                    root.batchError = message.length > 0 ? message : "没有保存，检查网络后重试"
                    return
                }
                const wantedNotice = editProfile.draftNotice.trim()
                if (action === "name" && wantedNotice !== root.notice) {
                    root.controller.updateGroupField("notice", wantedNotice)
                    return
                }
                root.batchSaving = false
                editProfile.close()
                return
            }
            if (root.savingField.length === 0)
                return
            const expected = root.savingField === "name" ? "name"
                           : root.savingField === "notice" ? "notice" : "nickname"
            if (action !== expected)
                return
            const field = root.savingField === "name" ? nameField
                        : root.savingField === "notice" ? noticeField : nickField
            if (ok)
                field.finish()
            else
                field.fail(message)
            root.savingField = ""
        }
        function onGroupIconFailed(message) {
            root.iconError = message
        }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.sheet
    }

    PageHeader {
        id: header
        title: "群聊资料"
        inset: 24
        backVisible: true
        backLabel: root.controller && root.controller.mode === ChatBridge.ContactMode
            ? "返回联系人" : "返回会话"
        onBackRequested: root.backRequested()
    }

    Flickable {
        id: body
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        contentWidth: width
        contentHeight: content.height + 56
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        Column {
            id: content
            x: root.inset
            y: 32
            width: root.contentWidth
            spacing: 0

            // 信头：头像 + 群名 + 成员数；改名时就地展开，信头跟着长高，不压住成员数
            Item {
                width: parent.width
                height: nameField.editing ? nameField.height + 18 : 80

                AvatarAction {
                    width: 64
                    height: 64
                    group: true
                    source: String(root.info.iconSource || "")
                    name: String(root.info.name || "群聊")
                    editable: root.canManage
                    onEditRequested: {
                        root.iconError = ""
                        iconPicker.open()
                    }
                }

                EditableField {
                    id: nameField
                    x: 80
                    y: 2
                    width: parent.width - 80
                    showLabel: false
                    labelWidth: 0
                    value: String(root.info.name || "群聊")
                    placeholder: "群聊"
                    valueSize: Theme.fontDisplay
                    valueWeight: Font.DemiBold
                    maxLength: 30
                    editable: root.canManage
                    busy: root.savingField === "name" && root.controller && root.controller.groupBusy
                    onEditStarted: root.beginField("name")
                    onCommitted: (value) => {
                        if (String(value).trim().length === 0) {
                            nameField.fail("群名不能为空")
                            return
                        }
                        root.saveField("name", "name", value)
                    }
                }

                Text {
                    x: 80
                    y: 56
                    visible: !nameField.editing
                    text: root.memberCount + " 位成员"
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontCaption
                }
            }

            Text {
                width: parent.width
                visible: root.iconError.length > 0
                bottomPadding: 8
                text: root.iconError
                color: Theme.danger
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.iconError = ""
                }
            }

            // 分区标题：左"群资料"，右批量入口；两者贴同一条基线
            Item {
                width: parent.width
                height: 32

                Text {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    text: "群资料"
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSmall
                    font.weight: Font.Medium
                }

                Text {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    visible: root.canManage
                    text: "编辑群资料"
                    color: Theme.primary
                    font.pixelSize: Theme.fontSmall
                    MouseArea {
                        anchors.fill: parent
                        anchors.margins: -8
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            root.batchError = ""
                            editProfile.openWith(root.info.name || "", root.notice)
                        }
                    }
                }
            }

            Rectangle {
                width: parent.width
                height: 1
                color: Theme.lineSoft
            }

            EditableField {
                id: noticeField
                width: parent.width
                visible: root.canManage || root.notice.length > 0
                label: "群公告"
                value: root.notice
                placeholder: "还没有群公告"
                mode: "multiline"
                maxLength: 200
                maxLines: 3
                editable: root.canManage
                busy: root.savingField === "notice" && root.controller && root.controller.groupBusy
                onEditStarted: root.beginField("notice")
                onCommitted: (value) => root.saveField("notice", "notice", value)
            }

            EditableField {
                id: nickField
                width: parent.width
                label: "群昵称"
                value: String(root.info.myNick || "")
                placeholder: "未设置"
                maxLength: 16
                busy: root.savingField === "nickname" && root.controller && root.controller.groupBusy
                onEditStarted: root.beginField("nickname")
                onCommitted: (value) => root.saveField("nickname", "nickname", value)
            }

            // 免打扰：只影响本机提示
            Item {
                width: parent.width
                height: 58

                Column {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 3
                    Text {
                        text: "消息免打扰"
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontBody
                    }
                    Text {
                        text: "只影响这台设备的提示音与横幅"
                        color: Theme.textHint
                        font.pixelSize: Theme.fontCaption
                    }
                }

                Rectangle {
                    anchors.right: parent.right
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
                        color: root.muted ? Theme.primaryText : Theme.textHint
                        Behavior on x { NumberAnimation { duration: 120 } }
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: if (root.controller) root.controller.toggleGroupMute()
                }
            }

            Item { width: 1; height: 12 }

            // 成员区
            Item {
                width: parent.width
                height: 28

                Text {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    text: "群成员 · " + root.memberCount
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSmall
                    font.weight: Font.Medium
                }
                AppLink {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    visible: root.memberCount > 0
                    text: "邀请好友"
                    onClicked: invitePopup.openWith(root.inviteCandidates())
                }
            }

            Item { width: 1; height: 6 }

            Flow {
                width: parent.width
                spacing: 8

                Repeater {
                    model: root.members
                    delegate: Item {
                        width: (content.width - (root.memberColumns - 1) * 8) / root.memberColumns
                        height: 88

                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: 1
                            radius: 8
                            color: memberArea.containsMouse ? Theme.panelAlt : "transparent"
                        }

                        Avatar {
                            id: memberAvatar
                            anchors.horizontalCenter: parent.horizontalCenter
                            y: 6
                            width: 44
                            height: 44
                            radius: 11
                            source: modelData.icon
                            name: modelData.name
                        }

                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.top: memberAvatar.bottom
                            anchors.topMargin: 4
                            width: parent.width - 8
                            text: modelData.name
                            textFormat: Text.PlainText
                            color: Theme.textPrimary
                            font.pixelSize: Theme.fontSmall
                            horizontalAlignment: Text.AlignHCenter
                            elide: Text.ElideRight
                        }

                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.top: memberAvatar.bottom
                            anchors.topMargin: 20
                            visible: modelData.role === 2 || modelData.role === 1
                            text: modelData.role === 2 ? "群主" : "管理员"
                            color: Theme.textHint
                            font.pixelSize: Theme.fontCaption
                        }

                        MouseArea {
                            id: memberArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
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
                                              removeConfirm.ask()
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

            Text {
                width: parent.width
                topPadding: 10
                visible: root.controller && root.controller.groupMessage.length > 0
                text: root.controller ? root.controller.groupMessage : ""
                textFormat: Text.PlainText
                color: Theme.danger
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            }

            Item { width: 1; height: 24 }

            Rectangle {
                width: parent.width
                height: 1
                color: Theme.lineSoft
            }

            Item { width: 1; height: 16 }

            // 危险区：只留文字链与后果说明，不做整行大按钮
            Column {
                width: parent.width
                spacing: 4

                AppLink {
                    text: root.isOwner ? "解散群聊" : "退出群聊"
                    danger: true
                    onClicked: root.isOwner ? dismissConfirm.ask() : leaveConfirm.ask()
                }
                Text {
                    width: parent.width
                    text: root.isOwner
                        ? "群主不能直接退出，需要先解散群聊。解散后消息无法恢复。"
                        : "退出后不再接收这个群的消息，本机聊天记录仍会保留。"
                    color: Theme.textHint
                    font.pixelSize: Theme.fontCaption
                    wrapMode: Text.WordWrap
                }
            }

            Item { width: 1; height: 28 }
        }
    }

    ContextMenu { id: memberMenu; anchors.fill: parent }

    ConfirmDialog {
        id: removeConfirm
        title: "移出群聊"
        body: "将「" + root.pendingRemoveName + "」移出这个群？对方会收到移出通知，之后不能再在这里发言。"
        confirmText: "移出"
        danger: true
        onConfirmed: {
            if (root.pendingRemoveUid > 0 && root.controller)
                root.controller.removeGroupMember(root.pendingRemoveUid)
            close()
        }
        onCancelled: {
            root.pendingRemoveUid = 0
            root.pendingRemoveName = ""
        }
    }

    ConfirmDialog {
        id: leaveConfirm
        title: "退出群聊"
        body: "退出后不再接收这个群的消息；本机聊天记录仍会保留。"
        confirmText: "退出群聊"
        danger: true
        onConfirmed: {
            if (root.controller)
                root.controller.leaveGroup()
            close()
        }
    }

    ConfirmDialog {
        id: dismissConfirm
        title: "解散群聊"
        body: "解散后所有成员都会失去这个群，消息无法恢复。"
        confirmText: "解散群聊"
        danger: true
        onConfirmed: {
            if (root.controller)
                root.controller.dismissGroup()
            close()
        }
    }

    Popup {
        id: memberCard
        anchors.centerIn: parent
        modal: true
        focus: true
        width: 300
        height: cardColumn.implicitHeight + 40
        padding: 0
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        Overlay.modal: Rectangle { color: Theme.overlay }

        background: Rectangle {
            radius: Theme.dialogRadius
            color: Theme.panelBg
            border.width: 1
            border.color: Theme.line
        }

        Column {
            id: cardColumn
            x: 20
            y: 20
            width: parent.width - 40
            spacing: 10

            Avatar {
                anchors.horizontalCenter: parent.horizontalCenter
                width: 64
                height: 64
                radius: 16
                source: root.cardMember.icon || ""
                name: root.cardMember.name || ""
                textSize: 22
            }
            Text {
                width: parent.width
                text: root.cardMember.name || ""
                textFormat: Text.PlainText
                color: Theme.textPrimary
                font.pixelSize: Theme.fontTitle
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
            }
            Text {
                width: parent.width
                text: root.cardMember.role === 2 ? "群主"
                    : root.cardMember.role === 1 ? "管理员" : "群成员"
                color: Theme.textSecondary
                font.pixelSize: Theme.fontCaption
                horizontalAlignment: Text.AlignHCenter
            }
            Text {
                width: parent.width
                visible: String(root.cardMember.groupNick || "").length > 0
                text: "群昵称：" + String(root.cardMember.groupNick || "")
                textFormat: Text.PlainText
                color: Theme.textHint
                font.pixelSize: Theme.fontCaption
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
            }
            AppButton {
                width: parent.width
                height: 36
                text: "关闭"
                variant: "secondary"
                onClicked: memberCard.close()
            }
        }
    }

    // 批量编辑：头像立即生效，群名与公告按顺序保存
    Popup {
        id: editProfile
        anchors.centerIn: parent
        modal: true
        focus: true
        width: 420
        height: batchColumn.implicitHeight + 48
        padding: 0
        closePolicy: Popup.CloseOnEscape
        Overlay.modal: Rectangle { color: Theme.overlay }

        property string draftName: ""
        property string draftNotice: ""

        onClosed: {
            root.batchSaving = false
            root.batchError = ""
        }

        function openWith(name, notice) {
            draftName = name
            draftNotice = notice
            root.batchError = ""
            root.iconError = ""
            nameInput.text = name
            noticeInput.text = notice
            open()
        }

        background: Rectangle {
            radius: Theme.dialogRadius
            color: Theme.panelBg
            border.width: 1
            border.color: Theme.line
        }

        Column {
            id: batchColumn
            x: 24
            y: 24
            width: parent.width - 48
            spacing: 12

            Text {
                text: "编辑群资料"
                color: Theme.textPrimary
                font.pixelSize: Theme.fontTitle
                font.weight: Font.DemiBold
            }

            Row {
                width: parent.width
                spacing: 14

                AvatarAction {
                    width: 56
                    height: 56
                    group: true
                    source: String(root.info.iconSource || "")
                    name: editProfile.draftName.length > 0 ? editProfile.draftName : "群聊"
                    onEditRequested: {
                        root.iconError = ""
                        iconPicker.open()
                    }
                }

                Column {
                    width: parent.width - 70
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 4
                    Text {
                        text: "群头像"
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontSmall
                    }
                    Text {
                        text: "点头像更换，选图后立即生效"
                        color: Theme.textHint
                        font.pixelSize: Theme.fontCaption
                    }
                }
            }

            Text {
                visible: root.iconError.length > 0
                width: parent.width
                text: root.iconError
                color: Theme.danger
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            }

            Text {
                text: "群名称"
                color: Theme.textSecondary
                font.pixelSize: Theme.fontCaption
            }
            AppInput {
                id: nameInput
                width: parent.width
                maximumLength: 30
                placeholder: "群名称"
                onTextEdited: editProfile.draftName = text
            }

            Text {
                text: "群公告"
                color: Theme.textSecondary
                font.pixelSize: Theme.fontCaption
            }
            AppTextArea {
                id: noticeInput
                width: parent.width
                height: 96
                maxLength: 200
                placeholder: "写点群规、活动安排，最多 200 个字符"
                onTextEdited: editProfile.draftNotice = text
            }

            Text {
                width: parent.width
                visible: root.batchError.length > 0
                text: root.batchError
                color: Theme.danger
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            }

            Row {
                width: parent.width
                spacing: 12

                CodeButton {
                    width: (parent.width - 12) / 2
                    height: 40
                    text: "取消"
                    onClicked: editProfile.close()
                }
                AppButton {
                    width: (parent.width - 12) / 2
                    height: 40
                    text: root.batchSaving ? "正在保存…" : "保存"
                    busy: root.batchSaving
                    onClicked: {
                        const name = editProfile.draftName.trim()
                        if (name.length === 0) {
                            root.batchError = "群名不能为空"
                            return
                        }
                        const notice = editProfile.draftNotice.trim()
                        if (name === String(root.info.name || "") && notice === root.notice) {
                            editProfile.close()
                            return
                        }
                        root.batchError = ""
                        root.batchSaving = true
                        if (name !== String(root.info.name || ""))
                            root.controller.updateGroupField("name", name)
                        else
                            root.controller.updateGroupField("notice", notice)
                    }
                }
            }
        }
    }

    Popup {
        id: invitePopup
        anchors.centerIn: parent
        modal: true
        focus: true
        width: 400
        height: 440
        padding: 0
        closePolicy: Popup.CloseOnEscape
        Overlay.modal: Rectangle { color: Theme.overlay }

        property var selected: []

        function openWith(list) {
            selected = []
            open()
        }

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

        background: Rectangle {
            radius: Theme.dialogRadius
            color: Theme.panelBg
            border.width: 1
            border.color: Theme.line
        }

        Column {
            x: 24
            y: 20
            width: parent.width - 48
            height: parent.height - 40
            spacing: 10

            Text {
                text: "邀请好友加入"
                color: Theme.textPrimary
                font.pixelSize: Theme.fontTitle
                font.weight: Font.DemiBold
            }

            Text {
                width: parent.width
                visible: root.inviteCandidates().length === 0
                text: "好友都已经在群里了。"
                color: Theme.textHint
                font.pixelSize: Theme.fontSmall
            }

            ListView {
                width: parent.width
                height: parent.height - 106
                clip: true
                model: root.inviteCandidates()
                spacing: 2
                boundsBehavior: Flickable.StopAtBounds

                delegate: Item {
                    width: ListView.view.width
                    height: 48
                    Rectangle {
                        anchors.fill: parent
                        radius: 8
                        color: inviteArea.containsMouse ? Theme.panelAlt : "transparent"
                    }
                    Avatar {
                        x: 6
                        width: 32
                        height: 32
                        radius: 8
                        anchors.verticalCenter: parent.verticalCenter
                        source: modelData.icon
                        name: modelData.name
                    }
                    Text {
                        x: 48
                        width: parent.width - 94
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData.name
                        textFormat: Text.PlainText
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontSmall
                        elide: Text.ElideRight
                    }
                    Rectangle {
                        anchors.right: parent.right
                        anchors.rightMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        width: 20
                        height: 20
                        radius: 4
                        color: invitePopup.selected.indexOf(modelData.uid) >= 0
                            ? Theme.primary : "transparent"
                        border.width: 1
                        border.color: invitePopup.selected.indexOf(modelData.uid) >= 0
                            ? Theme.primary : Theme.fieldBorder
                        AppIcon {
                            anchors.centerIn: parent
                            visible: invitePopup.selected.indexOf(modelData.uid) >= 0
                            name: "check"
                            size: 12
                            color: Theme.primaryText
                        }
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
                width: parent.width
                spacing: 12
                anchors.horizontalCenter: parent.horizontalCenter

                CodeButton {
                    width: (parent.width - 12) / 2
                    height: 40
                    text: "取消"
                    onClicked: invitePopup.close()
                }
                AppButton {
                    width: (parent.width - 12) / 2
                    height: 40
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

    FileDialog {
        id: iconPicker
        title: "选择群头像"
        nameFilters: ["图片 (*.png *.jpg *.jpeg *.webp *.bmp)"]
        onAccepted: {
            root.iconError = ""
            controller.updateGroupIcon(selectedFile)
        }
    }
}
