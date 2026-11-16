import QtQuick
import QtQuick.Controls
import XiguChat

// 聊天工作区：左侧导航 72、会话/联系人栏 280、右侧内容自适应。
// 这里只负责布局与焦点，导航状态和业务动作都交给 ChatBridge。
Item {
    id: root
    onEnabledChanged: if (!enabled) avatarEditor.dismiss()

    readonly property int sidebarWidth: 72
    readonly property int indexWidth: 280
    readonly property int listHeaderHeight: 78

    readonly property int friendCount: bridge.friends.totalCount
    readonly property int visibleFriendCount: bridge.friends.count
    readonly property int pendingCount: bridge.applies.pendingCount
    readonly property int chatCount: bridge.conversations.activeCount
    // 联系人空态：没有好友也没有待处理申请时才显示引导卡
    readonly property bool contactGuideVisible: bridge.mode === ChatBridge.ContactMode
        && friendCount === 0 && pendingCount === 0
    // 有好友或有申请时，列表顶部保留「新的朋友」入口
    readonly property bool newFriendEntryVisible: bridge.mode === ChatBridge.ContactMode
        && (friendCount > 0 || pendingCount > 0)
    // 标签条只在联系人模式且有标签时出现；筛选只影响列表，不影响入口与空态判断
    readonly property bool tagBarVisible: bridge.mode === ChatBridge.ContactMode
        && bridge.tagOptions.length > 0
    readonly property bool tagEmptyVisible: tagBarVisible && bridge.tagFilter.length > 0
        && visibleFriendCount === 0
    property bool tagFilterOpen: false //标签「更多」浮层是否展开
    onTagBarVisibleChanged: if (!tagBarVisible) tagFilterOpen = false
    property bool functionMenuOpen: false

    // 右栏空态跟着区域走：会话区说"聊天"，联系人区说"联系人"，不能混用
    readonly property string emptyTitle: {
        if (bridge.mode === ChatBridge.ContactMode) {
            if (friendCount === 0)
                return pendingCount > 0 ? "有 " + pendingCount + " 条好友申请待处理"
                                        : "还没有联系人，先添加一位吧"
            return pendingCount > 0 ? "有 " + pendingCount + " 条好友申请待处理"
                                    : "选择一位联系人，查看资料"
        }
        if (friendCount === 0)
            return pendingCount > 0 ? "有 " + pendingCount + " 条好友申请待处理"
                                    : "还没有好友，先添加一位吧"
        return chatCount === 0 ? "还没有会话，去联系人看看" : "选一位朋友，聊聊吧"
    }
    readonly property string emptyAction: {
        if (bridge.mode === ChatBridge.ContactMode) {
            if (pendingCount > 0)
                return "去处理"
            return friendCount === 0 ? "添加朋友" : ""
        }
        if (friendCount === 0)
            return pendingCount > 0 ? "去处理" : "添加朋友"
        return chatCount === 0 ? "去联系人" : ""
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
        // 有好友但没有会话：落到联系人列表，并打开第一位好友的资料页
        const first = bridge.friends.get(0)
        bridge.showContactMode()
        if (first && first.uid)
            bridge.openProfile(first.uid)
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

    Row {
        anchors.fill: parent
        enabled: !dialogs.visible && !createGroupDialog.visible

        // 一级导航：从上到下依次是聊天、联系人、设置；底部固定外观切换与本人头像
        Rectangle {
            width: root.sidebarWidth
            height: parent.height
            color: Theme.sidebarBg

            BrandMark {
                y: 26
                anchors.horizontalCenter: parent.horizontalCenter
                size: 32
            }

            // 三个入口共用 SideButton，选中态由 bridge.mode 决定
            Column {
                y: 112
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
                SideButton {
                    kind: "settings"
                    active: bridge.mode === ChatBridge.SettingsMode
                    onClicked: bridge.showSettings()
                }
            }

            // 外观切换放在头像上方，与列表栏保持同一条底部基线
            Rectangle {
                id: appearanceButton
                width: 40
                height: 40
                radius: 8
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: ownAvatar.top
                anchors.bottomMargin: 17
                color: appearanceArea.containsMouse || activeFocus ? Theme.selected : "transparent"
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: Theme.dark ? "切换到日间模式" : "切换到夜间模式"
                Accessible.onPressAction: Theme.toggle()
                Keys.onSpacePressed: Theme.toggle()
                Keys.onReturnPressed: Theme.toggle()
                AppIcon {
                    anchors.centerIn: parent
                    name: Theme.dark ? "sun" : "moon"
                    color: Theme.textSecondary
                    size: 20
                }
                MouseArea {
                    id: appearanceArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: Theme.toggle()
                }
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
                Accessible.onPressAction: bridge.showSettings()
                Keys.onSpacePressed: bridge.showSettings()
                Keys.onReturnPressed: bridge.showSettings()
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: bridge.showSettings()
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
            width: root.indexWidth
            height: parent.height
            color: Theme.panelBg

            Item {
                id: listHeader
                width: parent.width
                height: root.listHeaderHeight

                // 标题与统计随模式切换；加号在聊天页打开功能菜单，在联系人页打开加好友浮层
                Text {
                    x: 22
                    y: 21
                    text: bridge.mode === ChatBridge.ChatMode ? "聊天" : "联系人"
                    color: Theme.textPrimary
                    font.pixelSize: 22
                    font.bold: true
                }
                Text {
                    x: 23
                    y: 51
                    text: bridge.mode === ChatBridge.ChatMode
                        ? root.chatCount + " 个会话"
                        : bridge.friends.count + " 位好友"
                    color: Theme.textSecondary
                    font.pixelSize: 11
                }

                Rectangle {
                    anchors.right: parent.right
                    anchors.rightMargin: 18
                    anchors.verticalCenter: parent.verticalCenter
                    width: 30
                    height: 30
                    radius: 15
                    color: searchEntry.containsMouse || activeFocus ? Theme.selected : Theme.panelAlt
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: bridge.mode === ChatBridge.ChatMode ? "更多功能" : "添加朋友"
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

            // 两种模式共用同一个输入位置：聊天页过滤好友，联系人页添加朋友
            Item {
                id: searchArea
                x: 16
                y: listHeader.height + 2
                width: parent.width - 32
                height: 38

                SearchField {
                    id: chatSearch
                    anchors.fill: parent
                    visible: bridge.mode === ChatBridge.ChatMode
                    placeholder: "搜索好友"
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
                    placeholder: "想找谁？"
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

                // 用文字度量估每个标签的宽度，决定这一行能放几个，避免依赖滚动
                TextMetrics {
                    id: tagMetrics
                    font.pixelSize: Theme.fontSmall
                }

                readonly property int moreWidth: 52
                function chipWidth(text) {
                    tagMetrics.text = text
                    return Math.max(34, tagMetrics.boundingRect.width + 24)
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

            // 会话列表：单聊与群聊合成一张表，按最后消息时间倒序；
            // 联系人列表仍用好友模型，两边不再共用同一个 delegate
            ListView {
                id: conversationList
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: searchArea.bottom
                anchors.topMargin: 17
                anchors.bottom: parent.bottom
                clip: true
                visible: bridge.mode === ChatBridge.ChatMode
                model: bridge.conversations
                spacing: 2
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                delegate: Item {
                    id: convRow
                    width: conversationList.width
                    height: 76
                    readonly property bool chosen: (bridge.pane === ChatBridge.ChatPage
                        || bridge.pane === ChatBridge.GroupInfoPage)
                        && bridge.currentThreadId === model.threadId
                    readonly property bool group: model.kind === 2
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: model.displayName
                    Accessible.onPressAction: activate()
                    Keys.onSpacePressed: activate()
                    Keys.onReturnPressed: activate()
                    Keys.onEnterPressed: activate()
                    onActiveFocusChanged: {
                        if (activeFocus)
                            conversationList.positionViewAtIndex(index, ListView.Contain)
                    }

                    function activate() {
                        if (group)
                            bridge.openGroup(model.threadId)
                        else
                            bridge.openChat(model.uid)
                    }

                    // 群聊右键只给"进入会话/群资料"，退群与解散走群资料页的底部按钮
                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.RightButton
                        onClicked: (mouse) => {
                            const point = mapToItem(root, mouse.x, mouse.y)
                            if (group) {
                                const items = [
                                    { text: "进入群聊", action: () => bridge.openGroup(model.threadId) }
                                ]
                                //已离开的群没有可管理的资料，只剩历史与清理入口
                                if (model.leftState > 0) {
                                    items.push({ text: "清除聊天记录", action: () =>
                                        bridge.askClearHistory(model.threadId, 2, model.displayName) })
                                } else {
                                    items.push({ text: "群资料", action: () => {
                                        bridge.openGroup(model.threadId)
                                        bridge.openGroupInfo()
                                    } })
                                }
                                friendMenu.open(point.x, point.y, items)
                            } else {
                                const items = [
                                    { text: "发消息", action: () => bridge.openChat(model.uid) },
                                    { text: "查看资料", action: () => bridge.openProfile(model.uid) }
                                ]
                                //还是好友才显示"删除好友"；关系已经断了就只有清理入口
                                if (model.isFriend)
                                    items.push({ text: "删除好友", action: () => bridge.askDeleteFriend(model.uid) })
                                else
                                    items.push({ text: "清除聊天记录", action: () =>
                                        bridge.askClearHistory(model.threadId, 1, model.displayName) })
                                friendMenu.open(point.x, point.y, items)
                            }
                        }
                    }

                    Rectangle {
                        x: 10
                        width: parent.width - 20
                        height: parent.height - 2
                        radius: 7
                        color: convRow.chosen ? Theme.selected
                               : (convArea.containsMouse || convRow.activeFocus ? Theme.panelAlt : "transparent")
                        border.width: 1
                        border.color: convRow.activeFocus ? Theme.primary : "transparent"
                        Behavior on color { ColorAnimation { duration: 130 } }
                    }
                    Rectangle {
                        x: 10
                        width: 3
                        height: 18
                        radius: 1
                        anchors.verticalCenter: parent.verticalCenter
                        visible: convRow.chosen
                        color: Theme.accent
                    }
                    Avatar {
                        id: convAvatar
                        x: 22
                        width: 40
                        height: 40
                        radius: 11
                        anchors.verticalCenter: parent.verticalCenter
                        source: model.icon
                        name: model.displayName
                    }
                    Column {
                        anchors.left: convAvatar.right
                        anchors.leftMargin: 12
                        anchors.right: parent.right
                        anchors.rightMargin: 24
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 6

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
                                    font.pixelSize: 9
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
                                font.pixelSize: 10
                                visible: text.length > 0
                            }
                        }
                        Text {
                            width: parent.width
                            text: {
                                const body = model.lastText.length > 0 ? model.lastText : "还没有消息"
                                return model.atMe ? "[有人@我] " + body : body
                            }
                            color: model.atMe ? Theme.accent : Theme.textSecondary
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
                            font.pixelSize: 9
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
                }
            }

            // 联系人列表：只显示好友，按加好友顺序；标签筛选只作用于这张表
            ListView {
                id: friendsList
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: root.tagBarVisible ? contactTagBar.bottom : searchArea.bottom
                anchors.topMargin: root.tagBarVisible ? 10 : 17
                anchors.bottom: parent.bottom
                clip: true
                model: bridge.friends
                spacing: 2
                boundsBehavior: Flickable.StopAtBounds
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
                        radius: 7
                        color: bridge.pane === ChatBridge.ApplyFriendPage ? Theme.selected
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
                                font.pixelSize: 10
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
                    height: 60
                    readonly property bool chosen: bridge.pane === ChatBridge.FriendInfoPage
                        && bridge.profile.uid === model.uid
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: model.displayName
                    Accessible.onPressAction: activate()
                    Keys.onSpacePressed: activate()
                    Keys.onReturnPressed: activate()
                    Keys.onEnterPressed: activate()
                    onActiveFocusChanged: {
                        if (activeFocus)
                            friendsList.positionViewAtIndex(index, ListView.Contain)
                    }

                    function activate() {
                        bridge.openProfile(model.uid)
                    }

                    // 联系人行右键：发消息、看资料、删除好友（删除要二次确认）
                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.RightButton
                        onClicked: (mouse) => {
                            const point = mapToItem(root, mouse.x, mouse.y)
                            friendMenu.open(point.x, point.y, [
                                { text: "发消息", action: () => bridge.openChat(model.uid) },
                                { text: "查看资料", action: () => bridge.openProfile(model.uid) },
                                { text: "删除好友", action: () => bridge.askDeleteFriend(model.uid) }
                            ])
                        }
                    }

                    Rectangle {
                        x: 10
                        width: parent.width - 20
                        height: parent.height - 2
                        radius: 7
                        color: friendRow.chosen ? Theme.selected
                               : (friendArea.containsMouse || friendRow.activeFocus ? Theme.panelAlt : "transparent")
                        border.width: 1
                        border.color: friendRow.activeFocus ? Theme.primary : "transparent"
                        Behavior on color { ColorAnimation { duration: 130 } }
                    }
                    Rectangle {
                        x: 10
                        width: 3
                        height: 18
                        radius: 1
                        anchors.verticalCenter: parent.verticalCenter
                        visible: friendRow.chosen
                        color: Theme.accent
                    }
                    Avatar {
                        id: friendAvatar
                        x: 22
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
                }
            }

            // 聊天列表没有会话时的轻提示；联系人空态由下面的引导卡承担
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                y: searchArea.y + searchArea.height + 54
                visible: bridge.mode === ChatBridge.ChatMode && root.chatCount === 0
                text: root.friendCount === 0 ? "还没有好友" : "还没有会话"
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
                        text: "通讯录还是空的"
                        color: Theme.textPrimary
                        font.pixelSize: 15
                        font.weight: Font.DemiBold
                    }
                    Text {
                        width: parent.width
                        text: "搜个用户名或 ID，发去申请，通过了就能聊起来。"
                        color: Theme.textSecondary
                        font.pixelSize: 11
                        wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                    }
                    AppButton {
                        width: parent.width
                        height: 36
                        text: "添加朋友"
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
            width: root.indexWidth
            height: parent.height
            visible: bridge.mode === ChatBridge.SettingsMode
            controller: bridge
        }

        // 右侧内容区：按 pane 切换页面，实例常驻以保留输入草稿和滚动位置
        Item {
            width: parent.width - root.sidebarWidth - root.indexWidth
            height: parent.height

            Column {
                anchors.centerIn: parent
                width: Math.min(360, parent.width - 80)
                spacing: 18
                visible: bridge.pane === ChatBridge.EmptyPane
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
                visible: bridge.pane === ChatBridge.ChatPage
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
                onProfileRequested: bridge.openProfile(bridge.currentUid)
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
                visible: bridge.pane === ChatBridge.FriendInfoPage
                profile: bridge.profile
                onChatRequested: bridge.openChat(bridge.profile.uid)
                onDeleteRequested: bridge.askDeleteFriend(bridge.profile.uid)
                onEditRelationRequested: profileDialogs.openRemark(bridge.profile.uid,
                                                                  bridge.profile.displayName,
                                                                  bridge.profile.name,
                                                                  bridge.profile.icon,
                                                                  bridge.profile.remark,
                                                                  bridge.profile.tags || [])
            }
            GroupInfoPage {
                anchors.fill: parent
                visible: bridge.pane === ChatBridge.GroupInfoPage
                controller: bridge
                onBackRequested: bridge.backToChat()
            }
            ApplyFriendPage {
                anchors.fill: parent
                visible: bridge.pane === ChatBridge.ApplyFriendPage
                applies: bridge.applies
                onAuthRequested: (uid, name, desc) => dialogs.openAuthDialog(uid, name, desc)
                onChatRequested: (uid) => bridge.openChat(uid)
            }
            UserInfoPage {
                id: userInfoPage
                anchors.fill: parent
                visible: bridge.pane === ChatBridge.UserInfoPage
                controller: bridge
                onEditAvatarRequested: avatarEditor.chooseImage()
                onEditProfileRequested: (field) => profileDialogs.openSelf(field)
            }
            GeneralSettingsPage {
                anchors.fill: parent
                visible: bridge.pane === ChatBridge.GeneralPage
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
        x: root.sidebarWidth + root.indexWidth - 48
        y: 58
        width: 152
        height: 88
        radius: 8
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

    // 好友行右键菜单：删除入口放在这里，比翻资料页更直接
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
            radius: 12
            color: Theme.panelBg
            border.width: 1
            border.color: Theme.line

            Column {
                id: clearContent
                x: 24
                y: 24
                width: parent.width - 48
                spacing: 14

                Text {
                    text: "清除聊天记录"
                    color: Theme.textPrimary
                    font.pixelSize: 18
                    font.bold: true
                }
                Text {
                    width: parent.width
                    text: "清除与「" + bridge.clearConfirmTitle + "」的本地聊天记录？"
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
                        text: "确认清除"
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
            radius: 12
            color: Theme.panelBg
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
                        text: "确认删除"
                        onClicked: bridge.confirmDeleteFriend()
                    }
                }
            }
        }
    }
}
