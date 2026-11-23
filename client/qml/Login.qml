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
            height: 98

            PaperCompanion {
                anchors.centerIn: parent
                width: 140
                height: 98
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
            text: "欢迎回来，接着说上次的话。"
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSmall
        }

        // 注册/改密成功后的回执：固定占一行，没有回执时也不跳版
        Item {
            width: parent.width
            // 没有回执时只留一段空隙；有回执才展开，表单不会平白多出一大块
            height: AuthDraft.notice.length > 0 ? 38 : 14
            Behavior on height { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }

            Text {
                width: parent.width
                anchors.top: parent.top
                anchors.topMargin: 12
                horizontalAlignment: Text.AlignHCenter
                visible: AuthDraft.notice.length > 0
                text: AuthDraft.notice
                textFormat: Text.PlainText
                color: Theme.success
                font.pixelSize: Theme.fontCaption
                elide: Text.ElideRight
            }
        }

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
            onAccepted: passwordInput.forceInputFocus()
        }

        Item { width: 1; height: 12 }

        AppInput {
            id: passwordInput
            width: parent.width
            labeled: true
            label: "密码"
            password: true
            reveal: bridge.passwordVisible
            onTextEdited: {
                bridge.password = text
                AuthDraft.password = text
                AuthDraft.clearNotice()
            }
            onAccepted: {
                AuthDraft.clearNotice()
                bridge.login()
            }
            onRevealToggled: bridge.togglePasswordVisible()
        }

        Item { width: 1; height: 18 }

        // 记住登录与找回密码同排：一个决定下次是否自动进入，一个是账号自救入口
        Item {
            width: parent.width
            height: 28

            Item {
                id: rememberToggle
                width: 26 + rememberLabel.implicitWidth
                height: 28
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter

                Rectangle {
                    width: 18
                    height: 18
                    radius: 5
                    anchors.verticalCenter: parent.verticalCenter
                    color: bridge.remember ? Theme.primary : Theme.panelBg
                    border.width: 1
                    border.color: rememberToggle.activeFocus ? Theme.focusRing : bridge.remember ? Theme.primary : Theme.fieldBorder

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
                    onClicked: {
                        bridge.remember = !bridge.remember
                        AuthDraft.remember = bridge.remember
                        AuthDraft.rememberTouched = true
                    }
                }
            }

            AppLink {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: "忘记密码"
                onClicked: bridge.go("reset")
            }
        }

        Item { width: 1; height: 4 }

        // 提交级错误放在按钮上方：和输入区留出距离，又不会把“自动登录”推远
        Text {
            width: parent.width
            text: bridge.errorText
            color: Theme.danger
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.WrapAtWordBoundaryOrAnywhere
            // 固定预留一行：提示出现时按钮不上下跳，长文案仍可自然增高
            height: Math.max(18, implicitHeight)
            opacity: text.length > 0 ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 120 } }
        }

        Item { width: 1; height: 8 }

        AppButton {
            width: parent.width
            text: bridge.busy ? bridge.busyText : "登录"
            busy: bridge.busy
            onClicked: {
                AuthDraft.clearNotice()
                bridge.login()
            }
        }

        Item { width: 1; height: 12 }

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

    // 登录到一半去注册/找回再回来：邮箱、密码、自动登录选项都恢复
    function restoreDraft() {
        // 注册/改密成功回执带的邮箱优先于上次登录记录：返回登录后直接看到刚注册的邮箱
        if (AuthDraft.email.length > 0) {
            bridge.email = AuthDraft.email
            emailInput.text = AuthDraft.email
        }
        if (AuthDraft.password.length > 0 && bridge.password.length === 0) {
            bridge.password = AuthDraft.password
            passwordInput.text = AuthDraft.password
        }
        if (!AuthDraft.rememberTouched)
            AuthDraft.remember = bridge.remember
        bridge.remember = AuthDraft.remember
        // 邮箱可能由上次登录记录预填：同步进草稿，注册/找回页才能接着用
        if (AuthDraft.email.length === 0 && bridge.email.length > 0)
            AuthDraft.email = bridge.email
    }

    Component.onCompleted: {
        // 注册/改密刚成功时先不自动登录：回执要落到新账号的邮箱上
        const hasReceipt = AuthDraft.notice.length > 0
        // 有上次的令牌就直接恢复会话；失败时把草稿放回来，并聚焦到该输入的位置
        if (hasReceipt || !bridge.tryAutoLogin()) {
            restoreDraft()
            if (emailInput.text.length > 0)
                passwordInput.forceInputFocus()
            else
                emailInput.forceInputFocus()
        }
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
