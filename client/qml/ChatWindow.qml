import QtQuick
import QtCore
import QtQuick.Controls
import XiguChat

// 聊天工作区：一级导航、索引列表和右侧内容共用一套桌面节奏。
// 这里只负责布局与焦点，导航状态和业务动作都交给 ChatBridge。
Item {
    id: root
    onEnabledChanged: if (!enabled) avatarEditor.dismiss()

    //系统通知点击时由 Main.qml 调进来，避免再复制一份导航状态
    readonly property var chatBridge: bridge

    readonly property int sidebarWidth: Theme.sidebarWidth

    // 第二栏宽度可拖动：240–420，双击分隔条恢复 280；只在拖动结束时落盘
    property int indexWidth: Theme.indexWidth
    readonly property int minIndexWidth: 240
    readonly property int maxIndexWidth: Math.max(minIndexWidth, width - sidebarWidth - 380)
    property QtObject layoutPrefs: Settings {
        category: "Layout"
        property int indexWidth: Theme.indexWidth
    }

    // 窄窗口把第二栏收成头像条：优先保证内容区宽度，列表仍然可点。
    // 设置页有自己的分栏，不参与折叠。
    readonly property bool compactIndex: width < 760 && bridge.mode !== ChatBridge.SettingsMode
    readonly property int indexPanelWidth: compactIndex ? Theme.sidebarWidth : Math.min(indexWidth, maxIndexWidth)

    // 搜索快捷键：聊天与联系人共用一套入口，设置页不响应
    Shortcut {
        sequences: [StandardKey.Find]
        enabled: root.enabled && !root.modalOpen && !root.compactIndex && (bridge.mode === ChatBridge.ContactMode
            || (bridge.mode === ChatBridge.ChatMode && root.visiblePane !== ChatBridge.ChatPage))
        onActivated: {
            if (bridge.mode === ChatBridge.ChatMode)
                chatSearch.forceInputFocus()
            else
                addSearch.forceInputFocus()
        }
    }

    readonly property int listHeaderHeight: Theme.toolbarHeight

    readonly property int friendCount: bridge.friends.totalCount
    readonly property int visibleFriendCount: bridge.friends.count
    readonly property int pendingCount: bridge.applies.pendingCount
    readonly property int chatCount: bridge.conversations.activeCount
    // 联系人空态：没有好友也没有待处理申请时才显示引导卡
    readonly property bool contactGuideVisible: bridge.mode === ChatBridge.ContactMode
        && friendCount === 0 && pendingCount === 0 && !compactIndex
    // 有好友或有申请时，列表顶部保留「新的朋友」入口
    readonly property bool newFriendEntryVisible: bridge.mode === ChatBridge.ContactMode
        && (friendCount > 0 || pendingCount > 0) && !compactIndex
    // 标签条只在联系人模式且有标签时出现；筛选只影响列表，不影响入口与空态判断
    readonly property bool tagBarVisible: bridge.mode === ChatBridge.ContactMode
        && bridge.tagOptions.length > 0 && !compactIndex
    readonly property bool tagEmptyVisible: tagBarVisible && bridge.tagFilter.length > 0
        && visibleFriendCount === 0
    property bool tagFilterOpen: false //标签「更多」浮层是否展开

    // 任一模态浮层打开时，主内容不接收拖拽与快捷键
    readonly property bool modalOpen: dialogs.visible || createGroupDialog.visible
        || profileDialogs.visible || viewer.visible
        || bridge.clearConfirmVisible || bridge.deleteConfirmVisible || bridge.logoutConfirmVisible
        || forwardDialog.visible

    // 会话列表：↑/↓ 逐条切换并直接打开，Esc 回输入框
    function stepConversation(delta) {
        const count = bridge.conversations.count
        if (count <= 0)
            return
        const base = conversationList.currentIndex < 0 ? (delta > 0 ? -1 : 0)
                                                       : conversationList.currentIndex
        const next = Math.max(0, Math.min(count - 1, base + delta))
        conversationList.currentIndex = next
        conversationList.positionViewAtIndex(next, ListView.Contain)
        Qt.callLater(function() {
            const item = conversationList.currentItem
            if (!item)
                return
            item.forceActiveFocus()
            item.activate()
        })
    }

    // 联系人列表：↑/↓ 只移动焦点，Enter 才打开资料，避免每按一次就换页
    function stepFriend(delta) {
        const count = bridge.friends.count
        if (count <= 0)
            return
        const base = friendsList.currentIndex < 0 ? (delta > 0 ? -1 : 0)
                                                  : friendsList.currentIndex
        const next = Math.max(0, Math.min(count - 1, base + delta))
        friendsList.currentIndex = next
        friendsList.positionViewAtIndex(next, ListView.Contain)
        Qt.callLater(function() {
            if (friendsList.currentItem)
                friendsList.currentItem.forceActiveFocus()
        })
    }
    onTagBarVisibleChanged: if (!tagBarVisible) tagFilterOpen = false
    property bool functionMenuOpen: false

    // 窗口标题与"正在看"判定：最小化或切到别的应用时不能继续把当前会话算成已读
    readonly property int unreadTotal: bridge.unreadTotal
    readonly property var hostWindow: Window.window
    function refreshWindowTitle() {
        if (!hostWindow)
            return
        const count = bridge.unreadTotal
        hostWindow.title = count > 0 ? "(" + (count > 99 ? "99+" : count) + ") xiguchat" : "xiguchat"
    }
    onUnreadTotalChanged: refreshWindowTitle()
    onHostWindowChanged: if (hostWindow) bridge.setWindowActive(hostWindow.active)
    Component.onCompleted: {
        refreshWindowTitle()
        if (hostWindow)
            bridge.setWindowActive(hostWindow.active)
        // 列表栏宽度取本机值，夹在允许范围内
        root.indexWidth = Math.max(root.minIndexWidth, Math.min(420, layoutPrefs.indexWidth || Theme.indexWidth))
    }
    Component.onDestruction: if (hostWindow) hostWindow.title = "xiguchat"
    Connections {
        target: root.hostWindow
        function onActiveChanged() {
            if (root.hostWindow)
                bridge.setWindowActive(root.hostWindow.active)
        }
    }

    //右栏实际可显示的页面：区域与页面不匹配、或资料/会话数据已失效时统一落占位页，
    //避免出现"左边已经切走、右边还停着旧页面"的割裂状态
    readonly property int visiblePane: {
        const pane = bridge.pane
        if (bridge.mode === ChatBridge.ChatMode) {
            if (pane === ChatBridge.ApplyFriendPage
                || pane === ChatBridge.UserInfoPage || pane === ChatBridge.GeneralPage)
                return ChatBridge.EmptyPane
            if (pane === ChatBridge.ChatPage && bridge.currentThreadId <= 0 && bridge.currentUid <= 0)
                return ChatBridge.EmptyPane
            if (pane === ChatBridge.GroupInfoPage && bridge.currentThreadId <= 0)
                return ChatBridge.EmptyPane
        } else if (bridge.mode === ChatBridge.ContactMode) {
            if (pane === ChatBridge.ChatPage || pane === ChatBridge.GroupInfoPage
                || pane === ChatBridge.UserInfoPage || pane === ChatBridge.GeneralPage)
                return ChatBridge.EmptyPane
            if (pane === ChatBridge.FriendInfoPage
                && (!bridge.profile || !(bridge.profile.uid > 0)))
                return ChatBridge.EmptyPane
        } else if (bridge.mode === ChatBridge.SettingsMode) {
            if (pane !== ChatBridge.UserInfoPage && pane !== ChatBridge.GeneralPage)
                return ChatBridge.EmptyPane
        }
        return pane
    }

    // 右栏空态跟着区域走：会话区说"聊天"，联系人区说"联系人"，不能混用
    readonly property string emptyTitle: {
        if (bridge.mode === ChatBridge.SettingsMode)
            return "设置"
        if (bridge.mode === ChatBridge.ContactMode) {
            if (friendCount === 0)
                return pendingCount > 0 ? "有 " + pendingCount + " 条好友申请待处理"
                                        : "还没有联系人"
            return pendingCount > 0 ? "有 " + pendingCount + " 条好友申请待处理"
                                    : "选择一位联系人，查看资料"
        }
        return chatCount === 0 ? "还没有会话" : "选择一个会话"
    }
    readonly property string emptyAction: {
        if (bridge.mode === ChatBridge.SettingsMode)
            return ""
        if (bridge.mode === ChatBridge.ContactMode) {
            if (pendingCount > 0)
                return "查看申请"
            return friendCount === 0 ? "添加好友" : ""
        }
        if (chatCount > 0)
            return ""
        if (friendCount === 0)
            return pendingCount > 0 ? "查看申请" : "添加好友"
        return "查看联系人"
    }

    function handlePlus() {
        if (bridge.mode === ChatBridge.ChatMode) {
            functionMenuOpen = !functionMenuOpen
            return
        }
        if (bridge.addFriendOpen) {
            bridge.closeAddFriend()
            return
        }
        bridge.openAddFriend()
        Qt.callLater(addSearch.forceInputFocus)
    }

    function openAddFriendFromChat() {
        functionMenuOpen = false
        bridge.showContactMode()
        bridge.openAddFriend()
        Qt.callLater(addSearch.forceInputFocus)
    }

    // 发起群聊：先收起功能菜单，再交给桥上打开弹层。
    // 必须定义在根对象上：MouseArea 的 onClicked 处于自己的作用域，
    // 直接写 openGroupCreator() 会在运行时报 ReferenceError（父项里的函数不在作用域链上）
    function openGroupCreator() {
        functionMenuOpen = false
        bridge.openCreateGroup()
    }

    function runEmptyAction() {
        if (bridge.mode === ChatBridge.ContactMode) {
            if (pendingCount > 0)
                bridge.openApplyList()
            else if (friendCount === 0)
                bridge.openAddFriend()
            return
        }
        if (friendCount === 0) {
            if (pendingCount > 0) {
                bridge.openApplyList()
            } else {
                bridge.showContactMode()
                bridge.openAddFriend()
            }
            return
        }
        bridge.showContactMode()
    }

    ChatBridge {
        id: bridge
        objectName: "chatBridge"
    }

    // 输入框是命令式更新的：bridge 变化时同步文本，避免绑定被打断后清不掉
    Connections {
        target: bridge
        function onSearchTextChanged() {
            addSearch.setText(bridge.searchText)
        }
        function onFriendQueryChanged() {
            chatSearch.setText(bridge.friendQuery)
        }
        function onModeChanged() {
            root.functionMenuOpen = false
        }
        function onAddFriendOpenChanged() {
            if (bridge.addFriendOpen)
                Qt.callLater(addSearch.forceInputFocus)
            else
                addSearch.clearInputFocus()
        }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.chatBg
    }

    // 整窗接收文件：拖到任意位置都交给输入区的同一套处理，覆盖层只负责提示
    DropArea {
        id: windowDrop
        anchors.fill: parent
        z: 600
        enabled: root.visiblePane === ChatBridge.ChatPage && !root.modalOpen

        Rectangle {
            anchors.fill: parent
            visible: windowDrop.containsDrag
            color: Qt.rgba(Theme.primary.r, Theme.primary.g, Theme.primary.b, 0.06)
            border.width: 2
            border.color: Qt.rgba(Theme.primary.r, Theme.primary.g, Theme.primary.b, 0.35)

            Text {
                anchors.centerIn: parent
                text: "松开以添加附件"
                color: Theme.primary
                font.pixelSize: Theme.fontBody
                font.weight: Font.Medium
            }
        }

        onEntered: (drag) => { drag.accepted = drag.hasUrls }
        onDropped: (drop) => {
            if (drop.hasUrls)
                chatPage.acceptFiles(drop.urls)
            drop.acceptProposedAction()
        }
    }

    Row {
        anchors.fill: parent
        enabled: !root.modalOpen

        // 一级导航：聊天与联系人在上，设置与本人头像固定在底部
        Rectangle {
            width: root.sidebarWidth
            height: parent.height
            color: Theme.sidebarBg

            BrandMark {
                y: 20
                anchors.horizontalCenter: parent.horizontalCenter
                size: 28
            }

            // 选中态由当前区域决定
            Column {
                y: 96
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 8
                SideButton {
                    kind: "chat"
                    active: bridge.mode === ChatBridge.ChatMode
                    //会话徽标显示所有未读之和，与联系人徽标共用同一套样式
                    badge: bridge.unreadTotal
                    onClicked: bridge.showChatMode()
                }
                SideButton {
                    kind: "contact"
                    active: bridge.mode === ChatBridge.ContactMode
                    badge: bridge.applies.pendingCount
                    onClicked: bridge.showContactMode()
                }

            }

            // 设置入口固定在头像上方
            SideButton {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: ownAvatar.top
                anchors.bottomMargin: 12
                kind: "settings"
                active: bridge.mode === ChatBridge.SettingsMode
                onClicked: bridge.showSettings()
            }

            // 本人头像：点击等同于进入设置里的个人资料
            Avatar {
                id: ownAvatar
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 24
                anchors.horizontalCenter: parent.horizontalCenter
                width: 36
                height: 36
                radius: 11
                source: bridge.myIcon
                name: bridge.myName
                activeFocusOnTab: true
                border.width: activeFocus ? 2 : 0
                border.color: Theme.primary
                Accessible.role: Accessible.Button
                Accessible.name: "打开个人资料"
                Accessible.onPressAction: bridge.openMyProfile()
                Keys.onSpacePressed: bridge.openMyProfile()
                Keys.onReturnPressed: bridge.openMyProfile()
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: bridge.openMyProfile()
                }
            }
            Rectangle {
                anchors.right: parent.right
                width: 1
                height: parent.height
                color: Theme.line
            }
        }

        // 第二栏：标题、搜索框与列表。设置模式下整栏让位给 SettingsPanel
        Rectangle {
            id: indexPanel
            visible: bridge.mode !== ChatBridge.SettingsMode
            width: root.indexPanelWidth
            height: parent.height
            color: Theme.panelBg

            Item {
                id: listHeader
                width: parent.width
                height: root.listHeaderHeight

                // 标题与统计随模式切换；加号在聊天页打开功能菜单，在联系人页打开加好友浮层
                Text {
                    x: 20
                    y: 10
                    visible: !root.compactIndex
                    text: bridge.mode === ChatBridge.ChatMode ? "聊天" : "联系人"
                    color: Theme.textPrimary
                    font.pixelSize: 22
                    font.bold: true
                }
                Text {
                    x: 20
                    y: 39
                    visible: !root.compactIndex
                    text: bridge.mode === ChatBridge.ChatMode
                        ? root.chatCount + " 个会话"
                        : bridge.friends.count + " 位好友"
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSmall
                }

                Rectangle {
                    anchors.right: parent.right
                    anchors.rightMargin: root.compactIndex ? 21 : 18
                    anchors.verticalCenter: parent.verticalCenter
                    width: 30
                    height: 30
                    radius: Theme.pillRadius
                    color: searchEntry.containsMouse || activeFocus ? Theme.selected : Theme.panelAlt
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: bridge.mode === ChatBridge.ChatMode ? "更多功能" : "添加好友"
                    Accessible.onPressAction: root.handlePlus()
                    Keys.onSpacePressed: root.handlePlus()
                    Keys.onReturnPressed: root.handlePlus()
                    AppIcon {
                        anchors.centerIn: parent
                        name: "plus"
                        size: 16
                        color: Theme.primary
                    }
                    MouseArea {
                        id: searchEntry
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.handlePlus()
                    }
                }
            }

            // 两种模式共用同一个输入位置：聊天页过滤好友，联系人页添加好友
            Item {
                id: searchArea
                x: 16
                y: listHeader.height + 2
                width: parent.width - 32
                height: 38
                visible: !root.compactIndex

                SearchField {
                    id: chatSearch
                    anchors.fill: parent
                    visible: bridge.mode === ChatBridge.ChatMode
                    placeholder: "搜索联系人"
                    inputObjectName: "chatSearchInput"
                    onTextEdited: (value) => bridge.friendQuery = value
                    onAccepted: {
                        if (bridge.friendMatches.length > 0)
                            bridge.openChat(bridge.friendMatches[0].uid)
                    }
                    onEscapePressed: bridge.clearFriendSearch()
                    onClearRequested: bridge.clearFriendSearch()
                    onFocusGained: root.functionMenuOpen = false
                }
                SearchField {
                    id: addSearch
                    anchors.fill: parent
                    visible: bridge.mode === ChatBridge.ContactMode
                    placeholder: "搜索用户名或 ID"
                    inputObjectName: "searchInput"
                    onTextEdited: (value) => bridge.searchText = value
                    onAccepted: bridge.search()
                    onEscapePressed: bridge.closeAddFriend()
                    onClearRequested: bridge.clearSearch()
                    onFocusGained: bridge.openAddFriend()
                }
            }

            // 聊天页好友过滤结果，浮在会话列表上方
            FriendSearchPopup {
                id: friendSearchPopup
                visible: bridge.mode === ChatBridge.ChatMode && bridge.friendQuery.trim().length > 0
                x: searchArea.x
                y: searchArea.y + searchArea.height
                width: searchArea.width
                z: 24
                controller: bridge
                onAddFriendRequested: root.openAddFriendFromChat()
            }

            // 联系人页加好友浮层，搜索框仍是上面的输入框
            AddFriendPopover {
                id: addFriendPopover
                visible: bridge.addFriendOpen
                x: searchArea.x
                y: searchArea.y + searchArea.height
                width: searchArea.width
                z: 24
                controller: bridge
                onApplyRequested: (uid, name) => dialogs.openApplyDialog(uid, name)
                onRetryRequested: {
                    bridge.clearSearch()
                    addSearch.forceInputFocus()
                }
            }

            // 浮层打开时，点列表空白处只负责收起，不触发下面的条目
            MouseArea {
                visible: bridge.addFriendOpen
                anchors.top: searchArea.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                z: 20
                onClicked: bridge.closeAddFriend()
            }
            MouseArea {
                visible: bridge.mode === ChatBridge.ChatMode && bridge.friendQuery.trim().length > 0
                anchors.top: searchArea.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                z: 20
                onClicked: bridge.clearFriendSearch()
            }

            // 联系人页的标签筛选：只占一行，放不下的标签收进「更多」浮层。
            // 放在搜索框与列表之间，不随列表滚动，切走再回来仍能看见当前分组。
            Item {
                id: contactTagBar
                visible: root.tagBarVisible
                x: 12
                y: searchArea.y + searchArea.height + 8
                width: parent.width - 24
                height: visible ? 30 : 0

                // 用文字度量估每个标签的宽度，决定这一行能放几个，避免依赖滚动。
                // 这里必须用 FontMetrics 的纯函数：TextMetrics 要写 text 再读 boundingRect，
                // 在绑定求值里属于副作用，会触发 shownTags 的绑定循环
                FontMetrics {
                    id: tagFont
                    font.pixelSize: Theme.fontSmall
                }

                readonly property int moreWidth: 52
                function chipWidth(text) {
                    return Math.max(34, Math.ceil(tagFont.advanceWidth(text)) + 24)
                }
                // 完整放得下的标签数；预留「更多」的位置，避免它压住最后一个
                function countVisibleTags() {
                    const options = bridge.tagOptions
                    let used = chipWidth("全部") + 7
                    let count = 0
                    for (let i = 0; i < options.length; ++i) {
                        const width = chipWidth(options[i]) + 7
                        const reserve = (i < options.length - 1) ? moreWidth + 7 : 0
                        if (used + width + reserve > contactTagBar.width)
                            break
                        used += width
                        ++count
                    }
                    return count
                }

                readonly property var shownTags: {
                    const list = bridge.tagOptions.slice(0, countVisibleTags())
                    // 当前筛选的标签被挤出可视区时用它替换最后一个，保证筛选状态看得见
                    const current = bridge.tagFilter
                    if (current.length > 0 && list.indexOf(current) < 0) {
                        if (list.length > 0)
                            list[list.length - 1] = current
                        else
                            list.push(current)
                    }
                    return list
                }
                readonly property bool overflow: shownTags.length < bridge.tagOptions.length

                Row {
                    id: tagRow
                    width: parent.width
                    height: parent.height
                    spacing: 7

                    // 「全部」固定在最前，空串表示不筛选
                    Rectangle {
                        id: allChip
                        readonly property bool chosen: bridge.tagFilter.length === 0
                        width: allLabel.implicitWidth + 24
                        height: 28
                        anchors.verticalCenter: parent.verticalCenter
                        radius: 5
                        color: chosen ? Theme.selected : Theme.panelAlt
                        border.width: 1
                        border.color: chosen ? Theme.primary : "transparent"
                        Text {
                            id: allLabel
                            anchors.centerIn: parent
                            text: "全部"
                            color: allChip.chosen ? Theme.textPrimary : Theme.textSecondary
                            font.pixelSize: Theme.fontSmall
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                root.tagFilterOpen = false
                                bridge.clearTagFilter()
                            }
                        }
                    }
                    Repeater {
                        model: contactTagBar.shownTags
                        delegate: Rectangle {
                            readonly property bool chosen: bridge.tagFilter === modelData
                            width: tagLabel.implicitWidth + 24
                            height: 28
                            anchors.verticalCenter: parent.verticalCenter
                            radius: 5
                            color: chosen ? Theme.selected : Theme.panelAlt
                            border.width: 1
                            border.color: chosen ? Theme.primary : "transparent"
                            Text {
                                id: tagLabel
                                anchors.centerIn: parent
                                text: modelData
                                textFormat: Text.PlainText
                                color: chosen ? Theme.textPrimary : Theme.textSecondary
                                font.pixelSize: Theme.fontSmall
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    root.tagFilterOpen = false
                                    bridge.setTagFilter(modelData)
                                }
                            }
                        }
                    }
                }

                // 「更多」固定在最右，浮层里能看到完整标签列表
                Rectangle {
                    visible: contactTagBar.overflow
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: contactTagBar.moreWidth
                    height: 28
                    radius: 5
                    color: root.tagFilterOpen ? Theme.selected : Theme.panelAlt
                    border.width: 1
                    border.color: root.tagFilterOpen ? Theme.primary : "transparent"
                    Text {
                        anchors.centerIn: parent
                        text: "更多"
                        color: root.tagFilterOpen ? Theme.textPrimary : Theme.textSecondary
                        font.pixelSize: Theme.fontSmall
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.tagFilterOpen = !root.tagFilterOpen
                    }
                }
            }

            // 标签浮层的空白遮罩：点列表区域即可收起，不影响浮层本身
            MouseArea {
                visible: root.tagFilterOpen
                anchors.top: contactTagBar.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                z: 20
                onClicked: root.tagFilterOpen = false
            }

            TagFilterPopover {
                id: tagFilterPopover
                visible: root.tagFilterOpen
                x: contactTagBar.x
                y: contactTagBar.y + contactTagBar.height + 4
                width: contactTagBar.width
                z: 26
                controller: bridge
                onTagChosen: (tag) => {
                    root.tagFilterOpen = false
                    bridge.setTagFilter(tag)
                }
                onDismissed: root.tagFilterOpen = false
                onVisibleChanged: if (visible) forceActiveFocus()
            }

            // 筛选后没有匹配的好友：给一句提示，但不影响「新的朋友」入口与空态引导
            Text {
                visible: root.tagEmptyVisible
                anchors.top: contactTagBar.bottom
                anchors.topMargin: 40
                anchors.horizontalCenter: parent.horizontalCenter
                text: "「" + bridge.tagFilter + "」下还没有好友"
                textFormat: Text.PlainText
                color: Theme.textHint
                font.pixelSize: Theme.fontSmall
            }

            // 折叠态没有整行的「新的朋友」，用一个头像位保留申请入口
            Item {
                id: compactNewFriend
                visible: root.compactIndex && bridge.mode === ChatBridge.ContactMode
                    && root.pendingCount > 0
                x: 0
                y: listHeader.height + 6
                width: root.indexPanelWidth
                height: 56

                Rectangle {
                    anchors.centerIn: parent
                    width: 48
                    height: 48
                    radius: 12
                    color: compactFriendArea.containsMouse ? Theme.selected : Theme.panelAlt

                    AppIcon {
                        anchors.centerIn: parent
                        name: "contacts"
                        size: 20
                        color: Theme.primary
                    }

                    Rectangle {
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.rightMargin: -4
                        anchors.topMargin: -4
                        width: 16
                        height: 16
                        radius: 8
                        color: Theme.accent
                        Text {
                            anchors.centerIn: parent
                            text: root.pendingCount > 9 ? "9+" : root.pendingCount
                            color: "#FFFFFF"
                            font.pixelSize: 10
                        }
                    }

                    MouseArea {
                        id: compactFriendArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: bridge.openApplyList()
                    }
                }

                ToolTip.visible: compactFriendArea.containsMouse
                ToolTip.text: "新的朋友"
                ToolTip.delay: 350
            }

            // 会话列表：单聊与群聊合成一张表，按最后消息时间倒序；
            // 联系人列表仍用好友模型，两边不再共用同一个 delegate
            ListView {
                id: conversationList
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: root.compactIndex ? listHeader.bottom : searchArea.bottom
                anchors.topMargin: root.compactIndex ? 6 : 17
                anchors.bottom: parent.bottom
                clip: true
                visible: bridge.mode === ChatBridge.ChatMode
                model: bridge.conversations
                spacing: 2
                boundsBehavior: Flickable.StopAtBounds
                activeFocusOnTab: true
                Keys.onDownPressed: (event) => { root.stepConversation(1); event.accepted = true }
                Keys.onUpPressed: (event) => { root.stepConversation(-1); event.accepted = true }
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                delegate: Item {
                    id: convRow
                    width: conversationList.width
                    height: root.compactIndex ? 56 : Theme.listRowHeight
                    readonly property bool chosen: (root.visiblePane === ChatBridge.ChatPage
                        || root.visiblePane === ChatBridge.GroupInfoPage
                        || (bridge.mode === ChatBridge.ChatMode && root.visiblePane === ChatBridge.FriendInfoPage))
                        && bridge.currentThreadId === model.threadId
                    readonly property bool group: model.kind === 2
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: model.displayName
                    Accessible.onPressAction: activate()
                    Keys.onSpacePressed: activate()
                    Keys.onReturnPressed: activate()
                    Keys.onEnterPressed: activate()
                    Keys.onDownPressed: (event) => { root.stepConversation(1); event.accepted = true }
                    Keys.onUpPressed: (event) => { root.stepConversation(-1); event.accepted = true }
                    Keys.onEscapePressed: (event) => { chatPage.focusComposer(); event.accepted = true }
                    onActiveFocusChanged: {
                        if (activeFocus)
                            conversationList.positionViewAtIndex(index, ListView.Contain)
                    }
                    // 程序化打开会话（通知、快捷键）时也同步游标，键盘导航从当前会话继续
                    onChosenChanged: if (chosen) conversationList.currentIndex = index

                    function activate() {
                        conversationList.currentIndex = index
                        if (group)
                            bridge.openGroup(model.threadId)
                        else
                            bridge.openChat(model.uid)
                    }

                    // 会话菜单只做"整理"，不再重复点击行就能完成的"发消息"。
                    // 删除好友属于关系操作，只留在好友资料页，避免在列表里误触
                    function openMenu(px, py) {
                        //先把当前行的事实抓成常量：菜单开着时列表可能因新消息重排，
                        //闭包不能再依赖已经失效的 delegate model
                        const threadId = model.threadId
                        const kind = model.kind
                        const title = model.displayName
                        const pinned = model.pinned
                        const muted = model.muted
                        const isGroupRow = group
                        const items = [
                            { text: pinned ? "取消置顶" : "置顶会话",
                              action: () => bridge.setThreadPinned(threadId, !pinned) }
                        ]
                        //单聊免打扰是本地设置；群免打扰仍在群资料页，避免两套状态打架
                        if (!isGroupRow) {
                            items.push({ text: muted ? "消息免打扰：开" : "消息免打扰",
                                         action: () => bridge.setThreadMuted(threadId, !muted) })
                        }
                        items.push({ text: "标记为未读",
                                     action: () => bridge.markThreadUnread(threadId) })
                        items.push({ text: "清除聊天记录",
                                     action: () => bridge.askClearHistory(threadId, kind, title) })
                        items.push({ text: "删除会话",
                                     action: () => bridge.askDeleteConversation(threadId, kind, title) })
                        friendMenu.open(px, py, items)
                    }

                    // 右键仍保留为快捷方式，但内容与会话菜单一致
                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.RightButton
                        onClicked: (mouse) => {
                            const point = mapToItem(root, mouse.x, mouse.y)
                            convRow.openMenu(point.x, point.y)
                        }
                    }

                    Rectangle {
                        x: root.compactIndex ? 8 : 10
                        y: root.compactIndex ? 4 : 0
                        width: root.compactIndex ? convRow.width - 16 : parent.width - 20
                        height: root.compactIndex ? 48 : parent.height - 2
                        radius: 10
                        color: convRow.chosen ? Theme.selected
                               : (convArea.containsMouse || convRow.activeFocus ? Theme.panelAlt : "transparent")
                        border.width: 1
                        border.color: convRow.activeFocus ? Theme.primary : "transparent"
                        Behavior on color { ColorAnimation { duration: 130 } }
                    }
                    Avatar {
                        id: convAvatar
                        x: root.compactIndex ? (parent.width - width) / 2 : 22
                        width: 40
                        height: 40
                        radius: 11
                        anchors.verticalCenter: parent.verticalCenter
                        source: model.icon
                        name: model.displayName
                        group: convRow.group
                    }
                    // 置顶像别在信封上的一枚小图钉：只占头像左上角，不抢未读与名字的位置
                    Rectangle {
                        visible: model.pinned && !root.compactIndex
                        x: convAvatar.x - 3
                        y: convAvatar.y - 3
                        width: 16
                        height: 16
                        radius: 8
                        color: Theme.panelBg
                        border.width: 1
                        border.color: Theme.line
                        AppIcon {
                            anchors.centerIn: parent
                            name: "pin"
                            size: 10
                            color: Theme.primary
                        }
                    }
                    Column {
                        anchors.left: convAvatar.right
                        anchors.leftMargin: 12
                        anchors.right: parent.right
                        anchors.rightMargin: 38
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 6
                        visible: !root.compactIndex

                        Item {
                            width: parent.width
                            height: convName.implicitHeight

                            Text {
                                id: convName
                                anchors.left: parent.left
                                //群标记占在名字与时间之间，名字先让位给这两个标记
                                anchors.right: convRow.group ? groupTag.left : convTime.left
                                anchors.rightMargin: 6
                                anchors.verticalCenter: parent.verticalCenter
                                text: model.displayName
                                textFormat: Text.PlainText
                                color: Theme.textPrimary
                                font.pixelSize: 14
                                font.bold: convRow.chosen
                                elide: Text.ElideRight
                            }
                            //群标记：让单聊与群聊在会话列表里一眼分开
                            Rectangle {
                                id: groupTag
                                visible: convRow.group
                                anchors.right: convTime.left
                                anchors.rightMargin: 6
                                anchors.verticalCenter: convName.verticalCenter
                                width: 16
                                height: 14
                                radius: 3
                                color: Theme.primarySoft
                                Text {
                                    anchors.centerIn: parent
                                    text: "群"
                                    font.pixelSize: 10
                                    color: Theme.primary
                                }
                            }
                            Text {
                                id: convTime
                                anchors.right: parent.right
                                anchors.baseline: convName.baseline
                                text: model.lastTime
                                textFormat: Text.PlainText
                                color: Theme.textHint
                                font.pixelSize: Theme.fontCaption
                                visible: text.length > 0
                            }
                        }
                        Text {
                            width: parent.width
                            text: {
                                if (model.draft.length > 0)
                                    return "草稿 · " + model.draft
                                const body = model.lastText.length > 0 ? model.lastText : "还没有消息"
                                return model.atMe ? "[有人@我] " + body : body
                            }
                            color: model.draft.length > 0 || model.atMe
                                ? Theme.accent : Theme.textSecondary
                            font.pixelSize: 12
                            elide: Text.ElideRight
                            maximumLineCount: 1
                            textFormat: Text.PlainText
                        }
                    }

                    // 未读：免打扰的群只显示一个小圆点，不报数字（和微信一致）
                    Rectangle {
                        visible: model.unread > 0 && !model.muted
                        x: convAvatar.x + convAvatar.width - 9
                        y: convAvatar.y - 2
                        width: Math.max(16, convUnread.implicitWidth + 8)
                        height: 16
                        radius: 8
                        color: Theme.accent
                        Text {
                            id: convUnread
                            anchors.centerIn: parent
                            text: model.unread > 99 ? "99+" : model.unread
                            color: Theme.primaryText
                            font.pixelSize: 10
                        }
                    }
                    Rectangle {
                        visible: model.unread > 0 && model.muted
                        x: convAvatar.x + convAvatar.width - 7
                        y: convAvatar.y
                        width: 8
                        height: 8
                        radius: 4
                        color: Theme.accent
                    }
                    MouseArea {
                        id: convArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            convRow.forceActiveFocus(Qt.MouseFocusReason)
                            convRow.activate()
                        }
                    }

                    // 折叠态只有头像，名字用悬停提示补回来
                    ToolTip.visible: root.compactIndex && convArea.containsMouse
                    ToolTip.text: model.displayName
                    ToolTip.delay: 350

                    // 悬停才出现的「…」：会话管理不必依赖右键，触控板与不熟悉右键的用户也能找到
                    Rectangle {
                        id: convMore
                        anchors.right: parent.right
                        anchors.rightMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        width: 24
                        height: 24
                        radius: 6
                        visible: (convArea.containsMouse || convMoreArea.containsMouse
                            || convRow.activeFocus) && !root.compactIndex
                        color: convMoreArea.containsMouse ? Theme.selected : "transparent"
                        AppIcon {
                            anchors.centerIn: parent
                            name: "more"
                            size: 15
                            color: Theme.textSecondary
                        }
                        MouseArea {
                            id: convMoreArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                const point = convMore.mapToItem(root, 0, convMore.height + 2)
                                convRow.openMenu(point.x - 126, point.y)
                            }
                        }
                    }

                    // 键盘也能打开同一张菜单：Shift+F10 或菜单键
                    Keys.onPressed: (event) => {
                        if (event.key === Qt.Key_Menu
                            || (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier))) {
                            const point = convRow.mapToItem(root, 0, 0)
                            convRow.openMenu(point.x + 48, point.y + 40)
                            event.accepted = true
                        }
                    }
                }
            }

            // 联系人列表：只显示好友，按加好友顺序；标签筛选只作用于这张表
            ListView {
                id: friendsList
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: root.compactIndex ? listHeader.bottom
                           : (root.tagBarVisible ? contactTagBar.bottom : searchArea.bottom)
                anchors.topMargin: root.compactIndex
                    ? (compactNewFriend.visible ? 62 : 6)
                    : (root.tagBarVisible ? 10 : 17)
                anchors.bottom: parent.bottom
                clip: true
                model: bridge.friends
                spacing: 2
                boundsBehavior: Flickable.StopAtBounds
                activeFocusOnTab: true
                Keys.onDownPressed: (event) => { root.stepFriend(1); event.accepted = true }
                Keys.onUpPressed: (event) => { root.stepFriend(-1); event.accepted = true }
                visible: bridge.mode === ChatBridge.ContactMode && !root.contactGuideVisible

                // 联系人列表顶部固定「新的朋友」；没有好友但有申请时只显示这一行
                header: Item {
                    width: friendsList.width
                    height: root.newFriendEntryVisible ? (root.friendCount > 0 ? 96 : 72) : 0
                    visible: root.newFriendEntryVisible

                    Rectangle {
                        id: newFriendButton
                        x: 12
                        width: parent.width - 24
                        height: 62
                        radius: 10
                        color: root.visiblePane === ChatBridge.ApplyFriendPage ? Theme.selected
                               : (newFriendArea.containsMouse || activeFocus ? Theme.panelAlt : "transparent")
                        border.width: 1
                        border.color: activeFocus ? Theme.primary : "transparent"
                        activeFocusOnTab: visible
                        Accessible.role: Accessible.Button
                        Accessible.name: root.pendingCount > 0
                            ? "新的朋友，" + root.pendingCount + " 条申请待处理" : "新的朋友"
                        Accessible.onPressAction: bridge.openApplyList()
                        Keys.onSpacePressed: bridge.openApplyList()
                        Keys.onReturnPressed: bridge.openApplyList()
                        Keys.onEnterPressed: bridge.openApplyList()
                        onActiveFocusChanged: if (activeFocus) friendsList.positionViewAtBeginning()
                        Rectangle {
                            x: 10
                            anchors.verticalCenter: parent.verticalCenter
                            width: 36
                            height: 36
                            radius: 9
                            color: Theme.primarySoft
                            AppIcon {
                                anchors.centerIn: parent
                                name: "mail"
                                size: 19
                                color: Theme.primary
                            }
                        }
                        Column {
                            x: 58
                            width: parent.width - 58 - 38
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 3
                            Text {
                                width: parent.width
                                text: "新的朋友"
                                textFormat: Text.PlainText
                                font.pixelSize: 14
                                color: Theme.textPrimary
                                elide: Text.ElideRight
                            }
                            Text {
                                width: parent.width
                                visible: root.pendingCount > 0
                                text: root.pendingCount + " 条申请待处理"
                                textFormat: Text.PlainText
                                font.pixelSize: Theme.fontCaption
                                color: Theme.textSecondary
                                elide: Text.ElideRight
                            }
                        }
                        Rectangle {
                            visible: root.pendingCount > 0
                            anchors.right: parent.right
                            anchors.rightMargin: 12
                            anchors.verticalCenter: parent.verticalCenter
                            width: Math.max(18, pendingLabel.implicitWidth + 10)
                            height: 18
                            radius: 9
                            color: Theme.accent
                            Text {
                                id: pendingLabel
                                anchors.centerIn: parent
                                text: root.pendingCount
                                font.pixelSize: 10
                                color: Theme.primaryText
                            }
                        }
                        MouseArea {
                            id: newFriendArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                newFriendButton.forceActiveFocus(Qt.MouseFocusReason)
                                bridge.openApplyList()
                            }
                        }
                    }
                    Text {
                        x: 22
                        y: 74
                        visible: root.friendCount > 0
                        //筛选时标题直接说明当前分组与命中数量，避免"全部"与结果不符
                        text: bridge.tagFilter.length > 0
                            ? "「" + bridge.tagFilter + "」· " + root.visibleFriendCount + " 位"
                            : "全部联系人"
                        textFormat: Text.PlainText
                        font.pixelSize: 11
                        color: Theme.textHint
                    }
                }

                // 联系人行：只负责打开资料页，最后一条消息与未读属于会话列表
                delegate: Item {
                    id: friendRow
                    width: friendsList.width
                    height: root.compactIndex ? 56 : 60
                    readonly property bool chosen: root.visiblePane === ChatBridge.FriendInfoPage
                        && bridge.profile.uid === model.uid
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: model.displayName
                    Accessible.onPressAction: activate()
                    Keys.onSpacePressed: activate()
                    Keys.onReturnPressed: activate()
                    Keys.onEnterPressed: activate()
                    Keys.onDownPressed: (event) => { root.stepFriend(1); event.accepted = true }
                    Keys.onUpPressed: (event) => { root.stepFriend(-1); event.accepted = true }
                    Keys.onEscapePressed: (event) => { addSearch.forceInputFocus(); event.accepted = true }
                    onActiveFocusChanged: {
                        if (activeFocus)
                            friendsList.positionViewAtIndex(index, ListView.Contain)
                    }

                    function activate() {
                        friendsList.currentIndex = index
                        bridge.openProfile(model.uid)
                    }

                    // 联系人行不再挂右键菜单：点击就是看资料，删除好友只在资料页里做，
                    // 避免在一张每天要扫很多遍的列表里出现不可逆操作

                    Rectangle {
                        x: root.compactIndex ? 8 : 10
                        y: root.compactIndex ? 4 : 0
                        width: root.compactIndex ? friendRow.width - 16 : parent.width - 20
                        height: root.compactIndex ? 48 : parent.height - 2
                        radius: 10
                        color: friendRow.chosen ? Theme.selected
                               : (friendArea.containsMouse || friendRow.activeFocus ? Theme.panelAlt : "transparent")
                        border.width: 1
                        border.color: friendRow.activeFocus ? Theme.primary : "transparent"
                        Behavior on color { ColorAnimation { duration: 130 } }
                    }
                    Avatar {
                        id: friendAvatar
                        x: root.compactIndex ? (parent.width - width) / 2 : 22
                        width: 36
                        height: 36
                        radius: 10
                        anchors.verticalCenter: parent.verticalCenter
                        source: model.icon
                        name: model.displayName
                    }
                    Column {
                        anchors.left: friendAvatar.right
                        anchors.leftMargin: 12
                        anchors.right: parent.right
                        anchors.rightMargin: 16
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 4
                        visible: !root.compactIndex

                        Text {
                            width: parent.width
                            text: model.displayName
                            textFormat: Text.PlainText
                            color: Theme.textPrimary
                            font.pixelSize: 14
                            font.bold: friendRow.chosen
                            elide: Text.ElideRight
                        }
                        //联系人摘要优先显示标签，其次个性签名；都没有就不占位
                        Text {
                            width: parent.width
                            visible: text.length > 0
                            text: model.tags.length > 0 ? model.tags.join(" · ") : model.desc
                            textFormat: Text.PlainText
                            color: Theme.textSecondary
                            font.pixelSize: 11
                            elide: Text.ElideRight
                            maximumLineCount: 1
                        }
                    }
                    MouseArea {
                        id: friendArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            friendRow.forceActiveFocus(Qt.MouseFocusReason)
                            friendRow.activate()
                        }
                    }

                    // 折叠态只有头像，名字用悬停提示补回来
                    ToolTip.visible: root.compactIndex && friendArea.containsMouse
                    ToolTip.text: model.displayName
                    ToolTip.delay: 350
                }
            }

            // 聊天列表没有会话时的轻提示；联系人空态由下面的引导卡承担
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                y: searchArea.y + searchArea.height + 54
                visible: bridge.mode === ChatBridge.ChatMode && root.chatCount === 0
                text: "还没有会话"
                font.pixelSize: 12
                color: Theme.textHint
            }

            // 没有好友也没有待处理申请时，才显示这张引导卡
            Rectangle {
                id: contactGuide
                visible: root.contactGuideVisible && !bridge.addFriendOpen
                x: 16
                y: searchArea.y + searchArea.height + 18
                width: parent.width - 32
                height: guideContent.implicitHeight + 36
                radius: 10
                color: Theme.panelAlt
                border.width: 1
                border.color: Theme.line

                Column {
                    id: guideContent
                    x: 18
                    y: 18
                    width: parent.width - 36
                    spacing: 10

                    Rectangle {
                        width: 36
                        height: 36
                        radius: 9
                        color: Theme.primarySoft
                        AppIcon {
                            anchors.centerIn: parent
                            name: "contacts"
                            size: 20
                            color: Theme.primary
                        }
                    }
                    Text {
                        text: "还没有联系人"
                        color: Theme.textPrimary
                        font.pixelSize: 15
                        font.weight: Font.DemiBold
                    }
                    Text {
                        width: parent.width
                        text: "搜索用户名或 ID，发送好友申请。"
                        color: Theme.textSecondary
                        font.pixelSize: 11
                        wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                    }
                    AppButton {
                        width: parent.width
                        height: 36
                        text: "添加好友"
                        onClicked: {
                            bridge.openAddFriend()
                            Qt.callLater(addSearch.forceInputFocus)
                        }
                    }
                }
            }

            Rectangle {
                anchors.right: parent.right
                width: 1
                height: parent.height
                color: Theme.line
            }
        }

        //设置栏与列表栏同宽，切换模式时左侧不跳动
        SettingsPanel {
            width: root.indexPanelWidth
            height: parent.height
            visible: bridge.mode === ChatBridge.SettingsMode
            controller: bridge
        }

        // 右侧内容区：按 pane 切换页面，实例常驻以保留输入草稿和滚动位置
        Item {
            id: contentPane
            width: parent.width - root.sidebarWidth - root.indexPanelWidth
            height: parent.height

            // 分隔条跨在第二栏右边缘上：拖动改宽度，双击恢复 280
            Item {
                id: splitter
                x: -3
                width: 6
                height: parent.height
                z: 100
                visible: !root.compactIndex

                Rectangle {
                    anchors.fill: parent
                    color: splitterArea.pressed || splitterArea.containsMouse
                        ? Qt.rgba(Theme.primary.r, Theme.primary.g, Theme.primary.b, 0.18)
                        : "transparent"
                    Behavior on color { ColorAnimation { duration: 120 } }
                }

                MouseArea {
                    id: splitterArea
                    anchors.fill: parent
                    cursorShape: Qt.SizeHorCursor
                    property real pressSceneX: 0
                    property int startWidth: 0
                    onPressed: (mouse) => {
                        pressSceneX = mapToItem(null, mouse.x, mouse.y).x
                        startWidth = root.indexWidth
                    }
                    onPositionChanged: (mouse) => {
                        if (!pressed)
                            return
                        const dx = mapToItem(null, mouse.x, mouse.y).x - pressSceneX
                        root.indexWidth = Math.round(Math.max(root.minIndexWidth,
                            Math.min(root.maxIndexWidth, startWidth + dx)))
                    }
                    onReleased: root.layoutPrefs.indexWidth = root.indexWidth
                    onDoubleClicked: {
                        root.indexWidth = Theme.indexWidth
                        root.layoutPrefs.indexWidth = Theme.indexWidth
                    }
                }
            }

            Column {
                anchors.centerIn: parent
                width: Math.min(360, parent.width - 80)
                spacing: 18
                visible: root.visiblePane === ChatBridge.EmptyPane
                PaperCompanion {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: 160
                    height: 112
                    compact: true
                }
                Text {
                    width: parent.width
                    text: root.emptyTitle
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                    color: Theme.textSecondary
                    font.pixelSize: 14
                }
                AppButton {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: 150
                    height: 38
                    visible: root.emptyAction.length > 0
                    text: root.emptyAction
                    onClicked: root.runEmptyAction()
                }
            }

            // 四个页面共用这块区域，只切 visible；聊天页负责提交，清空由外层在成功后调用
            ChatPage {
                id: chatPage
                anchors.fill: parent
                visible: root.visiblePane === ChatBridge.ChatPage
                bridge: bridge
                currentUid: bridge.currentUid
                currentThreadId: bridge.currentThreadId
                currentKind: bridge.currentKind
                groupInfo: bridge.groupInfo
                mentionCandidates: bridge.mentionCandidates
                title: bridge.currentName
                subtitle: bridge.currentSubtitle
                messages: bridge.messages
                blockedText: bridge.chatBlockedText
                onProfileRequested: bridge.openProfileFromChat(bridge.currentUid)
                onGroupInfoRequested: bridge.openGroupInfo()
                onMediaViewRequested: (items, index) => viewer.openPlaylist(items, index)
                onSendRequested: (parts) => {
                    if (bridge.sendParts(parts)) {
                        chatPage.clearInput()
                        chatPage.sent()
                    }
                }
            }
            FriendInfoPage {
                anchors.fill: parent
                visible: root.visiblePane === ChatBridge.FriendInfoPage
                profile: bridge.profile
                onChatRequested: bridge.openChat(bridge.profile.uid)
                onDeleteRequested: bridge.askDeleteFriend(bridge.profile.uid)
                onBackRequested: bridge.backFromProfile()
                onEditRelationRequested: profileDialogs.openRemark(bridge.profile.uid,
                                                                  bridge.profile.displayName,
                                                                  bridge.profile.name,
                                                                  bridge.profile.icon,
                                                                  bridge.profile.remark,
                                                                  bridge.profile.tags || [])
            }
            GroupInfoPage {
                anchors.fill: parent
                visible: root.visiblePane === ChatBridge.GroupInfoPage
                controller: bridge
                onBackRequested: bridge.backToChat()
            }
            ApplyFriendPage {
                anchors.fill: parent
                visible: root.visiblePane === ChatBridge.ApplyFriendPage
                applies: bridge.applies
                onAuthRequested: (uid, name, desc) => dialogs.openAuthDialog(uid, name, desc)
                onChatRequested: (uid) => bridge.openChat(uid)
            }
            UserInfoPage {
                id: userInfoPage
                anchors.fill: parent
                visible: root.visiblePane === ChatBridge.UserInfoPage
                controller: bridge
                onEditAvatarRequested: avatarEditor.chooseImage()
                onEditProfileRequested: (field) => profileDialogs.openSelf(field)
            }
            GeneralSettingsPage {
                anchors.fill: parent
                visible: root.visiblePane === ChatBridge.GeneralPage
                controller: bridge
            }
        }
    }

    // 聊天页加号的功能菜单：挂在加号下方，向右甩开，超出的部分落在内容区
    MouseArea {
        visible: root.functionMenuOpen && bridge.mode === ChatBridge.ChatMode
        x: root.sidebarWidth
        y: 0
        width: parent.width - root.sidebarWidth
        height: parent.height
        z: 28
        onClicked: root.functionMenuOpen = false
    }
    Rectangle {
        id: functionMenu
        visible: root.functionMenuOpen && bridge.mode === ChatBridge.ChatMode
        // 加号左边沿：面板宽减右边距和按钮宽，菜单从这里向右伸
        x: root.sidebarWidth + root.indexPanelWidth - 48
        y: 58
        width: 152
        height: 88
        radius: 10
        color: Theme.panelBg
        border.width: 1
        border.color: Theme.line
        z: 30

        Column {
            anchors.fill: parent
            anchors.margins: 2
            spacing: 2

            // 加好友：切到联系人并打开搜索浮层
            Rectangle {
                width: parent.width
                height: 40
                radius: 6
                color: menuArea.containsMouse || activeFocus ? Theme.selected : "transparent"
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: "加好友"
                Accessible.onPressAction: root.openAddFriendFromChat()
                Keys.onSpacePressed: root.openAddFriendFromChat()
                Keys.onReturnPressed: root.openAddFriendFromChat()
                Row {
                    x: 12
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 9
                    AppIcon { name: "user"; size: 16; color: Theme.primary }
                    Text { text: "加好友"; color: Theme.textPrimary; font.pixelSize: 12 }
                }
                MouseArea {
                    id: menuArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.openAddFriendFromChat()
                }
            }

            // 发起群聊：勾选好友后直接进入新群
            Rectangle {
                id: groupMenu
                width: parent.width
                height: 40
                radius: 6
                color: groupMenuArea.containsMouse || groupMenu.activeFocus
                    ? Theme.selected : "transparent"
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: "发起群聊"
                Accessible.onPressAction: root.openGroupCreator()
                Keys.onSpacePressed: root.openGroupCreator()
                Keys.onReturnPressed: root.openGroupCreator()
                Row {
                    x: 12
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 9
                    AppIcon { name: "contacts"; size: 16; color: Theme.primary }
                    Text { text: "发起群聊"; color: Theme.textPrimary; font.pixelSize: 12 }
                }
                MouseArea {
                    id: groupMenuArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.openGroupCreator()
                }
            }
        }
    }

    // 发起群聊：Popup 自己处理模态与焦点，打开时底层 Row 由 dialogs 之外单独禁用
    CreateGroupDialog {
        id: createGroupDialog
        controller: bridge
        visible: bridge.createGroupOpen
    }

    ForwardDialog {
        id: forwardDialog
        controller: bridge
        open: chatPage.visible && chatPage.forwardOpen
        count: chatPage.selectedCount
        mergeMode: chatPage.mergeMode
        onTargetChosen: (threadId, kind, uid) => chatPage.chooseForwardTarget(threadId, kind, uid)
        onCloseRequested: chatPage.forwardOpen = false
    }

    // 好友表单是覆盖层，打开时禁用底层 Row；头像编辑器独立于页面，避免切页被销毁
    FriendDialogs {
        id: dialogs
        anchors.fill: parent
        controller: bridge
    }

    // 资料编辑与备注编辑共用一套弹层，和好友表单一样覆盖整个聊天区
    ProfileDialogs {
        id: profileDialogs
        anchors.fill: parent
        controller: bridge
    }

    AvatarEditor {
        id: avatarEditor
        onAvatarSaved: userInfoPage.avatarSaved = true
    }

    //图片/视频查看器覆盖整个聊天区，打开时压在页面与弹层之上
    ViewerOverlay {
        id: viewer
        anchors.fill: parent
    }

    // 会话整理菜单：置顶、免打扰、标记未读、清除记录、删除会话。
    // 发消息由点击会话完成，删除好友只在资料页出现
    ContextMenu {
        id: friendMenu
        anchors.fill: parent
    }

    // 清除聊天记录确认：只清本机，文案里要说清楚，避免以为对方也看不到了
    Rectangle {
        anchors.fill: parent
        visible: bridge.clearConfirmVisible
        color: Theme.overlay
        z: 30

        MouseArea {
            anchors.fill: parent
            onClicked: bridge.cancelClearHistory()
        }

        Rectangle {
            anchors.centerIn: parent
            width: Math.min(420, parent.width - 64)
            implicitHeight: clearContent.implicitHeight + 48
            radius: Theme.dialogRadius
            color: Theme.surfaceFloating
            border.width: 1
            border.color: Theme.line

            Column {
                id: clearContent
                x: 24
                y: 24
                width: parent.width - 48
                spacing: 14

                Text {
                    text: bridge.clearConfirmDelete ? "删除会话" : "清除聊天记录"
                    color: Theme.textPrimary
                    font.pixelSize: 18
                    font.bold: true
                }
                Text {
                    width: parent.width
                    text: bridge.clearConfirmDelete
                        ? "删除与「" + bridge.clearConfirmTitle + "」的本机会话与聊天记录？"
                          + "好友关系和服务端记录都会保留，收到新消息时这个会话会重新出现。"
                        : "清除与「" + bridge.clearConfirmTitle + "」的本地聊天记录？"
                          + "只删除这台设备上的内容，服务器与对方的记录不受影响。"
                    color: Theme.textSecondary
                    font.pixelSize: 12
                    wrapMode: Text.WordWrap
                }
                Row {
                    width: parent.width
                    spacing: 12

                    CodeButton {
                        width: (parent.width - 12) / 2
                        height: 42
                        text: "取消"
                        onClicked: bridge.cancelClearHistory()
                    }
                    AppButton {
                        width: (parent.width - 12) / 2
                        height: 42
                        variant: "danger"
                        text: bridge.clearConfirmDelete ? "删除会话" : "清除聊天记录"
                        onClicked: bridge.confirmClearHistory()
                    }
                }
            }
        }
    }

    // 删除好友确认：资料页与联系人菜单共用同一层，避免各页面各写一份
    Rectangle {
        id: deleteConfirm
        anchors.fill: parent
        visible: bridge.deleteConfirmVisible
        color: Theme.overlay
        z: 30

        MouseArea {
            anchors.fill: parent
            onClicked: bridge.cancelDeleteFriend()
        }

        Rectangle {
            anchors.centerIn: parent
            width: Math.min(420, parent.width - 64)
            implicitHeight: deleteContent.implicitHeight + 48
            radius: Theme.dialogRadius
            color: Theme.surfaceFloating
            border.width: 1
            border.color: Theme.line

            Column {
                id: deleteContent
                x: 24
                y: 24
                width: parent.width - 48
                spacing: 14

                Text {
                    text: "删除好友"
                    color: Theme.textPrimary
                    font.pixelSize: 18
                    font.bold: true
                }
                Text {
                    width: parent.width
                    text: "删除「" + bridge.deleteTargetName + "」？对方不会收到通知，"
                        + "你们的历史消息仍会保留，但不能再发消息。"
                    color: Theme.textSecondary
                    font.pixelSize: 12
                    wrapMode: Text.WordWrap
                }
                Row {
                    width: parent.width
                    spacing: 12

                    CodeButton {
                        width: (parent.width - 12) / 2
                        height: 42
                        text: "取消"
                        onClicked: bridge.cancelDeleteFriend()
                    }
                    AppButton {
                        width: (parent.width - 12) / 2
                        height: 42
                        variant: "danger"
                        text: "删除好友"
                        onClicked: bridge.confirmDeleteFriend()
                    }
                }
            }
        }
    }

    // 退出登录确认：设置页里点一下不会立刻断线，避免误触丢失当前上下文
    Rectangle {
        anchors.fill: parent
        visible: bridge.logoutConfirmVisible
        color: Theme.overlay
        z: 40

        MouseArea {
            anchors.fill: parent
            onClicked: bridge.cancelLogout()
        }

        Rectangle {
            anchors.centerIn: parent
            width: Math.min(420, parent.width - 64)
            implicitHeight: logoutContent.implicitHeight + 48
            radius: Theme.dialogRadius
            color: Theme.surfaceFloating
            border.width: 1
            border.color: Theme.line

            Column {
                id: logoutContent
                x: 24
                y: 24
                width: parent.width - 48
                spacing: 14

                Text {
                    text: "退出登录"
                    color: Theme.textPrimary
                    font.pixelSize: 18
                    font.bold: true
                }
                Text {
                    width: parent.width
                    text: "退出后会断开与服务器的连接。本机聊天记录与草稿会保留，"
                        + "正在编辑但还没发送的附件不会保留。"
                    color: Theme.textSecondary
                    font.pixelSize: 12
                    wrapMode: Text.WordWrap
                }
                Row {
                    width: parent.width
                    spacing: 12

                    CodeButton {
                        width: (parent.width - 12) / 2
                        height: 42
                        text: "取消"
                        onClicked: bridge.cancelLogout()
                    }
                    AppButton {
                        width: (parent.width - 12) / 2
                        height: 42
                        text: "退出登录"
                        onClicked: bridge.confirmLogout()
                    }
                }
            }
        }
    }
}
