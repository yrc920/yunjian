import QtQuick
import XiguChat

// 资料编辑共用弹层，两种形态：
//   self   —— 改自己的昵称、性别、个性签名
//   remark —— 给某个好友设备注
// 骨架与 FriendDialogs 一致：遮罩 + 居中面板 + 固定底部操作，内容按需滚动。
FocusScope {
    id: root
    objectName: "profileDialogs"

    required property ChatBridge controller
    property string overlay: ""
    // self 表单
    property string nickText: ""
    property int sexValue: 0
    property string descText: ""
    // remark 表单
    property int remarkUid: 0
    property string remarkDisplay: ""
    property string remarkUser: ""
    property string remarkIcon: ""
    property var remarkTags: []

    visible: overlay !== ""
    focus: visible
    z: 10
    Keys.onEscapePressed: closeDialog()

    readonly property string heading: overlay === "self" ? "编辑资料" : "编辑好友资料"
    readonly property bool saving: overlay === "self"
                                   ? controller.profileSaving : controller.remarkSaving
    readonly property string errorText: overlay === "self"
                                        ? controller.profileMessage : controller.remarkMessage

    function closeDialog() {
        overlay = ""
    }

    // 打开时按当前资料回填，清掉上一次的错误提示
    function openSelf() {
        const profile = controller.myProfile
        nickText = profile.nick || ""
        sexValue = profile.sex === undefined ? 0 : profile.sex
        descText = profile.desc || ""
        // 输入框内容直接赋值：TextInput 一旦被编辑就会断开外部绑定，不能只靠绑定回填
        nickInput.text = nickText
        descInput.text = descText
        controller.clearProfileMessages()
        overlay = "self"
        content.contentY = 0
        nickInput.forceInputFocus()
    }

    function openRemark(uid, display, userName, icon, remark, tags) {
        remarkUid = uid
        remarkDisplay = display
        remarkUser = userName
        remarkIcon = icon
        remarkTags = tags || []
        remarkInput.text = remark || ""
        controller.clearProfileMessages()
        overlay = "remark"
        content.contentY = 0
        remarkInput.forceInputFocus()
    }

    function submit() {
        if (saving)
            return
        if (overlay === "self")
            controller.saveMyProfile(nickText, sexValue, descText)
        else if (overlay === "remark")
            controller.saveFriendRemark(remarkUid, remarkInput.text, remarkTags)
    }

    // 服务端确认后才关闭；失败时保留表单，错误提示留在内容底部
    Connections {
        target: root.controller
        function onProfileSaved(ok) {
            if (ok && root.overlay === "self")
                root.overlay = ""
        }
        function onRemarkSaved(ok, uid) {
            if (ok && root.overlay === "remark" && uid === root.remarkUid)
                root.overlay = ""
        }
    }

    // 遮罩同时负责关闭与拦截滚轮，弹层打开时底层页面不跟着滚动
    Rectangle {
        anchors.fill: parent
        color: Theme.overlay
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onClicked: root.closeDialog()
            onWheel: (wheel) => { wheel.accepted = true }
        }
    }

    Rectangle {
        id: panel
        anchors.centerIn: parent
        width: Math.min(440, root.width - 48)
        height: Math.min(root.height - 48, body.height + 170)
        radius: 12
        color: Theme.panelBg
        border.width: 1
        border.color: Theme.line
        clip: true

        // 面板本身拦截点击，避免点内容时触发遮罩关闭
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
        }

        Rectangle {
            x: 28
            y: 0
            width: 38
            height: 3
            color: Theme.accent
        }

        Item {
            id: header
            width: parent.width
            height: 76
            Text {
                x: 28
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - 92
                text: root.heading
                font.pixelSize: 20
                font.weight: Font.DemiBold
                color: Theme.textPrimary
                elide: Text.ElideRight
            }
            FocusScope {
                id: closeButton
                anchors.right: parent.right
                anchors.rightMargin: 20
                anchors.verticalCenter: parent.verticalCenter
                width: 32
                height: 32
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: "关闭"
                Accessible.onPressAction: root.closeDialog()
                Keys.onReturnPressed: root.closeDialog()
                Keys.onSpacePressed: root.closeDialog()
                Rectangle {
                    anchors.fill: parent
                    radius: 6
                    color: closeArea.containsMouse || closeButton.activeFocus ? Theme.panelAlt : "transparent"
                }
                AppIcon {
                    anchors.centerIn: parent
                    name: "close"
                    size: 18
                    color: Theme.textSecondary
                }
                MouseArea {
                    id: closeArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.closeDialog()
                }
            }
        }

        // 内容区按需滚动，底部按钮固定；右侧细条是自绘滚动指示
        Flickable {
            id: content
            x: 28
            anchors.top: header.bottom
            anchors.bottom: footer.top
            width: parent.width - 56
            contentWidth: width
            contentHeight: body.height
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: body
                width: content.width
                spacing: 0

                Row {
                    width: parent.width
                    spacing: 14
                    Avatar {
                        width: 52
                        height: 52
                        source: root.overlay === "self" ? root.controller.myIcon : root.remarkIcon
                        name: root.overlay === "self" ? root.controller.myName : root.remarkDisplay
                        textSize: 19
                    }
                    Column {
                        width: parent.width - 66
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 6
                        Text {
                            width: parent.width
                            text: root.overlay === "self" ? root.controller.myName : root.remarkDisplay
                            textFormat: Text.PlainText
                            color: Theme.textPrimary
                            font.pixelSize: 17
                            font.weight: Font.DemiBold
                            elide: Text.ElideRight
                        }
                        Text {
                            width: parent.width
                            text: {
                                if (root.overlay === "self")
                                    return "ID  " + (root.controller.myProfile.uid || "")
                                return "@" + root.remarkUser + "  ·  ID " + root.remarkUid
                            }
                            textFormat: Text.PlainText
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontSmall
                            elide: Text.ElideRight
                        }
                    }
                }

                // 本人资料表单
                Column {
                    width: parent.width
                    visible: root.overlay === "self"
                    spacing: 0
                    Spacer { gap: 24 }
                    Text {
                        text: "昵称"
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSmall
                    }
                    Spacer { gap: 8 }
                    AppInput {
                        id: nickInput
                        width: parent.width
                        maximumLength: 20
                        placeholder: "留空则显示用户名"
                        onTextEdited: root.nickText = text
                        onAccepted: root.submit()
                    }
                    Spacer { gap: 18 }
                    Text {
                        text: "性别"
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSmall
                    }
                    Spacer { gap: 10 }
                    Row {
                        spacing: 8
                        // 0 不公开，1 男，2 女；与资料页的显示规则保持一致
                        Repeater {
                            model: [{ label: "不公开", value: 0 }, { label: "男", value: 1 }, { label: "女", value: 2 }]
                            delegate: Rectangle {
                                readonly property bool chosen: root.sexValue === modelData.value
                                width: sexLabel.implicitWidth + 34
                                height: 32
                                radius: 5
                                color: chosen ? Theme.selected : Theme.panelAlt
                                border.width: 1
                                border.color: chosen ? Theme.primary : "transparent"
                                Text {
                                    id: sexLabel
                                    anchors.centerIn: parent
                                    text: modelData.label
                                    color: chosen ? Theme.textPrimary : Theme.textSecondary
                                    font.pixelSize: Theme.fontBody
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.sexValue = modelData.value
                                }
                            }
                        }
                    }
                    Spacer { gap: 18 }
                    Text {
                        text: "个性签名"
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSmall
                    }
                    Spacer { gap: 8 }
                    AppInput {
                        id: descInput
                        width: parent.width
                        maximumLength: 60
                        placeholder: "一句话介绍自己"
                        onTextEdited: root.descText = text
                        onAccepted: root.submit()
                    }
                    Spacer { gap: 10 }
                    Text {
                        width: parent.width
                        text: "用户名用于登录与他人搜索，不支持修改。"
                        color: Theme.textHint
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }
                    Spacer { gap: 12 }
                }

                // 好友资料表单：备注 + 分组标签，都只对自己生效
                Column {
                    width: parent.width
                    visible: root.overlay === "remark"
                    spacing: 0
                    Spacer { gap: 24 }
                    Text {
                        text: "备注名"
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSmall
                    }
                    Spacer { gap: 8 }
                    AppInput {
                        id: remarkInput
                        width: parent.width
                        maximumLength: 20
                        placeholder: "留空则显示对方的昵称"
                        onAccepted: root.submit()
                    }
                    Spacer { gap: 20 }
                    Text {
                        text: "标签"
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSmall
                    }
                    Spacer { gap: 10 }
                    TagEditor {
                        width: parent.width
                        tags: root.remarkTags
                        onCommitted: (list) => root.remarkTags = list
                    }
                    Spacer { gap: 12 }
                    Text {
                        width: parent.width
                        text: "备注与标签只对你自己生效，对方看到的是他自己的昵称。"
                        color: Theme.textHint
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }
                    Spacer { gap: 12 }
                }

                // 保存失败的提示；成功会直接关闭弹层
                Text {
                    width: parent.width
                    text: root.errorText
                    visible: text.length > 0
                    color: Theme.danger
                    font.pixelSize: Theme.fontSmall
                    wrapMode: Text.WordWrap
                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                }
                Spacer { gap: 8 }
            }
        }

        Rectangle {
            visible: content.contentHeight > content.height
            width: 2
            x: panel.width - 12
            y: header.height + (content.contentY / Math.max(1, content.contentHeight - content.height))
               * (content.height - height)
            height: Math.max(24, content.height * content.height / Math.max(1, content.contentHeight))
            color: Theme.textHint
            radius: 1
        }

        Item {
            id: footer
            anchors.bottom: parent.bottom
            width: parent.width
            height: 94
            Row {
                anchors.centerIn: parent
                width: parent.width - 56
                spacing: 12
                CodeButton {
                    width: (parent.width - 12) / 2
                    text: "取消"
                    onClicked: root.closeDialog()
                }
                AppButton {
                    width: (parent.width - 12) / 2
                    text: root.saving ? "保存中…" : "保存"
                    busy: root.saving
                    onClicked: root.submit()
                }
            }
        }
    }
}
