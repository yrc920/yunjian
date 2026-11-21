import QtQuick
import QtQuick.Layouts
import XiguChat

// 登录表单通过 bridge 提交，角色只跟随焦点和请求状态。
// 表单按「角色 → 标题 → 邮箱 → 密码 → 记住行 → 按钮 → 入口行」排列。
Item {
    id: page

    LoginBridge {
        id: bridge
        objectName: "pageBridge"
    }

    AuthPage {
        anchors.fill: parent
        enabled: !bridge.busy

        // 折纸角色只在输入区给反馈：聚焦、遮眼与忙碌都不影响表单可用性
        Item {
            width: parent.width
            height: 118

            PaperCompanion {
                anchors.centerIn: parent
                width: 168
                height: 118
                attentive: emailInput.inputFocused
                hiding: passwordInput.inputFocused && !passwordInput.reveal
                revealing: passwordInput.inputFocused && passwordInput.reveal
                busy: bridge.busy
            }
        }

        Item { width: 1; height: 14 }

        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: "登录"
            color: Theme.textPrimary
            font.pixelSize: Theme.fontHero
            font.weight: Font.DemiBold
        }

        Item { width: 1; height: 6 }

        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: "欢迎回来，继续上次的对话"
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSmall
        }

        Item { width: 1; height: 22 }

        AppInput {
            id: emailInput
            width: parent.width
            labeled: true
            label: "邮箱"
            onTextEdited: bridge.email = text
            onAccepted: passwordInput.forceInputFocus()
        }

        Item { width: 1; height: 14 }

        AppInput {
            id: passwordInput
            width: parent.width
            labeled: true
            label: "密码"
            password: true
            reveal: bridge.passwordVisible
            onTextEdited: bridge.password = text
            onAccepted: bridge.login()
            onRevealToggled: bridge.togglePasswordVisible()
        }

        Item { width: 1; height: 12 }

        // 提交级错误：没有错误时不占高度，出现时表单只下移一行
        Text {
            width: parent.width
            text: bridge.errorText
            color: Theme.danger
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.WrapAtWordBoundaryOrAnywhere
            height: text.length > 0 ? Math.max(18, implicitHeight) : 0
            visible: height > 0
            opacity: text.length > 0 ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 120 } }
        }

        Item { width: 1; height: 12 }

        // 记住登录与找回密码同排：一个决定下次是否自动进入，一个是账号自救入口
        Item {
            width: parent.width
            height: 20

            Item {
                id: rememberToggle
                width: 26 + rememberLabel.implicitWidth
                height: 20
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter

                Rectangle {
                    width: 18
                    height: 18
                    radius: 5
                    anchors.verticalCenter: parent.verticalCenter
                    color: bridge.remember ? Theme.primary : Theme.panelBg
                    border.width: 1
                    border.color: bridge.remember ? Theme.primary : Theme.fieldBorder

                    AppIcon {
                        anchors.centerIn: parent
                        name: "check"
                        size: 12
                        color: Theme.primaryText
                        visible: bridge.remember
                    }
                }

                Text {
                    id: rememberLabel
                    x: 26
                    anchors.verticalCenter: parent.verticalCenter
                    text: "自动登录"
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSmall
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: bridge.remember = !bridge.remember
                }
            }

            AppLink {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: "忘记密码"
                onClicked: bridge.go("reset")
            }
        }

        Item { width: 1; height: 18 }

        AppButton {
            width: parent.width
            text: bridge.busy ? bridge.busyText : "登录"
            busy: bridge.busy
            onClicked: bridge.login()
        }

        Item { width: 1; height: 18 }

        RowLayout {
            width: parent.width
            spacing: 6

            Item { Layout.fillWidth: true }

            Text {
                text: "还没有账号？"
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSmall
            }

            AppLink {
                text: "立即注册"
                onClicked: bridge.go("register")
            }

            Item { Layout.fillWidth: true }
        }
    }

    Component.onCompleted: {
        // 有上次的令牌就直接恢复会话，失败时表单接住并把焦点交给密码框
        if (!bridge.tryAutoLogin())
            emailInput.forceInputFocus()
    }

    Connections {
        target: bridge
        function onAutoLoginExpired() { passwordInput.forceInputFocus() }
        // 自动登录会回填邮箱；输入框是命令式更新的，这里同步一次
        function onEmailChanged() {
            if (emailInput.text !== bridge.email)
                emailInput.text = bridge.email
        }
    }
}
