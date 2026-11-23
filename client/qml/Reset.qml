import QtQuick
import QtQuick.Layouts
import XiguChat

// 找回密码通过用户名、邮箱与验证码验证，状态由 ResetBridge 提供。
// 与注册页同一套字段节奏：单列、浮动标签、行内验证码按钮。
Item {
    id: page

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
            text: "输入注册邮箱，我们会发一封验证码。"
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
                                           : (bridge.codeBusy ? "发送中…" : "获取验证码")
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
            onTextEdited: bridge.password = text
            onAccepted: bridge.submit()
        }

        Item { width: 1; height: 12 }

        // 提示位固定成一行：成功与失败共用，没内容时不留空洞
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
            text: bridge.done ? "密码已更新" : (bridge.busy ? "正在更新…" : "更新密码")
            busy: bridge.busy || bridge.done
            onClicked: bridge.submit()
        }

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
