import QtQuick
import QtQuick.Layouts
import XiguChat

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
        enabled: !(bridge.busy || bridge.codeBusy || bridge.done)

        Item {
            width: parent.width
            height: 90

            PaperCompanion {
                anchors.centerIn: parent
                width: 128
                height: 90
                attentive: userInput.inputFocused || emailInput.inputFocused || codeInput.inputFocused
                hiding: (passwordInput.inputFocused && !passwordInput.reveal)
                         || (confirmInput.inputFocused && !confirmInput.reveal)
                revealing: (passwordInput.inputFocused && passwordInput.reveal)
                            || (confirmInput.inputFocused && confirmInput.reveal)
                busy: bridge.busy || bridge.codeBusy
            }
        }

        Item { width: 1; height: 18 }

        Text {
            text: "创建账号"
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
            label: "密码"
            placeholder: "6–15 位"
            password: true
            onRevealToggled: reveal = !reveal
            onTextEdited: bridge.password = text
            onAccepted: confirmInput.forceInputFocus()
        }

        Item { width: 1; height: 14 }

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

        Item { width: 1; height: 12 }

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
            text: bridge.done ? "账号已创建" : (bridge.busy ? "正在创建…" : "创建账号")
            busy: bridge.busy || bridge.done
            onClicked: bridge.submit()
        }

        Item { width: 1; height: 16 }

        AppLink {
            width: parent.width
            text: "已有账号？登录"
            horizontalAlignment: Text.AlignHCenter
            onClicked: bridge.go("login")
        }
    }

    Component.onCompleted: userInput.forceInputFocus()
}
