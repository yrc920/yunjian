import QtQuick
import XiguChat

// 窗口负责页面装载、窗口尺寸和会话中断提示。
// 认证页固定窄窗，聊天页给最小尺寸并尽量保留用户已拉大的窗口。
Window {
    id: window
    visible: true
    title: "xiguchat"
    color: Theme.bgTop

    //通知点击后要回到哪条会话；关闭到托盘时用 quitting 区分"隐藏"和"真退出"
    property int notifyThread: 0
    property int notifyKind: 1
    property int notifyPeer: 0
    property bool quitting: false

    onClosing: (close) => {
        if (!quitting && tray.available && shell.page === "chat") {
            close.accepted = false
            window.hide()
        }
    }

    ShellBridge { id: shell }

    //系统托盘：QtWidgets 只作为平台集成层（QSystemTrayIcon/QMenu），
    //QML/Quick 仍是唯一 UI 层，不创建任何 QWidget 页面
    TrayController {
        id: tray
        tooltip: shell.unreadTotal > 0
            ? "xiguchat · " + shell.unreadTotal + " 条未读" : "xiguchat"

        onOpenRequested: {
            window.show()
            window.raise()
            window.requestActivate()
        }
        onQuitRequested: {
            window.quitting = true
            tray.hide()
            Qt.quit()
        }
        onMessageClicked: {
            window.show()
            window.raise()
            window.requestActivate()
            if (loader.item && loader.item.chatBridge && notifyThread > 0)
                loader.item.chatBridge.openThread(notifyThread, notifyKind, notifyPeer)
        }
    }

    //新消息只在窗口不在前台时弹系统通知；点击后回到对应会话
    Connections {
        target: shell
        function onMessageArrived(title, body, threadId, threadType, peerUid) {
            notifyThread = threadId
            notifyKind = threadType
            notifyPeer = peerUid
            if (!window.active && tray.available)
                tray.showMessage(title, body, 6000)
        }
    }

    //关闭到托盘后，从 Dock/任务栏重新激活要能把窗口带回来
    Connections {
        target: Qt.application
        function onStateChanged() {
            if (Qt.application.state === Qt.ApplicationActive && !window.visible)
                window.show()
        }
    }

    function pageFile(name) {
        if (name === "chat") return "ChatWindow.qml"
        if (name === "register") return "Register.qml"
        if (name === "reset") return "Reset.qml"
        return "Login.qml"
    }
    function showPage(name) {
        // 切页前先解除上一页的固定尺寸，否则聊天页会被认证页的 420 宽卡住
        maximumWidth = 16777215
        maximumHeight = 16777215
        if (name === "chat") {
            minimumWidth = 960
            minimumHeight = 680
            width = Math.max(width, 1060)
            height = Math.max(height, 740)
        } else {
            // 认证页高度按内容区分：登录 560、注册 740、重置 680
            const h = name === "register" ? 740 : name === "reset" ? 680 : 560
            minimumWidth = 420
            minimumHeight = h
            maximumWidth = 420
            maximumHeight = h
            width = 420
            height = h
        }
        loader.setSource(pageFile(name))
    }
    Component.onCompleted: showPage(shell.page)
    Connections {
        target: shell
        function onPageChanged() { window.showPage(shell.page) }
    }

    //普通断线时先给一条非阻塞的重连横条：缓存消息还能看，不强行弹回登录页
    Rectangle {
        id: reconnectBar
        anchors.top: parent.top
        width: parent.width
        height: visible ? 32 : 0
        visible: shell.reconnecting
        color: Theme.panelAlt
        z: 50
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: Theme.line
        }
        Row {
            anchors.centerIn: parent
            spacing: 8
            AppIcon {
                name: "reset"
                size: 14
                color: Theme.accent
            }
            Text {
                text: shell.reconnectText
                color: Theme.accent
                font.pixelSize: 12
                elide: Text.ElideRight
            }
        }
    }

    Loader {
        id: loader
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.top: reconnectBar.visible ? reconnectBar.bottom : parent.top
        enabled: shell.notice.length === 0
    }
    Rectangle {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 16
        width: 36
        height: 36
        radius: 18
        visible: shell.page !== "chat"
        enabled: shell.notice.length === 0
        color: modeArea.containsMouse || activeFocus ? Theme.panelAlt : "transparent"
        activeFocusOnTab: visible
        Accessible.role: Accessible.Button
        Accessible.name: Theme.dark ? "切换浅色外观" : "切换深色外观"
        Keys.onReturnPressed: Theme.toggle()
        Keys.onSpacePressed: Theme.toggle()
        AppIcon {
            anchors.centerIn: parent
            name: Theme.dark ? "sun" : "moon"
            color: Theme.ink
            size: 19
        }
        MouseArea {
            id: modeArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: Theme.toggle()
        }
    }

    // 掉线提示覆盖整个窗口，点「返回登录」才真正切页
    FocusScope {
        anchors.fill: parent
        visible: shell.notice.length > 0
        focus: visible
        z: 100
        Rectangle {
            anchors.fill: parent
            color: Theme.overlay
        }
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onWheel: (wheel) => { wheel.accepted = true }
        }
        Rectangle {
            anchors.centerIn: parent
            width: 352
            height: content.height + 56
            color: Theme.panelBg
            radius: 12
            border.color: Theme.line
            MouseArea { anchors.fill: parent }
            Column {
                id: content
                x: 28
                y: 28
                width: parent.width - 56
                spacing: 16
                AppIcon {
                    name: "mail"
                    size: 28
                    color: Theme.stamp
                }
                Text {
                    text: "连接中断了"
                    font.pixelSize: 22
                    font.weight: Font.DemiBold
                    color: Theme.textPrimary
                }
                Text {
                    width: parent.width
                    text: shell.notice
                    font.pixelSize: 14
                    color: Theme.textSecondary
                    wrapMode: Text.WordWrap
                }
                AppButton {
                    width: parent.width
                    text: "返回登录"
                    iconName: "arrowRight"
                    onClicked: shell.dismissNotice()
                }
            }
        }
    }
}
