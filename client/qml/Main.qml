import QtQuick
import QtCore
import QtQuick.Window
import XiguChat

// 窗口负责页面装载、窗口尺寸和会话中断提示。
// 认证页固定窄窗，聊天页给最小尺寸并尽量保留用户已拉大的窗口。
Window {
    id: window
    visible: true
    title: "XiguChat"
    color: Theme.bgTop

    //通知点击后要回到哪条会话；关闭到托盘时用 quitting 区分"隐藏"和"真退出"
    property int notifyThread: 0
    property int notifyKind: 1
    property int notifyPeer: 0
    property bool quitting: false
    //上一个页面类型：登录→聊天、聊天→登录时用来判断是否需要重新居中
    property string lastPage: ""

    // 聊天窗口的尺寸与位置记在本机；认证页尺寸固定，不参与记忆
    Settings {
        id: windowPrefs
        category: "Window"
        property int chatWidth: 1060
        property int chatHeight: 740
        property int chatX: -1
        property int chatY: -1
    }
    Timer {
        id: windowSaver
        interval: 400
        onTriggered: window.saveGeometry()
    }
    function saveGeometry() {
        if (shell.page !== "chat" || !visible)
            return
        windowPrefs.chatWidth = width
        windowPrefs.chatHeight = height
        windowPrefs.chatX = x
        windowPrefs.chatY = y
    }
    onWidthChanged: if (shell.page === "chat" && visible) windowSaver.restart()
    onHeightChanged: if (shell.page === "chat" && visible) windowSaver.restart()
    onXChanged: if (shell.page === "chat" && visible) windowSaver.restart()
    onYChanged: if (shell.page === "chat" && visible) windowSaver.restart()

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
        const areaX = Screen.availableX || 0
        const areaY = Screen.availableY || 0
        const areaW = Screen.availableWidth > 0 ? Screen.availableWidth : Screen.width
        const areaH = Screen.availableHeight > 0 ? Screen.availableHeight : Screen.height
        const limitW = Math.max(420, areaW - 16)
        const limitH = Math.max(480, areaH - 16)
        const enteringChat = name === "chat" && lastPage !== "chat"
        if (name === "chat") {
            minimumWidth = Math.min(900, limitW)
            minimumHeight = Math.min(620, limitH)
            // 尺寸取上次退出时的值，并夹在最小尺寸与屏幕可用区之间
            width = Math.min(Math.max(windowPrefs.chatWidth, minimumWidth), limitW)
            height = Math.min(Math.max(windowPrefs.chatHeight, minimumHeight), limitH)
            //从认证页进入聊天时恢复上次位置，没有合适的位置就居中；
            //已经是聊天页时保留用户自己拉过的位置，但不能跑到屏幕外
            if (enteringChat) {
                const sx = windowPrefs.chatX
                const sy = windowPrefs.chatY
                const restored = sx >= areaX && sy >= areaY
                    && sx + width <= areaX + areaW && sy + height <= areaY + areaH
                x = restored ? sx : areaX + Math.max(8, (areaW - width) / 2)
                y = restored ? sy : areaY + Math.max(8, (areaH - height) / 2)
            } else {
                if (x < areaX || x + width > areaX + areaW)
                    x = areaX + Math.max(8, (areaW - width) / 2)
                if (y < areaY || y + height > areaY + areaH)
                    y = areaY + Math.max(8, (areaH - height) / 2)
            }
        } else {
            // 认证页高度按内容区分：登录 580、注册 664、找回 620；超出屏幕时由 AuthPage 内部滚动
            const h = name === "register" ? 664 : name === "reset" ? 620 : 580
            const pageH = Math.min(h, limitH)
            minimumWidth = 420
            minimumHeight = pageH
            maximumWidth = 420
            maximumHeight = pageH
            width = 420
            height = pageH
            x = areaX + Math.max(0, (areaW - width) / 2)
            y = areaY + Math.max(0, (areaH - height) / 2)
        }
        lastPage = name
        loader.setSource(pageFile(name))
    }
    // 系统外观由平台上报，Unknown 视为浅色；是否采纳由 Theme 的跟随开关决定
    function syncSystemScheme() {
        Theme.systemDark = Qt.styleHints.colorScheme === Qt.ColorScheme.Dark
    }
    Connections {
        target: Qt.styleHints
        function onColorSchemeChanged() { window.syncSystemScheme() }
    }

    Component.onCompleted: {
        syncSystemScheme()
        showPage(shell.page)
        Qt.callLater(syncChrome)
    }

    // 标题栏与界面外观保持一致：macOS 隐藏标题文字，并在深色下把标题栏一起变深
    function syncChrome() {
        WindowChrome.apply(window, Theme.dark)
    }
    Connections {
        target: Theme
        function onDarkChanged() { window.syncChrome() }
    }
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
        id: appearanceButton
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
        Accessible.name: Theme.dark ? "切换到浅色" : "切换到深色"
        Keys.onReturnPressed: Theme.toggleMode()
        Keys.onSpacePressed: Theme.toggleMode()
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
            // 一键切换：点了就换，跟随系统的三选一留在设置页
            onClicked: Theme.toggleMode()
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
