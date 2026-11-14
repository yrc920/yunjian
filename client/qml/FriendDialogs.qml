import QtQuick
import XiguChat

// 好友表单共用一个弹层；标题和操作始终可见，内容按需滚动。
// overlay 只有两种形态：apply 填写申请、auth 通过申请。
FocusScope {
    id: root
    objectName: "friendDialogs"

    required property ChatBridge controller
    property string overlay: ""
    property int pendingUid: 0
    property string pendingName: ""
    property string pendingNick: ""
    property string pendingDesc: ""
    property string pendingIcon: ""
    // 本次申请要加的分组标签，提交后由服务端保存
    property var selectedTags: []

    visible: overlay !== ""
    focus: visible
    z: 10
    Keys.onEscapePressed: closeDialog()

    // 标题与主按钮文案由 overlay 推导，申请和通过共用一套骨架
    readonly property string heading: overlay === "apply" ? "添加朋友" : "通过好友申请"
    readonly property string actionText: overlay === "apply" ? "发送申请" : "同意添加"

    function closeDialog() {
        overlay = ""
    }

    // 同意方不需要写留言，只确认备注名；服务端会补一句默认欢迎语
    function openAuthDialog(uid, name, desc) {
        pendingUid = uid
        pendingName = name
        pendingDesc = desc
        pendingIcon = ""
        authRemarkInput.text = ""
        overlay = "auth"
        content.contentY = 0
        authRemarkInput.forceInputFocus()
    }

    // 申请表单只给一个默认文案，用户可以删空；空串原样提交，由服务端兜底
    function defaultWelcome() {
        return controller.myUserName.length > 0
            ? "你好，我是" + controller.myUserName
            : "你好，很高兴认识你"
    }

    function openApplyDialog(uid, name) {
        pendingUid = uid
        pendingName = name
        pendingNick = controller.searchResult.nick || ""
        pendingDesc = controller.searchResult.desc || ""
        pendingIcon = controller.searchResult.icon || ""
        selectedTags = []
        verifyInput.text = defaultWelcome()
        // 备注是可选项：不填就不写库，之后在好友资料页随时可以补
        applyRemarkInput.text = ""
        overlay = "apply"
        content.contentY = 0
        verifyInput.forceInputFocus()
    }

    function submit() {
        if (overlay === "apply") {
            // 留空就原样发空，由服务端回退到默认留言；不要在这里再补一次默认句
            controller.applyTo(pendingUid, pendingName, pendingNick, pendingIcon,
                               verifyInput.text.trim(),
                               applyRemarkInput.text.trim(),
                               selectedTags)
            // 浮层留在原地，由它显示「发送中 / 已发送 / 失败」
            overlay = ""
        } else if (overlay === "auth") {
            controller.authApply(pendingUid, authRemarkInput.text.trim())
            overlay = ""
        } else {
            closeDialog()
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
                        source: root.pendingIcon
                        name: root.pendingName
                        textSize: 19
                    }
                    Column {
                        width: parent.width - 66
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 6
                        Text {
                            width: parent.width
                            text: root.pendingName
                            textFormat: Text.PlainText
                            color: Theme.textPrimary
                            font.pixelSize: 17
                            font.weight: Font.DemiBold
                            wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                        }
                        Text {
                            text: "ID  " + root.pendingUid
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontSmall
                            font.letterSpacing: 0.5
                        }
                    }
                }

                Column {
                    width: parent.width
                    visible: root.overlay === "auth"
                    spacing: 16
                    Spacer { gap: 4 }
                    Text {
                        width: parent.width
                        text: root.pendingDesc || "请求添加你为好友"
                        textFormat: Text.PlainText
                        visible: text.length > 0
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontBody
                        wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                    }
                    Rectangle {
                        width: parent.width
                        height: 1
                        color: Theme.line
                    }
                    Spacer { gap: 2 }
                }

                Column {
                    width: parent.width
                    visible: root.overlay === "apply"
                    spacing: 0
                    Spacer { gap: 24 }
                    Text {
                        text: "申请留言"
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSmall
                    }
                    Spacer { gap: 8 }
                    AppInput {
                        id: verifyInput
                        width: parent.width
                        placeholder: "介绍一下自己"
                        onAccepted: applyRemarkInput.forceInputFocus()
                    }
                    Spacer { gap: 18 }
                    Text {
                        text: "备注名"
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSmall
                    }
                    Spacer { gap: 8 }
                    AppInput {
                        id: applyRemarkInput
                        width: parent.width
                        maximumLength: 20
                        placeholder: "可选，之后也能在资料页修改"
                        onAccepted: tagInput.forceInputFocus()
                    }
                    Spacer { gap: 20 }
                    Text {
                        text: "标签"
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSmall
                    }
                    Spacer { gap: 10 }
                    // 标签随申请一起提交，通过后直接成为自己的好友分组
                    TagEditor {
                        width: parent.width
                        tags: root.selectedTags
                        onCommitted: (list) => root.selectedTags = list
                    }
                    Spacer { gap: 4 }
                }

                Column {
                    width: parent.width
                    visible: root.overlay === "auth"
                    spacing: 8
                    Text {
                        text: "备注名"
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSmall
                    }
                    AppInput {
                        id: authRemarkInput
                        width: parent.width
                        maximumLength: 20
                        placeholder: "可选，之后也能在资料页修改"
                        onAccepted: root.submit()
                    }
                }
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
                    text: root.actionText
                    onClicked: root.submit()
                }
            }
        }
    }
}
