import QtQuick
import QtCore
import QtQuick.Controls
import YunJian

// 聊天工作区：一级导航、索引列表和右侧内容共用一套桌面节奏。
// 这里只负责布局与焦点，导航状态和业务动作都交给 ChatBridge。
Item {
    id: root
    onEnabledChanged: if (!enabled) avatarEditor.dismiss()

    //系统通知点击时由 Main.qml 调进来，避免再复制一份导航状态
    readonly property var chatBridge: bridge

    readonly property int sidebarWidth: Theme.sidebarWidth

    // 第二栏宽度可拖动：200–420，双击分隔条恢复 280；只在拖动结束时落盘
    property int indexWidth: Theme.indexWidth
    readonly property int minIndexWidth: 200
    // 列表最宽 420；窗口再宽也把空间留给正文，避免主次倒置
    readonly property int maxIndexWidth: Math.min(420,
        Math.max(minIndexWidth, width - sidebarWidth - 380))
    property QtObject layoutPrefs: Settings {
        category: "Layout"
        property int indexWidth: Theme.indexWidth
    }

    // 窄窗口把第二栏收成头像条：聊天、联系人、设置三处一致。
    // compactMode：-1 跟随窗口宽度，0 强制展开，1 强制收起。
    property int compactMode: -1
    // 拖分隔条时宽度直接跟着指针走：176 以下继续收到头像条，不再卡住等松手
    property bool indexDragging: false
    property int dragPanelWidth: 0
    property real indexDragStartX: 0
    property int indexDragStartWidth: 0
    property bool indexDragMoved: false
    property bool indexDragFromCompact: false
    readonly property bool compactIndex: indexDragging
        ? dragPanelWidth <= 176
        : (compactMode === 1 || (compactMode === -1 && width < 780))
    readonly property int indexPanelWidth: indexDragging
        ? dragPanelWidth
        : (compactIndex ? Theme.compactIndexWidth : Math.min(indexWidth, maxIndexWidth))

    // 只有点击把手/跨阈值才做宽度动画；拖动时 duration 为 0，保证跟手
    property bool indexAnimating: false
    property real indexPanelAnimatedWidth: indexPanelWidth
    Behavior on indexPanelAnimatedWidth {
        NumberAnimation {
            duration: root.indexAnimating ? 200 : 0
            easing.type: Easing.OutCubic
        }
    }
    Timer {
        id: indexAnimTimer
        interval: 240
        onTriggered: root.indexAnimating = false
    }
    Timer {
        id: compactAddTimer
        interval: 280
        onTriggered: bridge.openAddFriend()
    }
    function toggleIndex() {
        indexDragging = false
        root.indexAnimating = true
        // 宽窗展开后回到“跟随窗口”，之后再拉窄仍会自动收起；窄窗里展开则固定为展开
        compactMode = compactIndex ? (width < 780 ? 0 : -1) : 1
        indexAnimTimer.restart()
    }
    // 分隔条与把手共用一套拖动：从 60 的头像条也能直接拖出来
    function beginIndexDrag(sceneX) {
        indexDragStartX = sceneX
        indexDragStartWidth = indexPanelWidth
        indexDragFromCompact = compactIndex
        dragPanelWidth = indexDragStartWidth
        indexDragMoved = false
        indexAnimating = false
        indexDragging = true
    }
    function updateIndexDrag(sceneX) {
        if (!indexDragging)
            return
        const dx = sceneX - indexDragStartX
        if (Math.abs(dx) > 1)
            indexDragMoved = true
        dragPanelWidth = Math.round(Math.max(Theme.compactIndexWidth,
            Math.min(maxIndexWidth, indexDragStartWidth + dx)))
    }
    function endIndexDrag() {
        if (!indexDragging)
            return
        const finalWidth = dragPanelWidth
        // 从头像条拖出 24px 以上就当作“要展开”，松手补到最小列表宽度
        const grewOut = indexDragFromCompact && finalWidth >= indexDragStartWidth + 24
        const expanding = finalWidth > 176 || grewOut
        indexAnimating = true
        indexAnimTimer.restart()
        if (expanding) {
            compactMode = width < 780 ? 0 : -1
            indexWidth = Math.max(minIndexWidth, finalWidth)
            layoutPrefs.indexWidth = indexWidth
        } else {
            compactMode = 1
        }
        indexDragging = false
    }
    // 拖窗口跨过阈值时也给一次短动画；拖动分隔条时由指针直接决定宽度
    onCompactIndexChanged: {
        if (compactIndex) {
            // 收起后没有搜索框，浮层要一起收掉，避免挂在头像条上
            bridge.closeAddFriend()
            root.closeFunctionMenu()
            tagFilterOpen = false
        }
        root.rebuildGroupRows()
        if (root.indexDragging)
            return
        root.indexAnimating = true
        indexAnimTimer.restart()
    }


    // 搜索快捷键：聊天与联系人共用一套入口，设置页不响应
    Shortcut {
        sequences: [StandardKey.Find]
        enabled: root.enabled && !root.modalOpen && !root.compactIndex && (bridge.mode === ChatBridge.ContactMode
            || (bridge.mode === ChatBridge.ChatMode && root.visiblePane !== ChatBridge.ChatPage))
        onActivated: {
            if (bridge.mode === ChatBridge.ChatMode)
                chatSearch.forceInputFocus()
            else if (root.groupTab)
                groupSearch.forceInputFocus()
            else
                addSearch.forceInputFocus()
        }
    }

    readonly property int listHeaderHeight: Theme.toolbarHeight

    readonly property int friendCount: bridge.friends.totalCount
    readonly property int visibleFriendCount: bridge.friends.count
    readonly property int pendingCount: bridge.applies.pendingCount
    readonly property int chatCount: bridge.conversations.activeCount
    // 有好友或有申请时，列表顶部保留「新的朋友」入口
    readonly property bool newFriendEntryVisible: bridge.mode === ChatBridge.ContactMode
        && contactTab === 0 && (friendCount > 0 || pendingCount > 0) && !compactIndex
    // 标签条只在联系人模式且有标签时出现；筛选只影响列表，不影响入口与空态判断
    readonly property bool tagBarVisible: bridge.mode === ChatBridge.ContactMode
        && contactTab === 0 && bridge.tagOptions.length > 0 && !compactIndex
    readonly property bool tagEmptyVisible: tagBarVisible && bridge.tagFilter.length > 0
        && visibleFriendCount === 0
    property bool tagFilterOpen: false //标签「更多」浮层是否展开
    // 联系人页两个页签：0 好友 / 1 群聊。群聊只做本地筛选，不触发服务端搜索
    property int contactTab: 0
    readonly property bool groupTab: contactTab === 1
    readonly property int groupTotal: bridge.groups.totalCount

    // 任一模态浮层打开时，主内容不接收拖拽与快捷键
    readonly property bool modalOpen: dialogs.visible || createGroupDialog.visible
        || profileDialogs.visible || viewer.visible
        || bridge.clearConfirmVisible || bridge.deleteConfirmVisible || bridge.logoutConfirmVisible
        || forwardDialog.visible

    // 会话搜索无结果时，带关键词去联系人搜索
    function openContactSearchFor(keyword) {
        const q = String(keyword || "").trim()
        if (q.length === 0)
            return
        bridge.showContactMode()
        bridge.openAddFriend()
        bridge.searchText = q
    }

    function clearConversationFilter() {
        chatSearch.setText("")
        bridge.filteredConversations.filterText = ""
        conversationList.currentIndex = -1
    }

    function openFirstFilteredConversation() {
        if (bridge.filteredConversations.count <= 0)
            return
        conversationList.currentIndex = 0
        conversationList.positionViewAtIndex(0, ListView.Contain)
        Qt.callLater(function() {
            const item = conversationList.currentItem
            if (!item)
                return
            item.forceActiveFocus()
            item.activate()
        })
    }

    // 会话列表：↑/↓ 逐条切换并直接打开，Esc 回输入框
    function stepConversation(delta) {
        const count = bridge.filteredConversations.count
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
    // 群聊列表与好友列表同一套键盘规则：↑/↓ 移动焦点，Enter 打开资料
    // 模型里混了分组标题行，跳行时只停在真正的群上
    function stepGroup(delta) {
        const total = groupRowsModel.count
        if (total <= 0)
            return
        let next = groupsList.currentIndex
        if (next < 0)
            next = delta > 0 ? -1 : total
        do {
            next += delta
        } while (next >= 0 && next < total && groupRowsModel.get(next).header)
        if (next < 0 || next >= total)
            return
        groupsList.currentIndex = next
        groupsList.positionViewAtIndex(next, ListView.Contain)
        Qt.callLater(function() {
            if (groupsList.currentItem)
                groupsList.currentItem.forceActiveFocus()
        })
    }

    // 群列表按“我管理的（群主/管理员）/ 我加入的”分两段，段标题可收起。
    // 用一份轻量 ListModel 拼出标题行 + 群行：群数量很少，整表重建比维护索引简单。
    property bool managedGroupsCollapsed: false
    property bool joinedGroupsCollapsed: false
    ListModel { id: groupRowsModel }

    function rebuildGroupRows() {
        groupRowsModel.clear()
        if (!bridge || !bridge.groups)
            return
        const managed = []
        const joined = []
        const count = bridge.groups.count
        for (let i = 0; i < count; ++i) {
            const row = bridge.groups.get(i)
            if (!row || row.name === undefined)
                continue
            const entry = {
                header: false,
                title: "",
                sectionCount: 0,
                sectionKey: "",
                collapsed: false,
                threadId: row.threadId,
                name: row.name,
                icon: row.icon,
                memberCount: row.memberCount,
                role: row.role,
                muted: row.muted
            }
            if (row.role >= 1)
                managed.push(entry)
            else
                joined.push(entry)
        }

        // 头像条放不下标题，直接平铺，顺序仍是“管理的在前”
        if (root.compactIndex) {
            for (let i = 0; i < managed.length; ++i)
                groupRowsModel.append(managed[i])
            for (let i = 0; i < joined.length; ++i)
                groupRowsModel.append(joined[i])
            groupsList.currentIndex = -1
            return
        }

        function addSection(key, title, entries, collapsed) {
            if (entries.length === 0)
                return
            groupRowsModel.append({
                header: true,
                title: title,
                sectionCount: entries.length,
                sectionKey: key,
                collapsed: collapsed,
                threadId: 0,
                name: "",
                icon: "",
                memberCount: 0,
                role: 0,
                muted: false
            })
            if (collapsed)
                return
            for (let i = 0; i < entries.length; ++i)
                groupRowsModel.append(entries[i])
        }

        addSection("managed", "我管理的群", managed, root.managedGroupsCollapsed)
        addSection("joined", "我加入的群", joined, root.joinedGroupsCollapsed)
        groupsList.currentIndex = -1
    }

    function toggleGroupSection(key) {
        if (key === "managed")
            root.managedGroupsCollapsed = !root.managedGroupsCollapsed
        else
            root.joinedGroupsCollapsed = !root.joinedGroupsCollapsed
        root.rebuildGroupRows()
    }

    Connections {
        target: bridge ? bridge.groups : null
        function onCountChanged() { root.rebuildGroupRows() }
        function onDataChanged() { root.rebuildGroupRows() }
        function onModelReset() { root.rebuildGroupRows() }
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
        hostWindow.title = count > 0 ? "(" + (count > 99 ? "99+" : count) + ") 云笺" : "云笺"
    }
    onUnreadTotalChanged: refreshWindowTitle()
    onHostWindowChanged: if (hostWindow) bridge.setWindowActive(hostWindow.active)
    Component.onCompleted: {
        refreshWindowTitle()
        if (hostWindow)
            bridge.setWindowActive(hostWindow.active)
        // 列表栏宽度取本机值，夹在允许范围内
        root.indexWidth = Math.max(root.minIndexWidth, Math.min(420, layoutPrefs.indexWidth || Theme.indexWidth))
        root.rebuildGroupRows()
    }
    Component.onDestruction: if (hostWindow) hostWindow.title = "云笺"
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
            if (pane === ChatBridge.ChatPage
                || pane === ChatBridge.UserInfoPage || pane === ChatBridge.GeneralPage)
                return ChatBridge.EmptyPane
            if (pane === ChatBridge.GroupInfoPage && bridge.currentThreadId <= 0)
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
            if (root.groupTab)
                return root.groupTotal === 0 ? "还没有群聊" : "选择一个群聊，查看资料"
            if (friendCount === 0)
                return pendingCount > 0 ? "有 " + pendingCount + " 条好友申请待处理"
                                        : "还没有联系人"
            return pendingCount > 0 ? "有 " + pendingCount + " 条好友申请待处理"
                                    : "选择一位联系人，查看资料"
        }
        return chatCount === 0 ? "还没有会话" : "选择一个会话"
    }
    readonly property string emptyBody: {
        if (bridge.mode === ChatBridge.SettingsMode)
            return "账号、外观与本地数据都在这里。"
        if (bridge.mode === ChatBridge.ContactMode) {
            if (root.groupTab)
                return root.groupTotal === 0
                    ? "从联系人里选几个人，发起一个群聊。"
                    : "在左侧选择一个群，查看成员与设置。"
            if (friendCount === 0)
                return pendingCount > 0 ? "同意申请后，就能开始聊天。"
                                        : "搜索用户名或 ID，寄出第一封好友申请。"
            return pendingCount > 0 ? "先处理好友申请，再开始聊天。"
                                    : "备注、标签与个性签名都在资料页。"
        }
        if (chatCount === 0)
            return friendCount === 0 ? "先加一位联系人，再开始第一段对话。"
                                     : "在联系人里选一位，开始第一段对话。"
        return "点开会话，接着上次的写。"
    }
    readonly property string emptyAction: {
        if (bridge.mode === ChatBridge.SettingsMode)
            return ""
        if (bridge.mode === ChatBridge.ContactMode) {
            if (root.groupTab)
                return root.groupTotal === 0 ? "发起群聊" : ""
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

    // 关闭加号菜单：菜单本身负责收起，根属性只同步状态（提示、图标、悬停）
    function closeFunctionMenu() {
        if (typeof functionMenu !== "undefined" && functionMenu.visible)
            functionMenu.close()
        functionMenuOpen = false
    }

    function handlePlus() {
        if (bridge.mode === ChatBridge.ChatMode) {
            if (functionMenu.visible) {
                functionMenu.close()
                return
            }
            // 与消息右键同一套骨架：6px 上下留白、36 行高、16 图标槽
            // 加号在展开时贴右缘、收起时居中，菜单左沿取按钮左沿，两种宽度下都对齐
            const plusX = root.compactIndex
                ? root.sidebarWidth + (root.indexPanelWidth - 32) / 2
                : root.sidebarWidth + root.indexPanelWidth - 32 - 18
            functionMenu.open(plusX, 58, [
                { text: "添加好友", icon: "user", action: () => root.openAddFriendFromChat() },
                { text: "发起群聊", icon: "contacts", action: () => root.openGroupCreator() }
            ])
            functionMenuOpen = true
            return
        }
        if (root.groupTab) {
            bridge.openCreateGroup()
            return
        }
        // 头像条里没有搜索框：先展开列表，再打开加好友浮层
        if (root.compactIndex) {
            root.toggleIndex()
            compactAddTimer.restart()
            return
        }
        if (bridge.addFriendOpen) {
            bridge.closeAddFriend()
            return
        }
        bridge.openAddFriend()
        Qt.callLater(addSearch.forceInputFocus)
    }

    function switchContactTab(index) {
        if (root.contactTab === index)
            return
        root.contactTab = index
        root.tagFilterOpen = false
        root.closeFunctionMenu()
        bridge.closeAddFriend()
        bridge.clearSearch()
        bridge.groups.setFilter("")
        if (typeof addSearch !== "undefined")
            addSearch.setText("")
        if (typeof groupSearch !== "undefined")
            groupSearch.setText("")
    }

    function openAddFriendFromChat() {
        root.closeFunctionMenu()
        bridge.showContactMode()
        bridge.openAddFriend()
        Qt.callLater(addSearch.forceInputFocus)
    }

    // 发起群聊：先收起功能菜单，再交给桥上打开弹层。
    // 必须定义在根对象上：MouseArea 的 onClicked 处于自己的作用域，
    // 直接写 openGroupCreator() 会在运行时报 ReferenceError（父项里的函数不在作用域链上）
    function openGroupCreator() {
        root.closeFunctionMenu()
        bridge.openCreateGroup()
    }

    function runEmptyAction() {
        if (bridge.mode === ChatBridge.ContactMode) {
            if (root.groupTab) {
                bridge.openCreateGroup()
                return
            }
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
        function onModeChanged() {
            root.closeFunctionMenu()
            root.clearConversationFilter()
            if (bridge.mode !== ChatBridge.ContactMode)
                bridge.groups.setFilter("")
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
        color: Theme.sheet
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

        // 一级导航：整块墨色书脊；顶部只放带标签的两个入口，品牌不在这里重复。
        Rectangle {
            width: root.sidebarWidth
            height: parent.height
            color: Theme.spine

            // 书脊顶端垂下一小段书签：占住左上角，和云笺的气质一致，又不写任何文字
            SpineRibbon {
                anchors.horizontalCenter: parent.horizontalCenter
                y: 0
                width: 16
                height: 38
            }

            // 两个一级入口带文字标签；顶部不放品牌图标，避免被读成应用 logo
            Column {
                y: 60
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 8
                SideButton {
                    kind: "chat"
                    showLabel: true
                    active: bridge.mode === ChatBridge.ChatMode
                    badge: bridge.unreadTotal
                    onClicked: bridge.showChatMode()
                }
                SideButton {
                    kind: "contact"
                    showLabel: true
                    active: bridge.mode === ChatBridge.ContactMode
                    badge: bridge.applies.pendingCount
                    onClicked: bridge.showContactMode()
                }
            }

            SideButton {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: ownAvatar.top
                anchors.bottomMargin: 12
                kind: "settings"
                showLabel: true
                active: bridge.mode === ChatBridge.SettingsMode
                onClicked: bridge.showSettings()
            }

            // 本人头像：点击直达个人资料，不再先落到设置索引
            Avatar {
                id: ownAvatar
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 32
                anchors.horizontalCenter: parent.horizontalCenter
                width: 38
                height: 38
                radius: 10
                source: bridge.myIcon
                name: bridge.myName
                border.width: activeFocus ? 2 : 1
                border.color: activeFocus ? Theme.spineText : Theme.spineMuted
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: "打开个人资料"
                Accessible.onPressAction: bridge.openMyProfile()
                Keys.onSpacePressed: bridge.openMyProfile()
                Keys.onReturnPressed: bridge.openMyProfile()
                MouseArea {
                    id: ownArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: bridge.openMyProfile()
                }
                AppToolTip {
                    text: "个人资料"
                    hovered: ownArea.containsMouse
                }
            }

            // 深色下书脊与索引都很暗，靠这条 1px 棱线保住“书脊”的边界；浅色为透明
            Rectangle {
                anchors.right: parent.right
                width: 1
                height: parent.height
                color: Theme.spineEdge
            }
        }

        // 第二栏：标题、搜索框与列表。设置模式下整栏让位给 SettingsPanel
        Rectangle {
            id: indexPanel
            visible: bridge.mode !== ChatBridge.SettingsMode
            width: root.indexPanelAnimatedWidth
            clip: true
            height: parent.height
            color: Theme.indexBg

            Item {
                id: listHeader
                width: parent.width
                height: root.listHeaderHeight

                // 标题与统计随模式切换；加号在聊天页打开功能菜单，在联系人页打开加好友浮层
                Text {
                    x: 20
                    y: 10
                    opacity: root.compactIndex ? 0 : 1
                    Behavior on opacity { NumberAnimation { duration: root.indexDragging ? 0 : 120 } }
                    text: bridge.mode === ChatBridge.ChatMode ? "聊天" : "联系人"
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontDisplay
                    font.weight: Font.DemiBold
                }
                Text {
                    x: 20
                    y: 39
                    opacity: root.compactIndex ? 0 : 1
                    Behavior on opacity { NumberAnimation { duration: root.indexDragging ? 0 : 120 } }
                    text: bridge.mode === ChatBridge.ChatMode
                        ? (chatSearch.text.trim().length > 0
                            ? bridge.filteredConversations.count : root.chatCount) + " 个会话"
                        : root.groupTab ? root.groupTotal + " 个群聊"
                                        : bridge.friends.count + " 位好友"
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSmall
                }

                Rectangle {
                    // 展开时贴右缘；收起成头像条时居中，别在 72 的条里偏到一边
                    x: root.compactIndex ? (parent.width - width) / 2 : parent.width - width - 18
                    anchors.verticalCenter: parent.verticalCenter
                    width: 32
                    height: 32
                    radius: Theme.controlRadius
                    color: searchEntry.containsMouse || activeFocus ? Theme.selected : Theme.panelBg
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: bridge.mode === ChatBridge.ChatMode ? "更多功能"
                        : root.groupTab ? "发起群聊" : "添加好友"
                    Accessible.onPressAction: root.handlePlus()
                    Keys.onSpacePressed: root.handlePlus()
                    Keys.onReturnPressed: root.handlePlus()
                    AppIcon {
                        anchors.centerIn: parent
                        name: "plus"
                        size: 17
                        color: Theme.primary
                    }
                    MouseArea {
                        id: searchEntry
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.handlePlus()
                    }

                    AppToolTip {
                        text: bridge.mode === ChatBridge.ChatMode ? "更多功能"
                            : root.groupTab ? "发起群聊" : "添加好友"
                        // 功能菜单就贴在加号下方，菜单开着时收起提示，避免盖住第一项
                        hovered: searchEntry.containsMouse && !root.functionMenuOpen
                    }
                }
            }

            // 联系人页签：好友 / 群聊。下划线式，和标签筛选保持同一种语言
            Item {
                id: contactTabs
                visible: bridge.mode === ChatBridge.ContactMode
                enabled: !root.compactIndex
                opacity: root.compactIndex ? 0 : 1
                Behavior on opacity { NumberAnimation { duration: root.indexDragging ? 0 : 120 } }
                x: 20
                y: listHeader.height + 2
                width: parent.width - 40
                height: 32

                Row {
                    width: parent.width
                    height: parent.height
                    spacing: 22

                    Item {
                        id: friendTabItem
                        readonly property bool chosen: root.contactTab === 0
                        width: friendTabLabel.implicitWidth
                        height: parent.height
                        Text {
                            id: friendTabLabel
                            anchors.centerIn: parent
                            text: "好友"
                            color: friendTabItem.chosen ? Theme.textPrimary : Theme.textSecondary
                            font.pixelSize: Theme.fontSmall
                            font.weight: friendTabItem.chosen ? Font.Medium : Font.Normal
                        }
                        Rectangle {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: 2
                            color: friendTabItem.chosen ? Theme.primary : "transparent"
                        }
                        MouseArea {
                            anchors.fill: parent
                            anchors.topMargin: -4
                            anchors.bottomMargin: -4
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.switchContactTab(0)
                        }
                    }

                    Item {
                        id: groupTabItem
                        readonly property bool chosen: root.contactTab === 1
                        width: groupTabLabel.implicitWidth
                        height: parent.height
                        Text {
                            id: groupTabLabel
                            anchors.centerIn: parent
                            text: "群聊"
                            color: groupTabItem.chosen ? Theme.textPrimary : Theme.textSecondary
                            font.pixelSize: Theme.fontSmall
                            font.weight: groupTabItem.chosen ? Font.Medium : Font.Normal
                        }
                        Rectangle {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: 2
                            color: groupTabItem.chosen ? Theme.primary : "transparent"
                        }
                        MouseArea {
                            anchors.fill: parent
                            anchors.topMargin: -4
                            anchors.bottomMargin: -4
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.switchContactTab(1)
                        }
                    }
                }
            }

            // 折叠态用两枚图标保留好友 / 群聊切换
            Item {
                id: compactContactTabs
                visible: root.compactIndex && bridge.mode === ChatBridge.ContactMode
                x: 0
                y: listHeader.height + 2
                width: root.indexPanelWidth
                height: 28

                Row {
                    anchors.centerIn: parent
                    spacing: 4

                    Rectangle {
                        width: 30
                        height: 30
                        radius: Theme.controlRadius
                        color: root.contactTab === 0 ? Theme.panelBg
                             : (friendTabCompactArea.containsMouse ? Theme.panelAlt : "transparent")
                        border.width: root.contactTab === 0 ? 1 : 0
                        border.color: Theme.line
                        AppIcon {
                            anchors.centerIn: parent
                            name: "user"
                            size: 15
                            color: root.contactTab === 0 ? Theme.primary : Theme.textSecondary
                        }
                        MouseArea {
                            id: friendTabCompactArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.switchContactTab(0)
                        }
                        AppToolTip {
                            text: "好友"
                            hovered: friendTabCompactArea.containsMouse
                        }
                    }

                    Rectangle {
                        width: 30
                        height: 30
                        radius: Theme.controlRadius
                        color: root.contactTab === 1 ? Theme.panelBg
                             : (groupTabCompactArea.containsMouse ? Theme.panelAlt : "transparent")
                        border.width: root.contactTab === 1 ? 1 : 0
                        border.color: Theme.line
                        AppIcon {
                            anchors.centerIn: parent
                            name: "group"
                            size: 15
                            color: root.contactTab === 1 ? Theme.primary : Theme.textSecondary
                        }
                        MouseArea {
                            id: groupTabCompactArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.switchContactTab(1)
                        }
                        AppToolTip {
                            text: "群聊"
                            hovered: groupTabCompactArea.containsMouse
                        }
                    }
                }
            }

            // 两种模式共用同一个输入位置：聊天页筛会话，联系人页按页签搜索
            Item {
                id: searchArea
                x: 20
                y: listHeader.height + (bridge.mode === ChatBridge.ContactMode ? 44 : 6)
                width: parent.width - 40
                height: 36
                enabled: !root.compactIndex
                opacity: root.compactIndex ? 0 : 1
                Behavior on opacity { NumberAnimation { duration: root.indexDragging ? 0 : 120 } }

                SearchField {
                    id: chatSearch
                    anchors.fill: parent
                    visible: bridge.mode === ChatBridge.ChatMode
                    placeholder: "搜索会话"
                    inputObjectName: "chatSearchInput"
                    onTextEdited: (value) => {
                        bridge.filteredConversations.filterText = value
                        conversationList.currentIndex = -1
                    }
                    onAccepted: root.openFirstFilteredConversation()
                    onEscapePressed: root.clearConversationFilter()
                    onClearRequested: root.clearConversationFilter()
                    onFocusGained: root.closeFunctionMenu()
                }
                SearchField {
                    id: addSearch
                    anchors.fill: parent
                    visible: bridge.mode === ChatBridge.ContactMode && !root.groupTab
                    placeholder: "搜索用户名或 ID"
                    inputObjectName: "searchInput"
                    onTextEdited: (value) => bridge.searchText = value
                    onAccepted: bridge.search()
                    onEscapePressed: bridge.closeAddFriend()
                    onClearRequested: bridge.clearSearch()
                    onFocusGained: bridge.openAddFriend()
                }
                SearchField {
                    id: groupSearch
                    anchors.fill: parent
                    visible: bridge.mode === ChatBridge.ContactMode && root.groupTab
                    placeholder: "搜索群聊"
                    inputObjectName: "groupSearchInput"
                    onTextEdited: (value) => bridge.groups.setFilter(value)
                    onEscapePressed: {
                        groupSearch.setText("")
                        bridge.groups.setFilter("")
                    }
                    onClearRequested: {
                        groupSearch.setText("")
                        bridge.groups.setFilter("")
                    }
                }
            }

            // 联系人页加好友浮层，搜索框仍是上面的输入框
            AddFriendPopover {
                id: addFriendPopover
                visible: bridge.addFriendOpen && !root.groupTab
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
                visible: bridge.addFriendOpen && !root.groupTab
                anchors.top: searchArea.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                z: 20
                onClicked: bridge.closeAddFriend()
            }
            // 联系人页的标签筛选：下划线式标签，像信纸边缘的索引签，不再用实心胶囊。
            Item {
                id: contactTagBar
                visible: root.tagBarVisible
                x: 20
                y: searchArea.y + searchArea.height + 10
                width: parent.width - 40
                height: visible ? 30 : 0

                // 用文字度量估每个标签的宽度，决定这一行能放几个，避免依赖滚动。
                FontMetrics {
                    id: tagFont
                    font.pixelSize: Theme.fontSmall
                }

                readonly property int gap: 18
                readonly property int moreWidth: 40
                function chipWidth(text) {
                    return Math.max(28, Math.ceil(tagFont.advanceWidth(text)))
                }
                function countVisibleTags() {
                    const options = bridge.tagOptions
                    let used = chipWidth("全部") + gap
                    let count = 0
                    for (let i = 0; i < options.length; ++i) {
                        const width = chipWidth(options[i]) + gap
                        const reserve = (i < options.length - 1) ? moreWidth + gap : 0
                        if (used + width + reserve > contactTagBar.width)
                            break
                        used += width
                        ++count
                    }
                    return count
                }

                readonly property var shownTags: {
                    const list = bridge.tagOptions.slice(0, countVisibleTags())
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
                    spacing: contactTagBar.gap

                    Item {
                        id: allChip
                        readonly property bool chosen: bridge.tagFilter.length === 0
                        width: allLabel.implicitWidth
                        height: 30
                        anchors.verticalCenter: parent.verticalCenter
                        Text {
                            id: allLabel
                            anchors.centerIn: parent
                            text: "全部"
                            color: allChip.chosen ? Theme.textPrimary : Theme.textSecondary
                            font.pixelSize: Theme.fontSmall
                            font.weight: allChip.chosen ? Font.Medium : Font.Normal
                        }
                        Rectangle {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: 2
                            color: allChip.chosen ? Theme.primary : "transparent"
                        }
                        MouseArea {
                            anchors.fill: parent
                            anchors.topMargin: -6
                            anchors.bottomMargin: -6
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                root.tagFilterOpen = false
                                bridge.clearTagFilter()
                            }
                        }
                    }

                    Repeater {
                        model: contactTagBar.shownTags
                        delegate: Item {
                            id: tagChip
                            readonly property bool chosen: bridge.tagFilter === modelData
                            width: tagLabel.implicitWidth
                            height: 30
                            anchors.verticalCenter: parent.verticalCenter
                            Text {
                                id: tagLabel
                                anchors.centerIn: parent
                                text: modelData
                                textFormat: Text.PlainText
                                color: tagChip.chosen ? Theme.textPrimary : Theme.textSecondary
                                font.pixelSize: Theme.fontSmall
                                font.weight: tagChip.chosen ? Font.Medium : Font.Normal
                            }
                            Rectangle {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                height: 2
                                color: tagChip.chosen ? Theme.primary : "transparent"
                            }
                            MouseArea {
                                anchors.fill: parent
                                anchors.topMargin: -6
                                anchors.bottomMargin: -6
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    root.tagFilterOpen = false
                                    bridge.setTagFilter(modelData)
                                }
                            }
                        }
                    }
                }

                Item {
                    visible: contactTagBar.overflow
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: contactTagBar.moreWidth
                    height: 30
                    Text {
                        anchors.centerIn: parent
                        text: "更多"
                        color: root.tagFilterOpen ? Theme.textPrimary : Theme.textSecondary
                        font.pixelSize: Theme.fontSmall
                    }
                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: 2
                        color: root.tagFilterOpen ? Theme.primary : "transparent"
                    }
                    MouseArea {
                        anchors.fill: parent
                        anchors.topMargin: -6
                        anchors.bottomMargin: -6
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
                y: listHeader.height + 6 + (compactContactTabs.visible ? 34 : 0)
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
                        width: 18
                        height: 18
                        radius: 9
                        color: Theme.seal
                        Text {
                            anchors.centerIn: parent
                            text: root.pendingCount > 9 ? "9+" : root.pendingCount
                            color: Theme.onSeal
                            font.pixelSize: Theme.fontCaption
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

                AppToolTip {
                    text: "新的朋友"
                    hovered: compactFriendArea.containsMouse
                }
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
                    && !(chatSearch.text.trim().length > 0
                         && bridge.filteredConversations.count === 0)
                model: bridge.filteredConversations
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
                    // 时间与「…」共用行尾同一个槽位：悬停时时间淡出，按钮淡入，互不挤压
                    readonly property bool rowHovered: convArea.containsMouse
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
                        // 已经停在这个会话的聊天页：再点一次收起，回到「选择一个会话」的空态；
                        // 停在群资料/好友资料时仍然先回到聊天，不清空
                        if (root.visiblePane === ChatBridge.ChatPage
                                && bridge.currentThreadId === model.threadId) {
                            bridge.clearConversation()
                            return
                        }
                        if (group)
                            bridge.openGroup(model.threadId)
                        else
                            bridge.openChat(model.uid)
                        if (chatSearch.text.length > 0)
                            root.clearConversationFilter()
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
                            { text: pinned ? "取消置顶" : "置顶会话", icon: "pin",
                              action: () => bridge.setThreadPinned(threadId, !pinned) }
                        ]
                        //单聊免打扰是本地设置；群免打扰仍在群资料页，避免两套状态打架
                        if (!isGroupRow) {
                            items.push({ text: muted ? "消息免打扰：开" : "消息免打扰",
                                         icon: muted ? "bellOff" : "bell",
                                         action: () => bridge.setThreadMuted(threadId, !muted) })
                        }
                        items.push({ text: "标记为未读", icon: "mail",
                                     action: () => bridge.markThreadUnread(threadId) })
                        items.push({ text: "清除聊天记录", icon: "trash",
                                     action: () => bridge.askClearHistory(threadId, kind, title) })
                        items.push({ text: "删除会话", icon: "trash",
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
                        x: 8
                        y: root.compactIndex ? 4 : 2
                        width: root.compactIndex ? convRow.width - 16 : parent.width - 16
                        height: root.compactIndex ? 48 : parent.height - 4
                        radius: Theme.panelRadius
                        color: convRow.chosen ? Theme.panelBg : "transparent"
                        border.width: convRow.chosen ? 1 : 0
                        border.color: Theme.line

                        // 悬停用一层纸色叠在卡上：透明直接插值到纸色会先闪一层灰
                        Rectangle {
                            anchors.fill: parent
                            radius: parent.radius
                            color: Theme.selected
                            opacity: !convRow.chosen && (convArea.containsMouse || convRow.activeFocus) ? 1 : 0
                            Behavior on opacity { NumberAnimation { duration: 120 } }
                        }

                        Rectangle {
                            visible: convRow.chosen && !root.compactIndex
                            anchors.left: parent.left
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            anchors.topMargin: 8
                            anchors.bottomMargin: 8
                            width: 2
                            radius: 1
                            color: Theme.primary
                        }
                    }
                    Avatar {
                        id: convAvatar
                        x: root.compactIndex ? (parent.width - width) / 2 : 20
                        width: 40
                        height: 40
                        radius: 10
                        anchors.verticalCenter: parent.verticalCenter
                        source: model.icon
                        name: model.displayName
                        group: convRow.group
                        Behavior on x {
                            NumberAnimation { duration: root.indexDragging ? 0 : 180; easing.type: Easing.OutCubic }
                        }
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
                        // 选中卡片左右各内缩 10，文字再留 14，避免贴住卡片描边
                        width: Math.max(0, parent.width - (convAvatar.x + convAvatar.width + 12) - 24)
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 6
                        // 宽度切换时淡入淡出：展开/收起不再是一帧换一套布局
                        opacity: root.compactIndex ? 0 : 1
                        // 收起时留到动画结束再隐藏，文字才有时间淡出
                        visible: !root.compactIndex || root.indexAnimating
                        Behavior on opacity { NumberAnimation { duration: root.indexDragging ? 0 : 160 } }

                        Item {
                            width: parent.width
                            height: convName.implicitHeight

                            Text {
                                id: convName
                                anchors.left: parent.left
                                //群标记占在名字与时间之间，名字先让位给这两个标记
                                anchors.right: convTime.left
                                anchors.rightMargin: 8
                                anchors.verticalCenter: parent.verticalCenter
                                text: model.displayName
                                textFormat: Text.PlainText
                                color: Theme.textPrimary
                                font.pixelSize: Theme.fontBody
                                font.weight: convRow.chosen ? Font.Medium : Font.Normal
                                elide: Text.ElideRight
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
                                opacity: convRow.rowHovered ? 0 : 1
                                Behavior on opacity { NumberAnimation { duration: 100 } }
                            }

                            // 悬停时接替时间的位置：三点与名字同一条中线，不再整体居中
                            Rectangle {
                                id: convMore
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                width: 26
                                height: 26
                                radius: Theme.controlRadius
                                visible: !root.compactIndex
                                opacity: convRow.rowHovered ? 1 : 0
                                color: "transparent"
                                Behavior on opacity { NumberAnimation { duration: 100 } }
                                AppIcon {
                                    anchors.centerIn: parent
                                    name: "more"
                                    size: 15
                                    color: Theme.textSecondary
                                }
                            }
                        }
                        Text {
                            width: parent.width
                            text: {
                                if (model.draft.length > 0)
                                    return "草稿 · " + model.draft
                                const body = model.lastText.length > 0
                                    ? model.lastText : "还没有消息"
                                const line = convRow.group && model.subtitle.length > 0
                                    ? model.subtitle + " · " + body : body
                                return model.atMe ? "[有人@我] " + line : line
                            }
                            color: model.draft.length > 0 || model.atMe
                                ? Theme.accent : Theme.textSecondary
                            font.pixelSize: Theme.fontSmall
                            elide: Text.ElideRight
                            maximumLineCount: 1
                            textFormat: Text.PlainText
                        }
                    }

                    // 未读：免打扰的群只显示一个小圆点，不报数字（和微信一致）
                    Rectangle {
                        visible: model.unread > 0 && !model.muted
                        x: convAvatar.x + convAvatar.width - 9
                        y: convAvatar.y - 3
                        width: Math.max(18, convUnread.implicitWidth + 10)
                        height: 18
                        radius: 9
                        color: Theme.seal
                        Text {
                            id: convUnread
                            anchors.centerIn: parent
                            text: model.unread > 99 ? "99+" : model.unread
                            color: Theme.onSeal
                            font.pixelSize: Theme.fontCaption
                        }
                    }
                    Rectangle {
                        visible: model.unread > 0 && model.muted
                        x: convAvatar.x + convAvatar.width - 7
                        y: convAvatar.y
                        width: 8
                        height: 8
                        radius: 4
                        color: Theme.seal
                    }
                    MouseArea {
                        id: convArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: (mouse) => {
                            convRow.forceActiveFocus(Qt.MouseFocusReason)
                            // 行尾 40px 是「…」槽位：点这里开菜单，点其他位置开会话
                            if (!root.compactIndex && mouse.x >= convRow.width - 40) {
                                const point = convRow.mapToItem(root, mouse.x, mouse.y)
                                convRow.openMenu(point.x - 126, point.y + 8)
                                return
                            }
                            convRow.activate()
                        }
                    }

                    // 折叠态只有头像，名字用悬停提示补回来
                    AppToolTip {
                        text: model.displayName
                        hovered: root.compactIndex && convArea.containsMouse
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

            // 会话筛选没有结果：给一条去联系人搜索的明确出口
            Rectangle {
                id: noResultBox
                visible: bridge.mode === ChatBridge.ChatMode && !root.compactIndex
                    && chatSearch.text.trim().length > 0
                    && bridge.filteredConversations.count === 0
                x: 20
                y: searchArea.y + searchArea.height + 28
                width: parent.width - 40
                height: 58
                color: "transparent"
                z: 10

                Column {
                    width: parent.width
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 10
                    Text {
                        width: parent.width
                        text: "没有找到会话"
                        horizontalAlignment: Text.AlignHCenter
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSmall
                    }
                    AppLink {
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        elide: Text.ElideRight
                        color: noResultArea.containsMouse || activeFocus ? Theme.primary : Theme.textSecondary
                        font.underline: noResultArea.containsMouse || activeFocus
                        text: "在联系人里找「" + chatSearch.text.trim() + "」"
                        onClicked: root.openContactSearchFor(chatSearch.text.trim())
                    }
                }

                MouseArea {
                    id: noResultArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.openContactSearchFor(chatSearch.text.trim())
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
                    ? (compactNewFriend.visible ? 62 : 6) + (compactContactTabs.visible ? 34 : 0)
                    : (root.tagBarVisible ? 8 : 8)
                anchors.bottom: parent.bottom
                clip: true
                model: bridge.friends
                spacing: 2
                boundsBehavior: Flickable.StopAtBounds
                activeFocusOnTab: true
                Keys.onDownPressed: (event) => { root.stepFriend(1); event.accepted = true }
                Keys.onUpPressed: (event) => { root.stepFriend(-1); event.accepted = true }
                visible: bridge.mode === ChatBridge.ContactMode && !root.groupTab

                // 联系人列表顶部固定「新的朋友」；没有好友但有申请时只显示这一行
                header: Item {
                    width: friendsList.width
                    height: root.newFriendEntryVisible ? (root.friendCount > 0 ? 96 : 72) : 0
                    visible: root.newFriendEntryVisible

                    Rectangle {
                        id: newFriendButton
                        x: 8
                        width: parent.width - 16
                        height: 62
                        radius: Theme.panelRadius
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
                            x: 12
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
                            x: 60
                            width: parent.width - 60 - 38
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 3
                            Text {
                                width: parent.width
                                text: "新的朋友"
                                textFormat: Text.PlainText
                                font.pixelSize: Theme.fontBody
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
                                font.pixelSize: Theme.fontCaption
                                color: Theme.onSeal
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
                        x: 20
                        y: 74
                        visible: root.friendCount > 0
                        //筛选时标题直接说明当前分组与命中数量，避免"全部"与结果不符
                        text: bridge.tagFilter.length > 0
                            ? "「" + bridge.tagFilter + "」· " + root.visibleFriendCount + " 位"
                            : "全部联系人"
                        textFormat: Text.PlainText
                        font.pixelSize: Theme.fontCaption
                        color: Theme.textHint
                    }
                }

                // 联系人行：只负责打开资料页，最后一条消息与未读属于会话列表
                delegate: Item {
                    id: friendRow
                    width: friendsList.width
                    height: root.compactIndex ? 56 : Theme.listRowHeight
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
                        // 已经停在这位好友的资料页：再点一次回到空态，和会话列表同一套手感
                        if (root.visiblePane === ChatBridge.FriendInfoPage
                                && bridge.profile.uid === model.uid) {
                            bridge.clearContactSelection()
                            return
                        }
                        bridge.openProfile(model.uid)
                    }

                    // 联系人行不再挂右键菜单：点击就是看资料，删除好友只在资料页里做，
                    // 避免在一张每天要扫很多遍的列表里出现不可逆操作

                    Rectangle {
                        x: 8
                        y: root.compactIndex ? 4 : 2
                        width: root.compactIndex ? friendRow.width - 16 : parent.width - 16
                        height: root.compactIndex ? 48 : parent.height - 4
                        radius: Theme.panelRadius
                        color: friendRow.chosen ? Theme.panelBg : "transparent"
                        border.width: friendRow.chosen ? 1 : 0
                        border.color: Theme.line

                        // 悬停用一层纸色叠在卡上：透明直接插值到纸色会先闪一层灰
                        Rectangle {
                            anchors.fill: parent
                            radius: parent.radius
                            color: Theme.selected
                            opacity: !friendRow.chosen && (friendArea.containsMouse || friendRow.activeFocus) ? 1 : 0
                            Behavior on opacity { NumberAnimation { duration: 120 } }
                        }

                        Rectangle {
                            visible: friendRow.chosen && !root.compactIndex
                            anchors.left: parent.left
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            anchors.topMargin: 8
                            anchors.bottomMargin: 8
                            width: 2
                            radius: 1
                            color: Theme.primary
                        }
                    }
                    Avatar {
                        id: friendAvatar
                        x: root.compactIndex ? (parent.width - width) / 2 : 20
                        width: 40
                        height: 40
                        radius: 10
                        anchors.verticalCenter: parent.verticalCenter
                        source: model.icon
                        name: model.displayName
                        Behavior on x {
                            NumberAnimation { duration: root.indexDragging ? 0 : 180; easing.type: Easing.OutCubic }
                        }
                    }
                    Column {
                        anchors.left: friendAvatar.right
                        anchors.leftMargin: 12
                        width: Math.max(0, parent.width - (friendAvatar.x + friendAvatar.width + 12) - 24)
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 4
                        opacity: root.compactIndex ? 0 : 1
                        // 收起时留到动画结束再隐藏，文字才有时间淡出
                        visible: !root.compactIndex || root.indexAnimating
                        Behavior on opacity { NumberAnimation { duration: root.indexDragging ? 0 : 160 } }

                        Text {
                            width: parent.width
                            text: model.displayName
                            textFormat: Text.PlainText
                            color: Theme.textPrimary
                            font.pixelSize: Theme.fontBody
                            font.weight: friendRow.chosen ? Font.Medium : Font.Normal
                            elide: Text.ElideRight
                        }
                        //联系人摘要优先显示标签，其次个性签名；都没有就不占位
                        Text {
                            width: parent.width
                            visible: text.length > 0
                            text: model.tags.length > 0 ? model.tags.join(" · ") : model.desc
                            textFormat: Text.PlainText
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontSmall
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
                    AppToolTip {
                        text: model.displayName
                        hovered: root.compactIndex && friendArea.containsMouse
                    }
                }
            }

            // 群聊列表：按「我管理的群 / 我加入的群」分两段，段标题可收起；
            // 行本身仍与好友行同一套行高、头像、选中态
            ListView {
                id: groupsList
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: root.compactIndex ? listHeader.bottom : searchArea.bottom
                anchors.topMargin: root.compactIndex
                    ? 6 + (compactContactTabs.visible ? 34 : 0) : 10
                anchors.bottom: parent.bottom
                clip: true
                model: groupRowsModel
                spacing: 2
                boundsBehavior: Flickable.StopAtBounds
                visible: bridge.mode === ChatBridge.ContactMode && root.groupTab
                    && root.groupTotal > 0
                activeFocusOnTab: true
                Keys.onDownPressed: (event) => { root.stepGroup(1); event.accepted = true }
                Keys.onUpPressed: (event) => { root.stepGroup(-1); event.accepted = true }

                delegate: Item {
                    id: groupRow
                    width: groupsList.width
                    height: model.header ? 32 : (root.compactIndex ? 56 : Theme.listRowHeight)
                    readonly property bool chosen: !model.header
                        && root.visiblePane === ChatBridge.GroupInfoPage
                        && bridge.currentThreadId === model.threadId
                    activeFocusOnTab: !model.header
                    Accessible.role: model.header ? Accessible.Grouping : Accessible.Button
                    Accessible.name: model.header ? model.title : model.name
                    Accessible.onPressAction: if (!model.header) activate()
                    Keys.onSpacePressed: if (!model.header) activate()
                    Keys.onReturnPressed: if (!model.header) activate()
                    Keys.onEnterPressed: if (!model.header) activate()
                    Keys.onDownPressed: (event) => { root.stepGroup(1); event.accepted = true }
                    Keys.onUpPressed: (event) => { root.stepGroup(-1); event.accepted = true }
                    onActiveFocusChanged: {
                        if (activeFocus)
                            groupsList.positionViewAtIndex(index, ListView.Contain)
                    }

                    function activate() {
                        groupsList.currentIndex = index
                        // 已经停在这个群的资料页：再点一次回到空态
                        if (root.visiblePane === ChatBridge.GroupInfoPage
                                && bridge.currentThreadId === model.threadId) {
                            bridge.clearContactSelection()
                            return
                        }
                        bridge.openGroupInfoFor(model.threadId)
                    }

                    // 段标题：只做收起/展开，不参与键盘跳行
                    Item {
                        anchors.fill: parent
                        visible: model.header

                        Rectangle {
                            x: 8
                            y: 2
                            width: parent.width - 16
                            height: parent.height - 4
                            radius: 8
                            color: sectionArea.containsMouse ? Theme.selected : "transparent"
                        }
                        Row {
                            x: 20
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 6
                            Text {
                                text: model.title
                                textFormat: Text.PlainText
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontCaption
                                font.weight: Font.Medium
                            }
                            Text {
                                text: model.sectionCount
                                color: Theme.textHint
                                font.pixelSize: Theme.fontCaption
                            }
                        }
                        AppIcon {
                            anchors.right: parent.right
                            anchors.rightMargin: 22
                            anchors.verticalCenter: parent.verticalCenter
                            name: "chevronDown"
                            size: 12
                            color: Theme.textHint
                            rotation: model.collapsed ? -90 : 0
                            Behavior on rotation { NumberAnimation { duration: 140 } }
                        }
                        MouseArea {
                            id: sectionArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.toggleGroupSection(model.sectionKey)
                        }
                    }

                    // 群行
                    Item {
                        anchors.fill: parent
                        visible: !model.header

                        Rectangle {
                            x: 8
                            y: root.compactIndex ? 4 : 2
                            width: root.compactIndex ? groupRow.width - 16 : parent.width - 16
                            height: root.compactIndex ? 48 : parent.height - 4
                            radius: 10
                            color: groupRow.chosen ? Theme.panelBg : "transparent"
                            border.width: groupRow.chosen ? 1 : 0
                            border.color: Theme.line

                            // 悬停用一层纸色叠在卡上：透明直接插值到纸色会先闪一层灰
                            Rectangle {
                                anchors.fill: parent
                                radius: parent.radius
                                color: Theme.selected
                                opacity: !groupRow.chosen && (groupArea.containsMouse || groupRow.activeFocus) ? 1 : 0
                                Behavior on opacity { NumberAnimation { duration: 120 } }
                            }

                            Rectangle {
                                visible: groupRow.chosen && !root.compactIndex
                                anchors.left: parent.left
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                anchors.topMargin: 8
                                anchors.bottomMargin: 8
                                width: 2
                                radius: 1
                                color: Theme.primary
                            }
                        }

                        Avatar {
                            x: root.compactIndex ? (parent.width - width) / 2 : 20
                            width: 40
                            height: 40
                            radius: 10
                            anchors.verticalCenter: parent.verticalCenter
                            source: model.icon
                            name: model.name
                            group: true
                        }

                        Column {
                            anchors.left: parent.left
                            anchors.leftMargin: 72
                            width: Math.max(0, parent.width - 72 - 24)
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 5
                            opacity: root.compactIndex ? 0 : 1
                            // 收起时留到动画结束再隐藏，文字才有时间淡出
                            visible: !root.compactIndex || root.indexAnimating
                            Behavior on opacity { NumberAnimation { duration: root.indexDragging ? 0 : 160 } }

                            Text {
                                width: parent.width
                                text: model.name
                                textFormat: Text.PlainText
                                color: Theme.textPrimary
                                font.pixelSize: Theme.fontBody
                                font.weight: groupRow.chosen ? Font.Medium : Font.Normal
                                elide: Text.ElideRight
                            }
                            Text {
                                width: parent.width
                                text: model.memberCount + " 位成员"
                                    + (model.role === 2 ? " · 群主" : model.role === 1 ? " · 管理员" : "")
                                    + (model.muted ? " · 免打扰" : "")
                                textFormat: Text.PlainText
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontSmall
                                elide: Text.ElideRight
                            }
                        }

                        MouseArea {
                            id: groupArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                groupRow.forceActiveFocus(Qt.MouseFocusReason)
                                groupRow.activate()
                            }
                        }

                        AppToolTip {
                            text: model.name
                            hovered: root.compactIndex && groupArea.containsMouse
                        }
                    }
                }
            }

            Text {
                visible: bridge.mode === ChatBridge.ContactMode && root.groupTab
                    && root.groupTotal > 0 && bridge.groups.count === 0
                anchors.horizontalCenter: parent.horizontalCenter
                y: searchArea.y + searchArea.height + 44
                text: "没有找到这个群"
                color: Theme.textHint
                font.pixelSize: Theme.fontSmall
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
            width: root.indexPanelAnimatedWidth
            compact: root.compactIndex
            height: parent.height
            visible: bridge.mode === ChatBridge.SettingsMode
            controller: bridge
            onAboutRequested: aboutDialog.open()
        }

        // 右侧内容区：按 pane 切换页面，实例常驻以保留输入草稿和滚动位置
        Item {
            id: contentPane
            width: parent.width - root.sidebarWidth - root.indexPanelAnimatedWidth
            height: parent.height

            // 分隔条跨在第二栏右边缘上：拖动改宽度，双击恢复 280
            Item {
                id: splitter
                x: -5
                width: 10
                height: parent.height
                z: 100
                // 头像条状态下也保留这条把手：可以像展开态一样直接拖出宽度
                visible: true

                Rectangle {
                    anchors.fill: parent
                    // 只在真正拖动时亮蓝条：hover 时靠光标和把手提示，松手立刻消失
                    color: splitterArea.pressed
                        ? Qt.rgba(Theme.primary.r, Theme.primary.g, Theme.primary.b, 0.18)
                        : "transparent"
                }

                MouseArea {
                    id: splitterArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.SizeHorCursor
                    onPressed: (mouse) => root.beginIndexDrag(mapToItem(null, mouse.x, mouse.y).x)
                    onPositionChanged: (mouse) => {
                        if (pressed)
                            root.updateIndexDrag(mapToItem(null, mouse.x, mouse.y).x)
                    }
                    onReleased: root.endIndexDrag()
                    onDoubleClicked: {
                        root.indexDragging = false
                        if (root.compactIndex) {
                            root.indexAnimating = true
                            indexAnimTimer.restart()
                            root.compactMode = 0
                            return
                        }
                        root.indexAnimating = true
                        indexAnimTimer.restart()
                        root.indexWidth = Theme.indexWidth
                        root.layoutPrefs.indexWidth = Theme.indexWidth
                    }
                }
            }

            // 整条分隔条附近 28px 都能唤起把手：鼠标一靠近列表边界就浮出，不用瞄准中线
            Item {
                x: -14
                width: 28
                height: parent.height
                z: 115
                HoverHandler {
                    id: boundaryHover
                    cursorShape: Qt.SplitHCursor
                }
            }

            // 收起把手：鼠标靠近分隔条时，在中点浮出一枚小按钮，点击切窄窗。
            // 展开后同一位置反向，窄窗状态下也一直可达。
            Item {
                id: indexHandle
                visible: !root.indexDragging || handleArea.pressed
                x: -12
                width: 24
                height: 56
                anchors.verticalCenter: parent.verticalCenter
                z: 120

                Rectangle {
                    anchors.centerIn: parent
                    width: 20
                    height: 48
                    radius: 9
                    color: handleArea.containsMouse ? Theme.panelBg : Theme.sheet
                    border.width: 1
                    border.color: handleArea.containsMouse ? Theme.line : Theme.lineSoft
                    opacity: !root.indexDragging
                        && (boundaryHover.hovered || handleArea.containsMouse) ? 1 : 0
                    Behavior on opacity { NumberAnimation { duration: 120 } }

                    AppIcon {
                        anchors.centerIn: parent
                        name: root.compactIndex ? "chevronRight" : "chevronLeft"
                        size: 13
                        color: Theme.textSecondary
                    }
                }

                MouseArea {
                    id: handleArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onPressed: (mouse) => root.beginIndexDrag(mapToItem(null, mouse.x, mouse.y).x)
                    onPositionChanged: (mouse) => {
                        if (pressed)
                            root.updateIndexDrag(mapToItem(null, mouse.x, mouse.y).x)
                    }
                    onReleased: root.endIndexDrag()
                    // 拖过之后不再触发“点击切换”，避免松手又弹回原状态
                    onClicked: if (!root.indexDragMoved) root.toggleIndex()
                }

                AppToolTip {
                    text: bridge.mode === ChatBridge.SettingsMode
                        ? (root.compactIndex ? "展开设置" : "收起设置")
                        : (root.compactIndex ? "展开列表" : "收起为头像条")
                    hovered: handleArea.containsMouse
                }
            }

            Column {
                anchors.centerIn: parent
                width: Math.min(360, parent.width - 80)
                spacing: 18
                visible: root.visiblePane === ChatBridge.EmptyPane
                opacity: visible ? 1 : 0
                Behavior on opacity { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
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
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontTitle
                    font.weight: Font.Medium
                }
                Text {
                    width: parent.width
                    text: root.emptyBody
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSmall
                    lineHeight: 1.45
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
                opacity: visible ? 1 : 0
                Behavior on opacity { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
                profile: bridge.profile
                controller: bridge
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
                opacity: visible ? 1 : 0
                Behavior on opacity { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
                controller: bridge
                onBackRequested: bridge.mode === ChatBridge.ContactMode
                    ? bridge.backToContactList() : bridge.backToChat()
            }
            ApplyFriendPage {
                anchors.fill: parent
                visible: root.visiblePane === ChatBridge.ApplyFriendPage
                opacity: visible ? 1 : 0
                Behavior on opacity { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
                applies: bridge.applies
                onAuthRequested: (uid, name, desc) => dialogs.openAuthDialog(uid, name, desc)
                onChatRequested: (uid) => bridge.openChat(uid)
            }
            UserInfoPage {
                id: userInfoPage
                anchors.fill: parent
                visible: root.visiblePane === ChatBridge.UserInfoPage
                opacity: visible ? 1 : 0
                Behavior on opacity { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
                controller: bridge
                onEditAvatarRequested: avatarEditor.chooseImage()
                onEditProfileRequested: (field) => profileDialogs.openSelf(field)
            }
            GeneralSettingsPage {
                anchors.fill: parent
                visible: root.visiblePane === ChatBridge.GeneralPage
                opacity: visible ? 1 : 0
                Behavior on opacity { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
                controller: bridge
            }
        }
    }

    // 聊天页加号的功能菜单：与右键菜单同一个组件，位置挂在加号下方
    ContextMenu {
        id: functionMenu
        anchors.fill: parent
        onClosed: root.functionMenuOpen = false
    }

    // 发起群聊：Popup 自己处理模态与焦点，打开时底层 Row 由 dialogs 之外单独禁用
    AboutDialog {
        id: aboutDialog
    }

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
        onCloseRequested: {
            chatPage.forwardOpen = false
            if (!chatPage.selecting)
                chatPage.selectedItems = []
        }
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
        onAvatarEditRequested: avatarEditor.chooseImage()
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

    // 清除、删除、退出共用一套确认骨架：标题、后果、动词按钮
    ConfirmDialog {
        id: clearConfirm
        title: bridge.clearConfirmDelete ? "删除会话" : "清除聊天记录"
        body: bridge.clearConfirmDelete
            ? "删除与「" + bridge.clearConfirmTitle + "」的会话？聊天记录只从这台设备移除，"
              + "好友关系和服务端记录都会保留，收到新消息时会话会重新出现。"
            : "清除与「" + bridge.clearConfirmTitle + "」的聊天记录？只删除这台设备上的内容，"
              + "服务器与对方的记录不受影响。"
        confirmText: bridge.clearConfirmDelete ? "删除会话" : "清除聊天记录"
        danger: true
        onConfirmed: {
            bridge.confirmClearHistory()
            close()
        }
        onCancelled: bridge.cancelClearHistory()
    }

    ConfirmDialog {
        id: deleteConfirm
        title: "删除好友"
        body: "删除「" + bridge.deleteTargetName + "」？对方不会收到通知，"
            + "你们的历史消息仍会保留，但不能再发消息。"
        confirmText: "删除好友"
        danger: true
        onConfirmed: {
            bridge.confirmDeleteFriend()
            close()
        }
        onCancelled: bridge.cancelDeleteFriend()
    }

    ConfirmDialog {
        id: logoutConfirm
        title: "退出登录"
        body: "退出后会断开与服务器的连接。这台设备上的聊天记录与草稿会保留，"
            + "正在编辑但还没发送的附件不会保留。"
        confirmText: "退出登录"
        danger: false
        onConfirmed: {
            bridge.confirmLogout()
            close()
        }
        onCancelled: bridge.cancelLogout()
    }

    Connections {
        target: bridge
        function onClearConfirmChanged() {
            if (bridge.clearConfirmVisible)
                clearConfirm.ask()
            else
                clearConfirm.close()
        }
        function onDeleteConfirmChanged() {
            if (bridge.deleteConfirmVisible)
                deleteConfirm.ask()
            else
                deleteConfirm.close()
        }
        function onLogoutConfirmChanged() {
            if (bridge.logoutConfirmVisible)
                logoutConfirm.ask()
            else
                logoutConfirm.close()
        }
    }

}
