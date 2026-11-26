import QtQuick
import QtQuick.Layouts
import YunJian

// 注册信息和请求状态归 bridge，表单负责焦点顺序与密码显示。
// 字段全部单列：标签浮在框上，不再单独占一行，也不把两个密码框挤成半宽。
Item {
    id: page

    RegisterBridge {
        id: bridge
        objectName: "pageBridge"
    }

    AuthPage {
        anchors.fill: parent
        backVisible: true
        onBackRequested: bridge.go("login")
        enabled: !(bridge.busy || bridge.codeBusy || bridge.done)

        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: "创建账号"
            color: Theme.textPrimary
            font.pixelSize: Theme.fontHero
            font.weight: Font.DemiBold
        }

        Item { width: 1; height: 6 }

        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: "先取个名字，再去找想写信的人。"
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSmall
        }

        Item { width: 1; height: 16 }

        AppInput {
            id: userInput
            width: parent.width
            labeled: true
            label: "用户名"
            onTextEdited: {
                bridge.user = text
                AuthDraft.user = text
                AuthDraft.clearNotice()
            }
            onAccepted: emailInput.forceInputFocus()
        }

        Item { width: 1; height: 12 }

        AppInput {
            id: emailInput
            width: parent.width
            labeled: true
            label: "邮箱"
            onTextEdited: {
                bridge.email = text
                AuthDraft.email = text
                AuthDraft.clearNotice()
            }
            onAccepted: codeInput.forceInputFocus()
        }

        Item { width: 1; height: 12 }

        // 验证码输入与发送按钮同一行，倒计时和发送中状态都由 bridge 控制
        RowLayout {
            width: parent.width
            spacing: 10

            AppInput {
                id: codeInput
                Layout.fillWidth: true
                labeled: true
                label: "验证码"
                onTextEdited: bridge.code = text
                onAccepted: passwordInput.forceInputFocus()
            }

            CodeButton {
                Layout.preferredWidth: 112
                text: bridge.countdown > 0 ? bridge.countdown + " 秒后重发"
                                           : (bridge.codeBusy ? "正在发送…" : "获取验证码")
                usable: bridge.countdown === 0 && !bridge.codeBusy && !bridge.busy && !bridge.done
                onClicked: bridge.sendCode()
            }
        }

        Item { width: 1; height: 12 }

        AppInput {
            id: passwordInput
            width: parent.width
            labeled: true
            label: "密码"
            placeholder: "6–15 位"
            password: true
            onRevealToggled: reveal = !reveal
            onTextEdited: bridge.password = text
            onAccepted: confirmInput.forceInputFocus()
        }

        Item { width: 1; height: 12 }

        AppInput {
            id: confirmInput
            width: parent.width
            labeled: true
            label: "确认密码"
            password: true
            onRevealToggled: reveal = !reveal
            onTextEdited: bridge.confirm = text
            onAccepted: bridge.submit()
        }

        Item { width: 1; height: 10 }

        Text {
            width: parent.width
            text: bridge.tipText
            color: bridge.tipOk ? Theme.success : Theme.danger
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.WrapAtWordBoundaryOrAnywhere
            // 固定预留一行：提示出现时表单不上下跳，长文案仍可自然增高
            height: Math.max(18, implicitHeight)
            opacity: text.length > 0 ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 120 } }
        }

        Item { width: 1; height: 10 }

        AppButton {
            width: parent.width
            text: bridge.done ? "账号已创建" : (bridge.busy ? "正在创建…" : "创建账号")
            busy: bridge.busy || bridge.done
            onClicked: bridge.submit()
        }

    }

    // 从登录页带过来的邮箱与用户名：只填空字段，不覆盖用户已经输入的内容
    function restoreDraft() {
        if (bridge.user.length === 0 && AuthDraft.user.length > 0) {
            bridge.user = AuthDraft.user
            userInput.text = AuthDraft.user
        }
        if (bridge.email.length === 0 && AuthDraft.email.length > 0) {
            bridge.email = AuthDraft.email
            emailInput.text = AuthDraft.email
        }
    }

    Connections {
        target: bridge
        function onDoneChanged() {
            if (!bridge.done)
                return
            AuthDraft.user = bridge.user
            AuthDraft.email = bridge.email
            AuthDraft.notice = "账号已创建，用新密码登录。"
            AuthDraft.clearPassword()
        }
    }

    Component.onCompleted: {
        restoreDraft()
        userInput.forceInputFocus()
    }
}
