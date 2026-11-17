import QtQuick
import QtCore
import QtQuick.Controls
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
    property double currentThreadId: 0 //thread_id 是 64 位，QML 用 double 承接
    //会话键：两个刚加的好友都还没有 thread_id 时，也要能区分切换
    readonly property string conversationKey: currentKind + ":" + currentThreadId + ":" + currentUid
    property int currentKind: 1 //1 单聊 / 2 群聊
    property var groupInfo: null
    property var mentionCandidates: []
    readonly property bool isGroup: currentKind === 2
    //已离开的群只剩历史：不再提供资料入口，也不展示公告
    readonly property int groupLeftState: isGroup && groupInfo ? (groupInfo.leftState || 0) : 0
    //被移出或解散后输入区整体关闭，避免"还能打字但发不出去"的假交互
    readonly property bool inputLocked: isGroup && groupLeftState > 0
    readonly property bool hasNotice: isGroup && groupLeftState === 0 && groupInfo
        && String(groupInfo.notice || "").length > 0
    signal sendRequested(var parts)
    signal profileRequested()
    signal groupInfoRequested()
    signal mediaViewRequested(var items, int index) //查看器：当前会话的图片与视频列表
    //单聊关系提示（已删除对方 / 被对方删除 / 验证已发送），空串表示正常
    property string blockedText: ""
    //表情面板：本地内置资源，展开后停在输入框上方
    property bool emojiOpen: false
    //发送朋友验证的小弹层：留在当前聊天里完成，不切到联系人区域
    property bool reAddOpen: false
    property string reAddDraft: ""
    property bool stickBottom: true //用户停在底部时才自动跟随新消息
    //停在底部之外时新到的消息条数：底部浮签用它提示，不把用户强行拉回末尾
    property int pendingNew: 0
    property int lastMessageCount: 0
    // 上滑加载更早消息：插入头部会让内容高度突然变大，这段时间按差值补偿 contentY，
    // 把用户正在看的那一条按在原地，而不是被新插入的内容顶下去
    property bool prepending: false
    property real prependHeight: 0
    // 刚切进会话的一小段时间：模型在重建、图片在加载，contentY 会被反复改写。
    // 这段时间内不改写 stickBottom，并持续贴底，避免"打开会话停在第一条"。
    property bool settling: false
    //搜索结果跳转期间禁止自动贴底与内容高度变化触发的回尾逻辑
    property bool jumping: false
    property int jumpRow: -1

    readonly property bool hasAttachment: composerBridge.hasAttachment
    readonly property int headerHeight: 78
    //附件先横向排，放不下换行；区域最多两行高，编辑区高度保持不变
    readonly property real attachmentCardWidth: Math.min(250,
        Math.max(170, Math.floor((width - 84) / 3)))
    readonly property int composerHeight: 216
        + (attachmentView.visible ? attachmentView.height : 0)

    property int saveAsMessageId: 0
    property string actionMessage: ""
    //消息搜索面板与搜索命中的一次性高亮
    property bool messageSearchOpen: false
    property double highlightMessageId: 0

    //@ 成员选择：anchor 是当前 "@" 在文档里的位置，用户继续输入时当作过滤词
    property int mentionAnchor: -1
    property string mentionQuery: ""
    property bool mentionOpen: false
    readonly property var filteredMentions: {
        const all = root.mentionCandidates || []
        const q = root.mentionQuery.toLowerCase()
        if (q.length === 0)
            return all
        return all.filter(function(member) {
            return String(member.name).toLowerCase().indexOf(q) >= 0
        })
    }

    ComposerBridge { id: composerBridge }

    // 切换会话时把输入文档也切过去，每个会话保留自己的草稿；新会话先滚到底部。
    // 草稿按 thread_id 存，群聊与单聊用同一套键，不会互相串
    onConversationKeyChanged: {
        composerBridge.switchConversation(input.textDocument, currentThreadId)
        messageSearchOpen = false
        closeMentionPicker()
        root.emojiOpen = false //换会话时收起面板，避免停在新会话上
        root.stickBottom = true
        root.pendingNew = 0
        root.settling = true
        root.jumping = false
        root.jumpRow = -1
        jumpTimer.stop()
        root.prepending = false
        root.prependHeight = 0
        settleTimer.restart()
        root.lastMessageCount = root.messages ? root.messages.count : 0
        if (root.bridge)
            root.bridge.setChatAtBottom(true)
        Qt.callLater(messageList.positionViewAtEnd)
    }
    Component.onCompleted: composerBridge.switchConversation(input.textDocument, currentThreadId)
    onVisibleChanged: if (visible && !inputLocked) input.forceActiveFocus()
    onInputLockedChanged: {
        if (inputLocked) {
            root.emojiOpen = false
            input.focus = false
        }
    }
    //停/离底部决定当前会话算不算"正在看"：只有贴底时才允许清未读
    onStickBottomChanged: {
        if (root.bridge)
            root.bridge.setChatAtBottom(stickBottom)
        if (stickBottom)
            root.pendingNew = 0
    }

    // 上滑到顶时向桥接要一页更早的消息：库里没有更早的会立即返回，代价是一次空查询
    function loadOlder() {
        if (!root.bridge)
            return
        if (root.prepending || root.settling || root.currentThreadId <= 0)
            return
        root.prepending = true
        root.prependHeight = messageList.contentHeight
        root.bridge.loadOlderMessages()
        prependTimer.restart()
    }

    // 输入区、消息气泡与媒体卡片共用一套右键菜单，父项是整页，坐标直接是页面坐标
    ContextMenu { id: contextMenu; anchors.fill: parent }

    //消息搜索覆盖整页；点结果会切会话并回到原消息
    MessageSearchPanel {
        id: messageSearchPanel
        controller: root.bridge
        open: root.messageSearchOpen
        currentThreadId: root.currentThreadId
        currentKind: root.currentKind
        onCloseRequested: root.messageSearchOpen = false
    }

    //Ctrl/Cmd+F 打开消息搜索
    Shortcut {
        sequence: StandardKey.Find
        onActivated: root.messageSearchOpen = true
    }

    //搜索结果定位：先把目标行滚到中间，再短暂描边；找不到说明上下文没装载成功
    function scrollToMessage(id) {
        if (!root.bridge)
            return
        const row = root.bridge.messageRow(id)
        if (row < 0) {
            root.bridge.clearScrollToMessage()
            return
        }
        root.settling = false
        root.stickBottom = false
        root.pendingNew = 0
        root.jumping = true
        root.jumpRow = row
        messageList.positionViewAtIndex(row, ListView.Center)
        jumpTimer.restart()
        root.highlightMessageId = id
        highlightTimer.restart()
        root.bridge.clearScrollToMessage()
    }
    Connections {
        target: root.bridge
        function onScrollToMessageChanged() {
            const id = root.bridge ? root.bridge.scrollToMessageId : 0
            if (id > 0)
                Qt.callLater(function() { root.scrollToMessage(id) })
        }
    }
    Timer {
        id: highlightTimer
        interval: 2500
        onTriggered: root.highlightMessageId = 0
    }
    //等列表完成一次布局后再定位一次，随后恢复正常滚动判定
    Timer {
        id: jumpTimer
        interval: 700
        onTriggered: {
            if (root.jumpRow >= 0)
                messageList.positionViewAtIndex(root.jumpRow, ListView.Center)
            root.jumping = false
            root.jumpRow = -1
        }
    }

    Connections {
        target: root.bridge
        function onResourceSaved(ok, path) {
            root.showActionMessage(ok ? "已保存" : "保存失败，请重试")
        }
        function onResourceNotice(message) {
            root.showActionMessage(message)
        }
        // 服务端判定"已经不是好友"时给一句明确提示，弹完即清，避免重复
        function onChatNoticeChanged() {
            if (!root.bridge || root.bridge.chatNotice.length === 0)
                return
            root.showActionMessage(root.bridge.chatNotice)
            root.bridge.clearChatNotice()
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

        // 标题行：显示名 + 个性签名/成员数。群聊里点标题直接进群资料
        Item {
            id: titleRow
            anchors.left: parent.left
            anchors.leftMargin: 28
            anchors.right: messageSearchButton.left
            anchors.rightMargin: 12
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
            MouseArea {
                anchors.fill: parent
                enabled: root.isGroup && root.groupLeftState === 0
                cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: root.groupInfoRequested()
            }
        }

        // 搜索本机消息：入口放在资料按钮左侧，键盘也可用 Ctrl/Cmd+F
        Rectangle {
            id: messageSearchButton
            anchors.right: profileButton.left
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            width: 34
            height: 34
            radius: 7
            color: searchButtonArea.containsMouse || activeFocus ? Theme.panelAlt : "transparent"
            activeFocusOnTab: true
            Accessible.role: Accessible.Button
            Accessible.name: "搜索消息"
            Keys.onReturnPressed: root.messageSearchOpen = true
            Keys.onSpacePressed: root.messageSearchOpen = true
            AppIcon {
                anchors.centerIn: parent
                name: "search"
                size: 17
                color: Theme.textSecondary
            }
            MouseArea {
                id: searchButtonArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.messageSearchOpen = true
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
            visible: !(root.isGroup && root.groupLeftState > 0)
            color: profileArea.containsMouse || activeFocus ? Theme.panelAlt : "transparent"
            activeFocusOnTab: true
            Accessible.role: Accessible.Button
            Accessible.name: root.isGroup ? "查看群资料" : "查看好友资料"
            Accessible.onPressAction: root.isGroup ? root.groupInfoRequested()
                                                   : root.profileRequested()
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
                text: root.isGroup ? "群资料" : "资料"
                font.pixelSize: 12
                color: Theme.textSecondary
            }
            MouseArea {
                id: profileArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.isGroup ? root.groupInfoRequested() : root.profileRequested()
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

    // 群公告：有内容时占一条，点一下进群资料；没有公告不占位
    Rectangle {
        id: groupNoticeBar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: header.bottom
        height: root.hasNotice ? 32 : 0
        visible: height > 0
        color: Theme.panelAlt

        Text {
            anchors.left: parent.left
            anchors.leftMargin: 28
            anchors.right: parent.right
            anchors.rightMargin: 28
            anchors.verticalCenter: parent.verticalCenter
            text: root.hasNotice ? "群公告：" + root.groupInfo.notice : ""
            textFormat: Text.PlainText
            color: Theme.textSecondary
            font.pixelSize: 11
            elide: Text.ElideRight
        }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: root.groupInfoRequested()
        }
    }

    // 关系提示条：比公告更靠下，说明"为什么发不出去"并给出补加好友的入口
    Rectangle {
        id: blockedBar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: root.hasNotice ? groupNoticeBar.bottom : header.bottom
        height: root.blockedText.length > 0 ? 38 : 0
        visible: height > 0
        color: Theme.panelAlt

        Text {
            anchors.left: parent.left
            anchors.leftMargin: 28
            anchors.right: blockedAction.visible ? blockedAction.left : parent.right
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            text: root.blockedText
            textFormat: Text.PlainText
            color: Theme.danger
            font.pixelSize: 11
            elide: Text.ElideRight
        }
        Rectangle {
            id: blockedAction
            //只有单聊关系断了才需要"发送朋友验证"；群被移出/解散时这个按钮没有意义
            visible: !root.isGroup
            anchors.right: parent.right
            anchors.rightMargin: 24
            anchors.verticalCenter: parent.verticalCenter
            width: blockedActionText.implicitWidth + 22
            height: 24
            radius: 12
            color: blockedActionArea.containsMouse ? Theme.selected : "transparent"
            border.width: 1
            border.color: Theme.danger

            Text {
                id: blockedActionText
                anchors.centerIn: parent
                text: "发送朋友验证"
                color: Theme.danger
                font.pixelSize: 11
            }
            MouseArea {
                id: blockedActionArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                enabled: root.blockedText.indexOf("已发送") < 0
                onClicked: {
                    root.reAddDraft = "你好，我是" + (root.bridge ? root.bridge.myName : "")
                    root.reAddOpen = true
                }
            }
        }
    }

    // 发送朋友验证：轻量遮罩，不切区域、不打断聊天上下文
    Rectangle {
        id: reAddOverlay
        anchors.fill: parent
        visible: root.reAddOpen
        focus: visible
        color: Theme.overlay
        z: 60
        Keys.onEscapePressed: root.reAddOpen = false
        onVisibleChanged: if (visible) reAddInput.forceInputFocus()

        MouseArea {
            anchors.fill: parent
            onClicked: root.reAddOpen = false
        }

        Rectangle {
            anchors.centerIn: parent
            width: Math.min(380, parent.width - 48)
            implicitHeight: reAddColumn.implicitHeight + 40
            radius: 12
            color: Theme.panelBg
            border.width: 1
            border.color: Theme.line

            Column {
                id: reAddColumn
                x: 20
                y: 20
                width: parent.width - 40
                spacing: 10

                Text {
                    text: "发送朋友验证"
                    color: Theme.textPrimary
                    font.pixelSize: 15
                    font.bold: true
                }
                Text {
                    width: parent.width
                    text: "验证通过后即可继续聊天"
                    color: Theme.textSecondary
                    font.pixelSize: 11
                    wrapMode: Text.Wrap
                }
                AppInput {
                    id: reAddInput
                    width: parent.width
                    maximumLength: 60
                    text: root.reAddDraft
                    placeholder: "说点什么…"
                    onTextEdited: root.reAddDraft = text
                    onAccepted: if (root.reAddDraft.trim().length > 0) {
                        root.bridge.sendReAdd(root.reAddDraft.trim())
                        root.reAddOpen = false
                    }
                }
                Row {
                    spacing: 10
                    anchors.right: parent.right
                    AppButton {
                        width: 80
                        height: 32
                        text: "取消"
                        onClicked: root.reAddOpen = false
                    }
                    AppButton {
                        width: 92
                        height: 32
                        text: "发送"
                        enabled: root.reAddDraft.trim().length > 0
                        onClicked: {
                            root.bridge.sendReAdd(root.reAddDraft.trim())
                            root.reAddOpen = false
                        }
                    }
                }
            }
        }
    }

    // 消息列表：模型来自 MessageModel，条目高度由气泡内容决定；
    // 数量或内容高度变化后滚到底，正在翻历史时由模型增量插入避免重建
    ListView {
        id: messageList
        objectName: "messageList"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: root.blockedText.length > 0 ? blockedBar.bottom
                   : (root.hasNotice ? groupNoticeBar.bottom : header.bottom)
        anchors.bottom: root.inputLocked ? readOnlyBar.top : composer.top
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
            height: messageContent.height

            Column {
                id: messageContent
                width: parent.width
                spacing: 0

                // 时间条：与上一条间隔超过 5 分钟才出现，居中一行浅色小字
                Item {
                    width: parent.width
                    height: model.timeText.length > 0 ? 30 : 0
                    visible: height > 0
                    Text {
                        anchors.centerIn: parent
                        width: parent.width - 40
                        text: model.timeText
                        textFormat: Text.PlainText
                        color: Theme.textHint
                        font.pixelSize: 10
                        horizontalAlignment: Text.AlignHCenter
                        elide: Text.ElideRight
                    }
                }

                // 进入会话时的未读起点：一条细线加一枚小标签，位置固定在这条消息上，
                // 后续新消息不会把它推走
                Item {
                    readonly property bool showDivider: root.bridge && model.messageId > 0
                        && model.messageId === root.bridge.entryDividerId
                    width: parent.width
                    height: showDivider ? 34 : 0
                    visible: showDivider

                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width
                        height: 1
                        color: Theme.line
                    }
                    Rectangle {
                        anchors.centerIn: parent
                        width: dividerLabel.implicitWidth + 22
                        height: 20
                        radius: 10
                        color: Theme.panelBg
                        border.width: 1
                        border.color: Theme.accent
                        Text {
                            id: dividerLabel
                            anchors.centerIn: parent
                            text: "以下 " + (root.bridge ? root.bridge.entryUnread : 0) + " 条未读"
                            color: Theme.accent
                            font.pixelSize: 10
                        }
                    }
                }

                // 撤回：正文和资源都不再显示，只留一行提示；两侧措辞不同，
                // 自己撤回的文本还可以一键填回输入框重新编辑
                Item {
                    width: parent.width
                    height: model.recalled ? 34 : 0
                    visible: model.recalled
                    Row {
                        anchors.centerIn: parent
                        spacing: 8
                        Text {
                            text: model.isSelf ? "你撤回了一条消息"
                                               : "「" + model.name + "」撤回了一条消息"
                            textFormat: Text.PlainText
                            color: Theme.textHint
                            font.pixelSize: 10
                        }
                        Text {
                            visible: model.recallText.length > 0
                            text: "重新编辑"
                            color: Theme.primary
                            font.pixelSize: 10
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.restoreRecalled(model.recallText)
                            }
                        }
                    }
                }

                // 群系统消息：居中灰字，不画头像与气泡，和前后的聊天内容区分开
                Item {
                    width: parent.width
                    height: model.isSystem ? 30 : 0
                    visible: model.isSystem
                    Rectangle {
                        anchors.centerIn: parent
                        width: systemLabel.implicitWidth + 20
                        height: 22
                        radius: 11
                        color: Theme.panelAlt
                        Text {
                            id: systemLabel
                            anchors.centerIn: parent
                            text: model.text
                            textFormat: Text.PlainText
                            color: Theme.textSecondary
                            font.pixelSize: 10
                        }
                    }
                }

                Loader {
                    id: loader
                    width: parent.width
                    visible: !model.recalled && !model.isSystem
                    sourceComponent: model.msgType >= 2 ? mediaComponent : bubbleComponent
                }
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
                    senderName: model.showSender ? model.senderName : ""
                    imageSource: model.imageSource
                    imageWidth: model.imageWidth
                    imageHeight: model.imageHeight
                    status: model.status
                    statusText: model.statusText
                    retryable: model.canRetry
                    mentioned: model.mentioned
                    highlighted: root.highlightMessageId > 0
                        && model.messageId === root.highlightMessageId
                    onRetryRequested: {
                        const code = root.bridge.retryMessage(model.msgId)
                        if (code === 1)
                            root.showActionMessage(root.blockedText.length > 0
                                ? root.blockedText : "对方还不是你的好友，发送朋友验证后才能重发")
                        else if (code === 2)
                            root.showActionMessage(model.msgType >= 2
                                ? "无法重发：原文件已不在本机" : "这条消息无法重发")
                    }
                    transferProgress: model.transferProgress
                    onImageClicked: root.openImage(model.messageId, model.imageSource)
                    onContextRequested: (x, y) => root.showMessageMenu(
                        model.messageId, model.msgType, model.text, model.imageSource,
                        mapToItem(root, x, y), model.canRecall)
                }
            }

            Component {
                id: mediaComponent
                MediaBubble {
                    width: messageRow.width
                    avatar: model.avatar
                    name: model.name
                    senderName: model.showSender ? model.senderName : ""
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
                        model.fileName, mapToItem(root, x, y), model.canRecall)
                }
            }
        }

        // 只有停在底部时才跟随新消息；向上翻历史时不再被拉回底部，
        // 图片异步加载导致的高度变化也用短定时器合并，避免连续跳动
        onCountChanged: {
            const total = messageList.count
            const delta = total - root.lastMessageCount
            root.lastMessageCount = total
            if (root.jumping)
                return
            //正在向前补历史时不贴底：那会把刚翻上去的内容又甩回末尾，也不该计新消息
            if (root.prepending)
                return
            if (delta > 0 && !root.stickBottom && !root.settling)
                root.pendingNew += delta
            if (root.stickBottom || root.settling) {
                root.pendingNew = 0
                Qt.callLater(messageList.positionViewAtEnd)
            }
        }
        onContentHeightChanged: {
            if (root.jumping)
                return
            //插入期间按差值把 contentY 往下推，视觉上内容停在原位
            if (root.prepending) {
                messageList.contentY += messageList.contentHeight - root.prependHeight
                root.prependHeight = messageList.contentHeight
            }
            if ((root.stickBottom || root.settling) && !messageList.moving)
                scrollTimer.restart()
        }
        //离底部 64px 内就视为“在底部”：图片加载让 contentHeight 变高时还能继续跟到底，
        //快速滚动到末尾也不会停在最后一张图上
        onMovementEnded: {
            //用户主动拖动说明要自己看，settling 立即结束，jumping 也取消，不再强行贴底
            if (root.jumping) {
                root.jumping = false
                root.jumpRow = -1
                jumpTimer.stop()
            }
            if (root.settling) {
                root.settling = false
                settleTimer.stop()
            }
            root.stickBottom = contentY + height >= contentHeight - 64
            if (root.stickBottom)
                scrollTimer.restart() //滚动期间新加载的图片可能把底部又推高了
        }
        onContentYChanged: {
            if (root.jumping)
                return
            //settling 期间的位置变化是程序贴底造成的，不能据此判定"用户翻上去了"
            if (root.settling)
                return
            if (!moving && !flicking)
                root.stickBottom = contentY + height >= contentHeight - 64
            //离顶部 120px 内就开始预取，别等真的拉到 0 才发请求
            if (contentY < 120 && count > 0 && !root.prepending)
                root.loadOlder()
        }
    }

    // 打开会话后的贴底窗口：模型重建与首屏图片加载都在这段时间内完成
    Timer {
        id: settleTimer
        interval: 320
        onTriggered: {
            root.settling = false
            if (root.jumping)
                return
            if (root.stickBottom)
                messageList.positionViewAtEnd()
        }
    }

    // 前插补偿的观察窗口：委托是懒创建的，内容高度会在几帧内分几次变大，
    // 因此留一小段时间持续按差值补偿，而不是只算一次
    Timer {
        id: prependTimer
        interval: 160
        onTriggered: {
            root.prepending = false
            root.prependHeight = 0
        }
    }

    Timer {
        id: scrollTimer
        interval: 40
        onTriggered: if (root.stickBottom && !root.jumping) messageList.positionViewAtEnd()
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

    //@ 成员选择：浮在输入区上方，键盘上下选择、回车确认、Esc 收起
    Rectangle {
        id: mentionPicker
        visible: root.mentionOpen && root.isGroup && root.filteredMentions.length > 0
        x: composer.x + 16
        y: composer.y - height - 8
        width: 220
        height: Math.min(240, mentionList.contentHeight + 8)
        radius: 8
        color: Theme.panelBg
        border.width: 1
        border.color: Theme.line
        z: 40

        ListView {
            id: mentionList
            anchors.fill: parent
            anchors.margins: 4
            clip: true
            model: root.filteredMentions
            spacing: 2
            boundsBehavior: Flickable.StopAtBounds

            delegate: Item {
                width: mentionList.width
                height: 34
                Rectangle {
                    anchors.fill: parent
                    radius: 6
                    color: mentionArea.containsMouse ? Theme.selected : "transparent"
                }
                Avatar {
                    x: 6
                    width: 24
                    height: 24
                    radius: 7
                    anchors.verticalCenter: parent.verticalCenter
                    source: modelData.icon
                    name: modelData.name
                }
                Text {
                    x: 38
                    width: parent.width - 46
                    anchors.verticalCenter: parent.verticalCenter
                    text: modelData.name
                    textFormat: Text.PlainText
                    color: Theme.textPrimary
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
                MouseArea {
                    id: mentionArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.pickMention(modelData.uid, modelData.name)
                }
            }
        }
    }

    // 输入区：工具与发送在顶部，附件在编辑区上方逐条排列，编辑区保持固定高度。
    // 附件多时只让附件列表变高并自身滚动，不再挤压文字输入空间。
    Rectangle {
        id: composer
        visible: !root.inputLocked
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: 24
        anchors.rightMargin: 24
        anchors.bottomMargin: 22
        height: root.inputLocked ? 0 : root.composerHeight
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
                    usable: true
                    Accessible.name: "表情"
                    onClicked: root.emojiOpen = !root.emojiOpen
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
                    root.emojiOpen = false
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
                //默认 4px 文档边距会让输入文字比占位提示缩进一截，
                //表情也和气泡里的位置对不上，这里清零
                textMargin: 0
                selectionColor: Theme.selected
                selectedTextColor: Theme.textPrimary
                onCursorRectangleChanged: root.followCursor()
                onTextChanged: root.trackMention()

                Keys.onPressed: (event) => {
                    if (event.matches(StandardKey.Paste)) {
                        root.pasteFromClipboard()
                        event.accepted = true
                        return
                    }
                    if (inputMethodComposing)
                        return
                    if (event.key === Qt.Key_Escape && root.emojiOpen) {
                        root.emojiOpen = false
                        event.accepted = true
                        return
                    }
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
                text: "写点什么…（Enter 发送）"
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

    // 已离开的群：输入区整体收掉，只留一条说明，避免"能打字但发不出去"
    Rectangle {
        id: readOnlyBar
        visible: root.inputLocked
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: 24
        anchors.rightMargin: 24
        anchors.bottomMargin: 22
        height: visible ? 46 : 0
        radius: 10
        color: Theme.panelAlt
        border.width: 1
        border.color: Theme.line

        Row {
            anchors.centerIn: parent
            spacing: 8
            AppIcon {
                name: "lock"
                size: 15
                color: Theme.textSecondary
            }
            Text {
                text: "输入已关闭，只能查看历史消息"
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSmall
            }
        }
    }

    // 上翻历史时新消息到了：不强行拉回底部，只给一张可点的浮签
    Rectangle {
        id: newMessagePill
        visible: root.pendingNew > 0 && !root.inputLocked
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: root.inputLocked ? readOnlyBar.top : composer.top
        anchors.bottomMargin: 12
        width: pillLabel.implicitWidth + 30
        height: 30
        radius: 15
        color: Theme.panelBg
        border.width: 1
        border.color: Theme.accent
        z: 20

        Text {
            id: pillLabel
            anchors.centerIn: parent
            text: "↓ " + (root.pendingNew > 99 ? "99+" : root.pendingNew) + " 条新消息"
            color: Theme.accent
            font.pixelSize: 11
        }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: {
                root.pendingNew = 0
                root.stickBottom = true
                messageList.positionViewAtEnd()
            }
        }
    }

    // 表情面板：Grid 按可用宽度自动换行，窄窗口不会把表情挤出边界
    Rectangle {
        id: emojiPanel
        visible: root.emojiOpen
        anchors.left: composer.left
        anchors.right: composer.right
        anchors.bottom: composer.top
        anchors.bottomMargin: 8
        readonly property int cell: 56
        readonly property int gap: 6
        readonly property int columns: Math.max(1, Math.floor((width - 24 + gap) / (cell + gap)))
        readonly property int rows: Math.ceil(Emoji.items.length / columns)
        height: 20 + rows * cell + Math.max(0, rows - 1) * gap
        radius: 10
        color: Theme.panelBg
        border.width: 1
        border.color: Theme.line
        z: 30

        Grid {
            x: 12
            y: 10
            columns: emojiPanel.columns
            rowSpacing: emojiPanel.gap
            columnSpacing: emojiPanel.gap

            Repeater {
                model: Emoji.items
                delegate: Rectangle {
                    width: emojiPanel.cell
                    height: emojiPanel.cell
                    radius: 9
                    color: emojiHover.containsMouse ? Theme.selected : "transparent"

                    Image {
                        anchors.centerIn: parent
                        //面板按素材比例放大到两倍消息内尺寸，和消息里长得一样，不压扁
                        width: Emoji.size * 2
                        height: Emoji.height * 2
                        source: Emoji.assetUrl(modelData.file)
                        fillMode: Image.PreserveAspectFit
                        smooth: true
                    }
                    MouseArea {
                        id: emojiHover
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.insertEmoji(modelData.token, Emoji.assetUrl(modelData.file))
                    }
                    ToolTip.visible: emojiHover.containsMouse
                    ToolTip.text: modelData.name
                    ToolTip.delay: 350
                }
            }
        }
    }

    // 在光标处插入表情；有选区时先替换选区，和普通输入保持一致
    function insertEmoji(token, url) {
        if (!input || !composerBridge)
            return
        if (input.selectedText.length > 0)
            input.remove(input.selectionStart, input.selectionEnd)
        const pos = composerBridge.insertEmoji(input.textDocument, input.cursorPosition,
                                               token, url, Emoji.size, Emoji.height)
        if (pos >= 0)
            input.cursorPosition = pos
        //面板保持打开：连续发几个表情不用反复点工具按钮，发送或按 Esc 时再收起
        input.forceActiveFocus()
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
        const parts = composerBridge.parts(input.textDocument)
        closeMentionPicker()
        root.sendRequested(parts)
    }

    //只跟踪群聊里的 @：从光标往前找最近的 "@"，遇到空格/换行就停。
    //关键词超过 12 个字视为普通文字，不再弹成员列表
    function trackMention() {
        if (!root.isGroup) {
            if (root.mentionOpen)
                closeMentionPicker()
            return
        }
        const pos = input.cursorPosition
        if (pos <= 0) {
            closeMentionPicker()
            return
        }
        const plain = input.getText(0, pos)
        let at = -1
        for (let i = plain.length - 1; i >= 0; --i) {
            const ch = plain.charAt(i)
            if (ch === "@") {
                at = i
                break
            }
            if (ch === " " || ch === "\n" || ch === "\u00a0")
                break
        }
        if (at < 0) {
            closeMentionPicker()
            return
        }
        const query = plain.substring(at + 1)
        if (query.length > 12) {
            closeMentionPicker()
            return
        }
        root.mentionAnchor = at
        root.mentionQuery = query
        root.mentionOpen = true
    }

    function closeMentionPicker() {
        root.mentionOpen = false
        root.mentionAnchor = -1
        root.mentionQuery = ""
    }

    //选中成员：删掉用户输入的 "@关键词"，插入规范的 "@名字 "，光标落在空格后
    function pickMention(uid, name) {
        if (root.mentionAnchor < 0)
            return
        const anchor = root.mentionAnchor
        input.remove(anchor, input.cursorPosition)
        if (composerBridge.insertMention(input.textDocument, anchor, name, uid))
            input.cursorPosition = anchor + String(name).length + 2
        closeMentionPicker()
        input.forceActiveFocus()
    }

    // 仅在外层接受提交后清空，断线时原文仍留在编辑区。
    function clearInput() {
        input.clear()
        inputView.contentY = 0
        composerBridge.clearAttachments()
        composerBridge.clearDraft()
    }

    // 自己刚发出一条消息：无论之前翻到哪里都跟到底部
    function sent() {
        root.stickBottom = true
        Qt.callLater(messageList.positionViewAtEnd)
    }

    // 撤回后重新编辑：把原文填回输入框，光标放到末尾
    function restoreRecalled(text) {
        if (!text)
            return
        input.text = text
        input.cursorPosition = text.length
        input.forceActiveFocus()
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
    function showMessageMenu(messageId, msgType, text, imageUrl, point, canRecall) {
        // 撤回只在时间窗内、自己发的、未撤回的消息上出现，不做成灰掉的菜单项
        const recallItem = { text: "撤回", action: () => root.bridge.recallMessage(messageId) }
        if (msgType === 1) {
            const local = root.bridge ? root.bridge.resourceUrl(messageId).length > 0 : false
            const items = [
                { text: "查看大图", action: () => root.openImage(messageId, imageUrl) },
                { text: "复制图片", action: () => {
                    if (!root.bridge.copyImage(messageId, imageUrl))
                        root.showActionMessage("图片还没有下载完成")
                } },
                { text: "另存为", enabled: local,
                  action: () => root.beginImageSaveAs(messageId, imageUrl) },
                { text: "打开文件夹", enabled: local,
                  action: () => root.bridge.revealResource(messageId) }
            ]
            if (canRecall)
                items.push(recallItem)
            contextMenu.open(point.x, point.y, items)
            return
        }
        const items = [
            { text: "复制文字", enabled: text.length > 0,
              action: () => root.bridge.copyText(text) }
        ]
        if (canRecall)
            items.push(recallItem)
        contextMenu.open(point.x, point.y, items)
    }

    //视频/文件：按传输状态给出下载、暂停、继续、打开、定位与另存为
    function showMediaMenu(messageId, msgId, msgType, state, fileName, point, canRecall) {
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
        if (canRecall)
            items.push({ text: "撤回", action: () => root.bridge.recallMessage(messageId) })
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
        anchors.bottom: root.inputLocked ? readOnlyBar.top : composer.top
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
