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
        enabled: !(bridge.busy || bridge.codeBusy || bridge.done)

        Text {
            text: "找回密码"
            color: Theme.textPrimary
            font.pixelSize: Theme.fontDisplay
            font.weight: Font.DemiBold
        }

        Item { width: 1; height: 20 }

        AppInput {
            id: userInput
            width: parent.width
            labeled: true
            label: "用户名"
            onTextEdited: bridge.user = text
            onAccepted: emailInput.forceInputFocus()
        }

        Item { width: 1; height: 14 }

        AppInput {
            id: emailInput
            width: parent.width
            labeled: true
            label: "邮箱"
            onTextEdited: bridge.email = text
            onAccepted: codeInput.forceInputFocus()
        }

        Item { width: 1; height: 14 }

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

        Item { width: 1; height: 14 }

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
            height: text.length > 0 ? Math.max(18, implicitHeight) : 0
            visible: height > 0
            opacity: text.length > 0 ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 120 } }
        }

        Item { width: 1; height: 12 }

        AppButton {
            width: parent.width
            text: bridge.done ? "密码已更新" : (bridge.busy ? "正在更新…" : "更新密码")
            busy: bridge.busy || bridge.done
            onClicked: bridge.submit()
        }

        Item { width: 1; height: 14 }

        AppLink {
            width: parent.width
            text: "返回登录"
            horizontalAlignment: Text.AlignHCenter
            onClicked: bridge.go("login")
        }
    }

    Component.onCompleted: userInput.forceInputFocus()
}
