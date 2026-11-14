import QtQuick
import QtCore
import QtQuick.Dialogs
import XiguChat

// 会话页只管理编辑与呈现，提交内容由外层交给聊天服务。
// 结构：标题栏 → 消息列表 → 输入区；列表滚动与光标跟随都在本页处理。
Item {
    id: root

    property string title: ""
    property string subtitle: "" //好友个性签名，空表示没写，标题只显示名字
    property var messages: null
    property var bridge: null
    property int currentUid: 0
    signal sendRequested(var parts)
    signal profileRequested()
    signal mediaViewRequested(var items, int index) //查看器：当前会话的图片与视频列表
    property bool stickBottom: true //用户停在底部时才自动跟随新消息

    readonly property bool hasAttachment: composerBridge.hasAttachment
    readonly property int headerHeight: 78
    //附件先横向排，放不下换行；区域最多两行高，编辑区高度保持不变
    readonly property real attachmentCardWidth: Math.min(250,
        Math.max(170, Math.floor((width - 84) / 3)))
    readonly property int composerHeight: 216
        + (attachmentView.visible ? attachmentView.height : 0)

    property int saveAsMessageId: 0
    property string actionMessage: ""

    ComposerBridge { id: composerBridge }

    // 切换会话时把输入文档也切过去，每个好友保留自己的草稿；新会话先滚到底部
    onCurrentUidChanged: {
        composerBridge.switchConversation(input.textDocument, currentUid)
        root.stickBottom = true
        Qt.callLater(messageList.positionViewAtEnd)
    }
    Component.onCompleted: composerBridge.switchConversation(input.textDocument, currentUid)
    onVisibleChanged: if (visible) input.forceActiveFocus()

    // 输入区、消息气泡与媒体卡片共用一套右键菜单，父项是整页，坐标直接是页面坐标
    ContextMenu { id: contextMenu; anchors.fill: parent }

    Connections {
        target: root.bridge
        function onResourceSaved(ok, path) {
            root.showActionMessage(ok ? "已保存" : "保存失败，请重试")
        }
        function onResourceNotice(message) {
            root.showActionMessage(message)
        }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.chatBg
    }

    // 标题栏：左边是当前会话名，右边「资料」打开好友资料页
    Rectangle {
        id: header
        width: parent.width
        height: root.headerHeight
        color: Theme.chatBg

        // 标题行：显示名 + 个性签名。签名只在好友写过时出现，用更小的字号跟在名字后面
        Item {
            id: titleRow
            anchors.left: parent.left
            anchors.leftMargin: 28
            anchors.right: profileButton.left
            anchors.rightMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            height: 26

            Text {
                id: titleText
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                //有签名时给名字留 62% 的宽度，剩下的交给签名；没有签名就用满整行
                width: Math.min(implicitWidth,
                                root.subtitle.length > 0 ? parent.width * 0.62 : parent.width)
                text: root.title
                textFormat: Text.PlainText
                font.pixelSize: 20
                font.bold: true
                color: Theme.textPrimary
                elide: Text.ElideRight
            }
            Text {
                visible: root.subtitle.length > 0
                anchors.left: titleText.right
                anchors.leftMargin: 12
                anchors.right: parent.right
                anchors.baseline: titleText.baseline
                text: root.subtitle
                textFormat: Text.PlainText
                font.pixelSize: Theme.fontSmall
                color: Theme.textSecondary
                elide: Text.ElideRight
            }
        }

        Rectangle {
            id: profileButton
            anchors.right: parent.right
            anchors.rightMargin: 24
            anchors.verticalCenter: parent.verticalCenter
            width: 72
            height: 34
            radius: 7
            color: profileArea.containsMouse || activeFocus ? Theme.panelAlt : "transparent"
            activeFocusOnTab: true
            Accessible.role: Accessible.Button
            Accessible.name: "查看好友资料"
            Accessible.onPressAction: root.profileRequested()
            Keys.onSpacePressed: root.profileRequested()
            Keys.onReturnPressed: root.profileRequested()
            AppIcon {
                x: 10
                anchors.verticalCenter: parent.verticalCenter
                name: "user"
                size: 17
                color: Theme.textSecondary
            }
            Text {
                x: 34
                anchors.verticalCenter: parent.verticalCenter
                text: "资料"
                font.pixelSize: 12
                color: Theme.textSecondary
            }
            MouseArea {
                id: profileArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.profileRequested()
            }
        }

        Rectangle {
            x: 28
            anchors.bottom: parent.bottom
            width: parent.width - 56
            height: 1
            color: Theme.line
        }
    }

    // 消息列表：模型来自 MessageModel，条目高度由气泡内容决定；
    // 数量或内容高度变化后滚到底，正在翻历史时由模型增量插入避免重建
    ListView {
        id: messageList
        objectName: "messageList"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: header.bottom
        anchors.bottom: composer.top
        anchors.leftMargin: 28
        anchors.rightMargin: 28
        anchors.topMargin: 24
        anchors.bottomMargin: 24
        clip: true
        spacing: 18
        model: root.messages
        boundsBehavior: Flickable.StopAtBounds

        // 每行只创建一种气泡：文本/图片用 ChatBubble，视频/文件用 MediaBubble。
        // 之前两种组件同时存在，隐藏的图片也会解码缩略图，长会话快速滚动时非常卡。
        delegate: Item {
            id: messageRow
            width: ListView.view.width
            height: loader.item ? loader.item.implicitHeight : 0

            Loader {
                id: loader
                width: parent.width
                sourceComponent: model.msgType >= 2 ? mediaComponent : bubbleComponent
            }

            Component {
                id: bubbleComponent
                ChatBubble {
                    width: messageRow.width
                    text: model.text
                    isSelf: model.isSelf
                    isImage: model.isImage
                    avatar: model.avatar
                    name: model.name
                    imageSource: model.imageSource
                    imageWidth: model.imageWidth
                    imageHeight: model.imageHeight
                    status: model.status
                    transferProgress: model.transferProgress
                    onImageClicked: root.openImage(model.messageId, model.imageSource)
                    onContextRequested: (x, y) => root.showMessageMenu(
                        model.messageId, model.msgType, model.text, model.imageSource,
                        mapToItem(root, x, y))
                }
            }

            Component {
                id: mediaComponent
                MediaBubble {
                    width: messageRow.width
                    avatar: model.avatar
                    name: model.name
                    isSelf: model.isSelf
                    msgType: model.msgType
                    status: model.status
                    fileName: model.fileName
                    fileSize: model.fileSize
                    thumbSource: model.imageSource
                    transferState: model.transferState
                    transferProgress: model.transferProgress
                    onActionRequested: (action) => root.handleResourceAction(
                        model.messageId, model.msgId, model.msgType, action, model.fileName)
                    onContextRequested: (x, y) => root.showMediaMenu(
                        model.messageId, model.msgId, model.msgType, model.transferState,
                        model.fileName, mapToItem(root, x, y))
                }
            }
        }

        // 只有停在底部时才跟随新消息；向上翻历史时不再被拉回底部，
        // 图片异步加载导致的高度变化也用短定时器合并，避免连续跳动
        onCountChanged: {
            if (root.stickBottom)
                Qt.callLater(messageList.positionViewAtEnd)
        }
        onContentHeightChanged: {
            if (root.stickBottom && !messageList.moving)
                scrollTimer.restart()
        }
        //离底部 64px 内就视为“在底部”：图片加载让 contentHeight 变高时还能继续跟到底，
        //快速滚动到末尾也不会停在最后一张图上
        onMovementEnded: {
            root.stickBottom = contentY + height >= contentHeight - 64
            if (root.stickBottom)
                scrollTimer.restart() //滚动期间新加载的图片可能把底部又推高了
        }
        onContentYChanged: {
            if (!moving && !flicking)
                root.stickBottom = contentY + height >= contentHeight - 64
        }
    }

    Timer {
        id: scrollTimer
        interval: 40
        onTriggered: if (root.stickBottom) messageList.positionViewAtEnd()
    }

    // 没有消息时只显示一个轻提示，输入区仍然可以正常使用
    Column {
        anchors.centerIn: messageList
        visible: messageList.count === 0
        spacing: 14
        AppIcon {
            anchors.horizontalCenter: parent.horizontalCenter
            name: "chat"
            size: 28
            color: Theme.textHint
        }
        Text {
            text: "还没有消息"
            font.pixelSize: 12
            color: Theme.textHint
        }
    }

    // 输入区：工具与发送在顶部，附件在编辑区上方逐条排列，编辑区保持固定高度。
    // 附件多时只让附件列表变高并自身滚动，不再挤压文字输入空间。
    Rectangle {
        id: composer
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: 24
        anchors.rightMargin: 24
        anchors.bottomMargin: 22
        height: root.composerHeight
        radius: 10
        color: Theme.fieldBg
        border.width: composerDrop.containsDrag ? 2 : 1
        border.color: composerDrop.containsDrag || input.activeFocus ? Theme.primary : Theme.line
        Behavior on border.color { ColorAnimation { duration: 130 } }

        Item {
            id: topBar
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 44

            Row {
                id: tools
                x: 8
                anchors.verticalCenter: parent.verticalCenter
                spacing: 2
                ToolIcon {
                    kind: "emoji"
                    Accessible.name: "表情"
                }
                ToolIcon {
                    kind: "image"
                    usable: true
                    Accessible.name: "插入图片"
                    onClicked: imageDialog.open()
                }
                ToolIcon {
                    kind: "video"
                    usable: true
                    Accessible.name: "发送视频"
                    onClicked: videoDialog.open()
                }
                ToolIcon {
                    kind: "folder"
                    usable: true
                    Accessible.name: "发送文件"
                    onClicked: fileDialog.open()
                }
            }

            Text {
                anchors.left: tools.right
                anchors.leftMargin: 8
                anchors.right: sendButton.left
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                text: composerBridge.errorText
                visible: text.length > 0
                elide: Text.ElideRight
                color: Theme.danger
                font.pixelSize: 11
            }

            AppButton {
                id: sendButton
                anchors.right: parent.right
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                width: 76
                height: 30
                text: "发送"
                iconName: "send"
                enabled: (input.length > 0 || root.hasAttachment) && !input.inputMethodComposing
                onClicked: {
                    root.submit()
                    input.forceActiveFocus()
                }
            }
        }

        // 附件区：先横向排，放不下自动换行；最多显示两行，再多在区域内滚动
        Flickable {
            id: attachmentView
            visible: composerBridge.attachments.length > 0
            x: 12
            y: 45
            width: parent.width - 24
            height: visible ? Math.min(flow.height, 98) : 0
            contentWidth: width
            contentHeight: flow.height
            clip: true
            interactive: contentHeight > height
            boundsBehavior: Flickable.StopAtBounds

            Flow {
                id: flow
                width: attachmentView.width - 10
                spacing: 6

                Repeater {
                    model: composerBridge.attachments

                    delegate: Rectangle {
                        width: root.attachmentCardWidth
                        height: 46
                        radius: 8
                        color: Theme.panelAlt
                        border.width: 1
                        border.color: Theme.line

                        Rectangle {
                            id: attachmentTile
                            x: 7
                            anchors.verticalCenter: parent.verticalCenter
                            width: 30
                            height: 30
                            radius: 6
                            color: Theme.primarySoft

                            AppIcon {
                                anchors.centerIn: parent
                                name: modelData.kind === "video" ? "video" : "file"
                                size: 16
                                color: Theme.primary
                            }
                        }

                        Column {
                            anchors.left: attachmentTile.right
                            anchors.leftMargin: 8
                            anchors.right: attachmentClose.left
                            anchors.rightMargin: 4
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 3

                            Text {
                                width: parent.width
                                text: modelData.name || ""
                                color: Theme.textPrimary
                                font.pixelSize: 12
                                elide: Text.ElideMiddle
                                maximumLineCount: 1
                            }
                            Text {
                                width: parent.width
                                text: (modelData.kind === "video" ? "视频" : "文件") + " · "
                                    + root.formatSize(modelData.size || 0)
                                color: Theme.textHint
                                font.pixelSize: 10
                                elide: Text.ElideRight
                            }
                        }

                        ToolIcon {
                            id: attachmentClose
                            anchors.right: parent.right
                            anchors.rightMargin: 2
                            anchors.verticalCenter: parent.verticalCenter
                            width: 22
                            height: 22
                            kind: "close"
                            usable: true
                            Accessible.name: "移除附件"
                            onClicked: composerBridge.removeAttachment(index)
                        }

                        MouseArea {
                            anchors.fill: parent
                            acceptedButtons: Qt.RightButton
                            onPressed: (mouse) => {
                                const p = mapToItem(root, mouse.x, mouse.y)
                                root.showAttachmentMenu(index, p.x, p.y)
                            }
                        }
                    }
                }
            }
        }

        // 附件区滚动条：只在两行放不下时出现
        Rectangle {
            visible: flow.height > 98
            x: parent.width - 10
            y: attachmentView.y
            width: 4
            height: attachmentView.height
            radius: 2
            color: Theme.panelAlt

            Rectangle {
                width: parent.width
                radius: 2
                color: Theme.textHint
                height: Math.max(22, parent.height * parent.height
                    / Math.max(1, attachmentView.contentHeight))
                y: (parent.height - height) * Math.max(0, Math.min(1,
                    attachmentView.contentY / Math.max(1,
                        attachmentView.contentHeight - attachmentView.height)))
            }
        }

        // 编辑区自己滚动：TextEdit 不提供滚动条，外层 Flickable 只负责视口和滚轮
        Flickable {
            id: inputView
            x: 16
            y: 45 + attachmentView.height
            width: parent.width - 32
            height: parent.height - y - 10
            contentWidth: width
            contentHeight: Math.max(input.contentHeight, height)
            clip: true
            interactive: false
            boundsBehavior: Flickable.StopAtBounds

            WheelHandler {
                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                onWheel: (event) => {
                    const maxY = Math.max(0, inputView.contentHeight - inputView.height)
                    inputView.contentY = Math.min(Math.max(inputView.contentY - event.angleDelta.y, 0), maxY)
                }
            }

            // 回车发送、Shift+回车换行；输入法组合期间不抢键。
            // 粘贴先看剪贴板里有没有本地文件，没有才交给富文本粘贴
            TextEdit {
                id: input
                objectName: "messageInput"
                width: inputView.width
                height: Math.max(contentHeight, inputView.height)
                textFormat: TextEdit.RichText
                selectByMouse: true
                activeFocusOnTab: true
                Accessible.name: "消息输入框"
                wrapMode: TextEdit.WrapAtWordBoundaryOrAnywhere
                font.pixelSize: Theme.fontBody
                color: Theme.textPrimary
                selectionColor: Theme.selected
                selectedTextColor: Theme.textPrimary
                onCursorRectangleChanged: root.followCursor()

                Keys.onPressed: (event) => {
                    if (event.matches(StandardKey.Paste)) {
                        root.pasteFromClipboard()
                        event.accepted = true
                        return
                    }
                    if (inputMethodComposing)
                        return
                    if (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) {
                        const forward = event.key === Qt.Key_Tab && !(event.modifiers & Qt.ShiftModifier)
                        const target = input.nextItemInFocusChain(forward)
                        if (target)
                            target.forceActiveFocus(forward ? Qt.TabFocusReason : Qt.BacktabFocusReason)
                        event.accepted = true
                        return
                    }
                    if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                        if (event.modifiers & Qt.ShiftModifier)
                            return
                        event.accepted = true
                        root.submit()
                    }
                }
            }

            Text {
                visible: input.length === 0 && !input.inputMethodComposing
                text: "写点什么…"
                color: Theme.textHint
                font.pixelSize: Theme.fontBody
            }

            // 只接管右键：左键仍交给 TextEdit 做选择与光标定位
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.RightButton
                onPressed: (mouse) => {
                    const p = input.positionAt(mouse.x, mouse.y)
                    if (p >= 0)
                        input.cursorPosition = p
                    input.forceActiveFocus()
                    const point = mapToItem(root, mouse.x, mouse.y)
                    root.showInputMenu(point.x, point.y)
                }
            }
        }

        // 拖入本地文件：图片插入富文本，视频/文件追加附件；高亮边框提示可放置
        DropArea {
            id: composerDrop
            anchors.fill: parent
            onEntered: (drag) => { drag.accepted = drag.hasUrls }
            onDropped: (drop) => {
                root.acceptFiles(drop.urls)
                drop.acceptProposedAction()
            }
        }
    }

    // 光标移出视口时把编辑区滚到可见位置，留 4px 余量避免贴边
    function followCursor() {
        const rect = input.cursorRectangle
        if (!rect || rect.height <= 0)
            return
        const maxY = Math.max(0, inputView.contentHeight - inputView.height)
        let y = inputView.contentY
        if (rect.y + rect.height > y + inputView.height)
            y = rect.y + rect.height - inputView.height + 4
        else if (rect.y < y)
            y = rect.y - 4
        inputView.contentY = Math.min(Math.max(y, 0), maxY)
    }

    function submit() {
        root.sendRequested(composerBridge.parts(input.textDocument))
    }

    // 仅在外层接受提交后清空，断线时原文仍留在编辑区。
    function clearInput() {
        input.clear()
        inputView.contentY = 0
        composerBridge.clearAttachments()
    }

    // 自己刚发出一条消息：无论之前翻到哪里都跟到底部
    function sent() {
        root.stickBottom = true
        Qt.callLater(messageList.positionViewAtEnd)
    }

    function formatSize(bytes) {
        if (!bytes || bytes <= 0)
            return "0 B"
        if (bytes < 1024)
            return bytes + " B"
        if (bytes < 1024 * 1024)
            return (bytes / 1024).toFixed(1) + " KB"
        if (bytes < 1024 * 1024 * 1024)
            return (bytes / (1024 * 1024)).toFixed(1) + " MB"
        return (bytes / (1024 * 1024 * 1024)).toFixed(2) + " GB"
    }

    // 拖入/粘贴统一入口：图片插到光标处，视频与文件按顺序追加附件
    function acceptFiles(urls) {
        for (const raw of urls) {
            const url = typeof raw === "string" ? raw : raw.toString()
            const kind = composerBridge.fileKind(url)
            if (kind === "image") {
                input.cursorPosition = composerBridge.insertImage(
                    input.textDocument, input.cursorPosition, url)
            }
            else if (kind === "video" || kind === "file") {
                composerBridge.addAttachment(url, kind)
            }
        }
        input.forceActiveFocus()
    }

    // 剪贴板优先处理本地文件；文本和位图仍交给 ComposerBridge.paste
    function pasteFromClipboard() {
        const files = composerBridge.clipboardFiles()
        if (files.length > 0) {
            root.acceptFiles(files.map(item => item.url))
            return
        }
        input.cursorPosition = composerBridge.paste(
            input.textDocument, input.selectionStart, input.selectionEnd)
        input.forceActiveFocus()
    }

    function showInputMenu(x, y) {
        const selected = input.selectedText.length > 0
        contextMenu.open(x, y, [
            { text: "剪切", enabled: selected, action: () => { input.cut(); input.forceActiveFocus() } },
            { text: "复制", enabled: selected, action: () => { input.copy(); input.forceActiveFocus() } },
            { text: "粘贴", action: root.pasteFromClipboard },
            { text: "全选", action: () => { input.selectAll(); input.forceActiveFocus() } },
            { text: "复制全部内容", enabled: input.length > 0, action: () => {
                input.selectAll()
                input.copy()
                input.deselect()
                input.forceActiveFocus()
            } },
            { text: "移除全部附件", enabled: composerBridge.hasAttachment,
              action: () => composerBridge.clearAttachments() }
        ])
    }

    //普通消息：目前提供复制，图片额外支持查看、复制图片、另存为与定位文件
    function showMessageMenu(messageId, msgType, text, imageUrl, point) {
        if (msgType === 1) {
            const local = root.bridge ? root.bridge.resourceUrl(messageId).length > 0 : false
            contextMenu.open(point.x, point.y, [
                { text: "查看大图", action: () => root.openImage(messageId, imageUrl) },
                { text: "复制图片", action: () => {
                    if (!root.bridge.copyImage(messageId, imageUrl))
                        root.showActionMessage("图片还没有下载完成")
                } },
                { text: "另存为", enabled: local,
                  action: () => root.beginImageSaveAs(messageId, imageUrl) },
                { text: "打开文件夹", enabled: local,
                  action: () => root.bridge.revealResource(messageId) }
            ])
            return
        }
        contextMenu.open(point.x, point.y, [
            { text: "复制文字", enabled: text.length > 0,
              action: () => root.bridge.copyText(text) }
        ])
    }

    //视频/文件：按传输状态给出下载、暂停、继续、打开、定位与另存为
    function showMediaMenu(messageId, msgId, msgType, state, fileName, point) {
        const paused = state === 3 || state === 5
        const active = state === 2 || state === 4
        const done = state === 6
        const items = []
        if (done) {
            items.push({ text: msgType === 2 ? "播放" : "打开",
                         action: () => root.handleResourceAction(messageId, msgId, msgType,
                             "open", fileName) })
            items.push({ text: "打开文件夹",
                         action: () => root.bridge.revealResource(messageId) })
        }
        else if (active || paused) {
            items.push({ text: paused ? "继续" : "暂停",
                         action: () => root.bridge.toggleResourceTransfer(messageId, msgId) })
        }
        else {
            items.push({ text: "下载", action: () => root.bridge.downloadResource(messageId) })
        }
        items.push({ text: "另存为", action: () => root.beginSaveAs(messageId, fileName) })
        items.push({ text: "复制文件名", enabled: fileName.length > 0,
                     action: () => root.bridge.copyText(fileName) })
        contextMenu.open(point.x, point.y, items)
    }

    //图片按实际地址猜扩展名，避免把 JPEG 存成 .png
    function beginImageSaveAs(messageId, imageUrl) {
        let suffix = "jpg"
        const text = String(imageUrl || "")
        if (text.indexOf("data:image/png") === 0)
            suffix = "png"
        else if (text.indexOf("data:image/webp") === 0)
            suffix = "webp"
        else {
            const dot = text.lastIndexOf(".")
            const slash = text.lastIndexOf("/")
            if (dot > slash && dot >= 0)
                suffix = text.slice(dot + 1).split("?")[0]
        }
        root.beginSaveAs(messageId, "图片." + suffix)
    }

    function beginSaveAs(messageId, fileName) {
        root.saveAsMessageId = messageId
        const directory = StandardPaths.writableLocation(StandardPaths.DownloadLocation)
        const target = directory + "/" + (fileName && fileName.length > 0 ? fileName : "文件")
        //FileDialog 没有独立的建议文件名属性，用 currentFile 同时给出目录与默认名字
        saveDialog.currentFile = "file://" + encodeURI(target)
        saveDialog.open()
    }

    // 查看器按当前会话顺序打开；消息不在列表里时用气泡缩略图兜底
    function openImage(messageId, fallbackUrl) {
        root.openMedia(messageId, fallbackUrl, 1, "图片")
    }

    function openVideo(messageId, fileName) {
        root.openMedia(messageId, "", 2, fileName)
    }

    function openMedia(messageId, fallbackUrl, type, title) {
        let items = root.bridge ? root.bridge.mediaItems() : []
        let index = -1
        for (let i = 0; i < items.length; ++i) {
            if (items[i].messageId === messageId) {
                index = i
                break
            }
        }
        if (index < 0 && fallbackUrl.length > 0) {
            items = [{ messageId: messageId, type: type, url: fallbackUrl, title: title }]
            index = 0
        }
        if (index < 0 && root.bridge) {
            const url = root.bridge.resourceUrl(messageId)
            if (url.length === 0) {
                root.bridge.markResourceMissing(messageId)
                root.showActionMessage("本地文件已被移动或删除，请重新下载")
                return
            }
            items = [{ messageId: messageId, type: type, url: url, title: title }]
            index = 0
        }
        if (index >= 0)
            root.mediaViewRequested(items, index)
    }

    // 单条附件右键：移除当前或全部
    function showAttachmentMenu(index, x, y) {
        contextMenu.open(x, y, [
            { text: "移除附件", action: () => composerBridge.removeAttachment(index) },
            { text: "移除全部附件", enabled: composerBridge.attachments.length > 1,
              action: () => composerBridge.clearAttachments() }
        ])
    }

    function handleResourceAction(messageId, msgId, msgType, action, fileName) {
        if (!root.bridge || action === "noop")
            return
        if (action === "download") {
            root.bridge.downloadResource(messageId)
            return
        }
        if (action === "toggle" || action === "retryUpload") {
            root.bridge.toggleResourceTransfer(messageId, msgId)
            return
        }
        if (action === "open") {
            if (msgType === 2)
                root.openVideo(messageId, fileName)
            else
                root.bridge.openResource(messageId)
            return
        }
        if (action === "reveal") {
            root.bridge.revealResource(messageId)
            return
        }
        if (action === "saveAs")
            root.beginSaveAs(messageId, fileName)
    }

    function showActionMessage(text) {
        root.actionMessage = text
        toastTimer.restart()
    }

    FileDialog {
        id: imageDialog
        title: "插入图片"
        fileMode: FileDialog.OpenFile
        nameFilters: ["图片 (*.png *.jpg *.jpeg *.bmp *.webp)"]
        onAccepted: input.cursorPosition = composerBridge.insertImage(
            input.textDocument, input.cursorPosition, selectedFile)
    }

    FileDialog {
        id: videoDialog
        title: "发送视频"
        fileMode: FileDialog.OpenFile
        nameFilters: ["视频 (*.mp4 *.mov *.mkv *.avi *.webm)", "所有文件 (*)"]
        onAccepted: composerBridge.addAttachment(selectedFile, "video")
    }

    FileDialog {
        id: fileDialog
        title: "发送文件"
        fileMode: FileDialog.OpenFile
        nameFilters: ["所有文件 (*)"]
        onAccepted: composerBridge.addAttachment(selectedFile, "file")
    }

    FileDialog {
        id: saveDialog
        title: "另存为"
        fileMode: FileDialog.SaveFile
        onAccepted: {
            const result = root.bridge.saveResourceAs(root.saveAsMessageId, selectedFile)
            if (result === 1)
                root.showActionMessage("已保存")
            else if (result === 2)
                root.showActionMessage("开始下载，完成后自动保存")
            else
                root.showActionMessage("保存失败，请检查目标目录")
        }
    }

    Timer {
        id: toastTimer
        interval: 2600
        onTriggered: root.actionMessage = ""
    }

    Rectangle {
        visible: root.actionMessage.length > 0
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: composer.top
        anchors.bottomMargin: 14
        width: toastLabel.width + 28
        height: 30
        radius: 8
        color: Theme.panelAlt
        border.width: 1
        border.color: Theme.line

        Text {
            id: toastLabel
            anchors.centerIn: parent
            text: root.actionMessage
            color: Theme.textSecondary
            font.pixelSize: 11
        }
    }
}
