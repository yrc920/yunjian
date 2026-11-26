import QtQuick
import QtQuick.Layouts
import YunJian

// 找回密码通过用户名、邮箱与验证码验证，状态由 ResetBridge 提供。
// 与注册页同一套字段节奏：单列、浮动标签、行内验证码按钮、两次密码。
Item {
    id: page

    // 两次新密码不一致只在本地拦截，不把半成品交给 bridge
    property string localError: ""

    ResetBridge {
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
            text: "找回密码"
            color: Theme.textPrimary
            font.pixelSize: Theme.fontHero
            font.weight: Font.DemiBold
        }

        Item { width: 1; height: 6 }

        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: "填上注册邮箱，验证码马上寄到。"
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

        // 验证码行：输入框占满剩余宽度，按钮宽度固定，倒计时期间不可用
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
            label: "新密码"
            placeholder: "6–15 位密码"
            password: true
            onRevealToggled: reveal = !reveal
            onTextEdited: {
                bridge.password = text
                page.localError = ""
            }
            onAccepted: confirmInput.forceInputFocus()
        }

        Item { width: 1; height: 12 }

        AppInput {
            id: confirmInput
            width: parent.width
            labeled: true
            label: "确认新密码"
            password: true
            onRevealToggled: reveal = !reveal
            onTextEdited: page.localError = ""
            onAccepted: page.submit()
        }

        Item { width: 1; height: 12 }

        // 提示位固定成一行：本地不一致优先，其次才是 bridge 的成功/失败
        Text {
            width: parent.width
            text: page.localError.length > 0 ? page.localError : bridge.tipText
            color: page.localError.length > 0 ? Theme.danger
                 : bridge.tipOk ? Theme.success : Theme.danger
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
            text: bridge.done ? "密码已更新" : (bridge.busy ? "正在更新…" : "更新密码")
            busy: bridge.busy || bridge.done
            onClicked: page.submit()
        }

    }

    function submit() {
        if (passwordInput.text.length === 0) {
            page.localError = "先填一个新密码。"
            passwordInput.forceInputFocus()
            return
        }
        if (passwordInput.text !== confirmInput.text) {
            page.localError = "两次输入不一致，再确认一下。"
            confirmInput.forceInputFocus()
            return
        }
        page.localError = ""
        bridge.submit()
    }

    // 从登录页带过来的邮箱与用户名
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
            AuthDraft.notice = "密码已更新，用新密码登录。"
            AuthDraft.clearPassword()
        }
    }

    Component.onCompleted: {
        restoreDraft()
        userInput.forceInputFocus()
    }
}
