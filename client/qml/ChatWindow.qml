import QtQuick
import XiguChat

// 聊天工作区：左侧导航 72、会话/联系人栏 280、右侧内容自适应。
// 这里只负责布局与焦点，导航状态和业务动作都交给 ChatBridge。
Item {
    id: root
    onEnabledChanged: if (!enabled) avatarEditor.dismiss()

    readonly property int sidebarWidth: 72
    readonly property int indexWidth: 280
    readonly property int listHeaderHeight: 78

    readonly property int friendCount: bridge.friends.count
    readonly property int pendingCount: bridge.applies.pendingCount
    readonly property int chatCount: bridge.friends.chatCount
    // 联系人空态：没有好友也没有待处理申请时才显示引导卡
    readonly property bool contactGuideVisible: bridge.mode === ChatBridge.ContactMode
        && friendCount === 0 && pendingCount === 0
    // 有好友或有申请时，列表顶部保留「新的朋友」入口
    readonly property bool newFriendEntryVisible: bridge.mode === ChatBridge.ContactMode
        && (friendCount > 0 || pendingCount > 0)
    property bool functionMenuOpen: false

    readonly property string emptyTitle: friendCount === 0
        ? (pendingCount > 0 ? "有 " + pendingCount + " 条好友申请待处理" : "还没有好友，先添加一位吧")
        : (chatCount === 0 ? "还没有会话，去联系人看看" : "选一位朋友，聊聊吧")
    readonly property string emptyAction: friendCount === 0
        ? (pendingCount > 0 ? "去处理" : "添加朋友")
        : (chatCount === 0 ? "去联系人" : "")

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

    function runEmptyAction() {
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
        enabled: !dialogs.visible

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
                        ? bridge.friends.chatCount + " 个会话"
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

            // 会话与联系人共用同一个好友模型；聊天模式下没有 thread_id 的条目高度为0，
            // 所以统计会话数量用 friends.chatCount，而不是列表行数
            ListView {
                id: friendsList
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: searchArea.bottom
                anchors.topMargin: 17
                anchors.bottom: parent.bottom
                clip: true
                model: bridge.friends
                spacing: 2
                boundsBehavior: Flickable.StopAtBounds
                visible: !root.contactGuideVisible

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
                        text: "全部联系人"
                        font.pixelSize: 11
                        color: Theme.textHint
                    }
                }

                // 一行两用：聊天模式打开会话并显示最后一条摘要，联系人模式打开资料页；
                // 高度为0的条目在此不可见也不可聚焦
                delegate: Item {
                    id: friendRow
                    width: friendsList.width
                    //聊天模式只显示已有会话的好友；联系人模式全部显示
                    readonly property bool chatMode: bridge.mode === ChatBridge.ChatMode
                    readonly property bool hasThread: model.threadId > 0
                    readonly property bool shown: !chatMode || hasThread
                    height: !shown ? 0 : (chatMode ? 76 : 60)
                    visible: shown
                    readonly property bool chosen: chatMode
                        ? bridge.pane === ChatBridge.ChatPage && bridge.currentUid === model.uid
                        : bridge.pane === ChatBridge.FriendInfoPage && bridge.profile.uid === model.uid
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: model.name
                    Accessible.onPressAction: activate()
                    Keys.onSpacePressed: activate()
                    Keys.onReturnPressed: activate()
                    Keys.onEnterPressed: activate()
                    onActiveFocusChanged: {
                        if (activeFocus)
                            friendsList.positionViewAtIndex(index, ListView.Contain)
                    }

                    function activate() {
                        if (chatMode)
                            bridge.openChat(model.uid)
                        else
                            bridge.openProfile(model.uid)
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
                        width: 40
                        height: 40
                        radius: 11
                        anchors.verticalCenter: parent.verticalCenter
                        source: model.icon
                        name: model.name
                    }
                    Column {
                        anchors.left: friendAvatar.right
                        anchors.leftMargin: 12
                        anchors.right: parent.right
                        anchors.rightMargin: 24
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 6
                        Text {
                            width: parent.width
                            text: model.name
                            textFormat: Text.PlainText
                            color: Theme.textPrimary
                            font.pixelSize: 14
                            font.bold: friendRow.chosen
                            elide: Text.ElideRight
                        }
                        Text {
                            visible: friendRow.chatMode
                            width: parent.width
                            text: model.lastText.length > 0 ? model.lastText : "还没有消息"
                            color: Theme.textSecondary
                            font.pixelSize: 12
                            elide: Text.ElideRight
                            maximumLineCount: 1
                            textFormat: Text.PlainText
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
                currentUid: bridge.currentUid
                title: bridge.currentName
                messages: bridge.messages
                onProfileRequested: bridge.openProfile(bridge.currentUid)
                onSendRequested: (parts) => {
                    if (bridge.sendParts(parts))
                        chatPage.clearInput()
                }
            }
            FriendInfoPage {
                anchors.fill: parent
                visible: bridge.pane === ChatBridge.FriendInfoPage
                profile: bridge.profile
                onChatRequested: bridge.openChat(bridge.profile.uid)
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
        height: 44
        radius: 8
        color: Theme.panelBg
        border.width: 1
        border.color: Theme.line
        z: 30

        // 目前只有「加好友」一项；以后加群聊时在同一列追加条目即可
        Rectangle {
            anchors.fill: parent
            anchors.margins: 2
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
                AppIcon {
                    name: "user"
                    size: 16
                    color: Theme.primary
                }
                Text {
                    text: "加好友"
                    color: Theme.textPrimary
                    font.pixelSize: 12
                }
            }
            MouseArea {
                id: menuArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.openAddFriendFromChat()
            }
        }
    }

    // 好友表单是覆盖层，打开时禁用底层 Row；头像编辑器独立于页面，避免切页被销毁
    FriendDialogs {
        id: dialogs
        anchors.fill: parent
        controller: bridge
    }

    AvatarEditor {
        id: avatarEditor
        onAvatarSaved: userInfoPage.avatarSaved = true
    }
}
