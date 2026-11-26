import QtQuick
import QtQuick.Controls
import YunJian

//转发先选择收件人，确认后再发送，避免列表误点直接投递。
// 过滤只在前端做，不触发服务端搜索。
FocusScope {
    id: root
    anchors.fill: parent
    visible: open
    z: 95
    focus: open

    property var controller: null
    property bool open: false
    //true 时标题改成"合并转发"，目标选择后的动作由调用方决定
    property bool mergeMode: false
    property int count: 0
    property var recipient: null
    Keys.onEscapePressed: root.closeRequested()
    property string filter: ""
    signal targetChosen(double threadId, int kind, int uid)
    signal closeRequested()

    readonly property var allRows: {
        if (!open || !controller)
            return []
        const model = controller.conversations
        const total = model ? model.count : 0
        const rows = []
        for (let i = 0; i < total; ++i) {
            const row = model.get(i)
            if (row && row.threadId > 0)
                rows.push(row)
        }
        return rows
    }
    readonly property var filteredRows: {
        const keyword = filter.trim().toLowerCase()
        if (keyword.length === 0)
            return allRows
        return allRows.filter(function(row) {
            const name = String(row.displayName || "").toLowerCase()
            return name.indexOf(keyword) >= 0
        })
    }

    function focusRow(index) {
        if (list.count === 0)
            return
        list.currentIndex = Math.max(0, Math.min(list.count - 1, index))
        list.positionViewAtIndex(list.currentIndex, ListView.Contain)
        if (list.currentItem)
            list.currentItem.forceActiveFocus()
    }

    onOpenChanged: {
        if (open) {
            recipient = null
            filter = ""
            input.setText("")
            list.currentIndex = -1
            Qt.callLater(input.forceInputFocus)
        }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.overlay
        MouseArea {
            anchors.fill: parent
            onClicked: root.closeRequested()
        }
    }

    Rectangle {
        id: panel
        anchors.centerIn: parent
        width: Math.min(440, parent.width - 48)
        // 高度跟着收件人数走：两三个人时不留一整块空列表；超过 5 行再滚动
        readonly property int visibleRows: Math.min(5, Math.max(1, root.filteredRows.length))
        // 加上列表底部 10 的内边距，正好放下 N 行，不出多余滚动条；5 行以内不滚动
        height: Math.min(540, Math.max(320, 142 + visibleRows * 58 + 104))
        radius: Theme.dialogRadius
        color: Theme.surfaceFloating
        border.width: 1
        border.color: Theme.line

        //吞掉面板内的点击，避免点到空白处关闭
        MouseArea { anchors.fill: parent }

        Item {
            id: head
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 24
            height: 40

            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: (root.mergeMode ? "合并转发 " : "转发 ") + root.count + " 条消息到"
                font.pixelSize: Theme.fontTitle
                font.weight: Font.DemiBold
                color: Theme.textPrimary
            }

            Rectangle {
                id: closeButton
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: 32
                height: 32
                radius: Theme.controlRadius
                color: closeArea.containsMouse ? Theme.panelAlt : "transparent"
                AppIcon {
                    anchors.centerIn: parent
                    name: "close"
                    size: 16
                    color: Theme.textSecondary
                }
                MouseArea {
                    id: closeArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.closeRequested()
                }
            }
        }

        SearchField {
            id: input
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: head.bottom
            anchors.leftMargin: 24
            anchors.rightMargin: 24
            anchors.topMargin: 8
            placeholder: "搜索会话"
            inputObjectName: "forwardSearchInput"
            Keys.onDownPressed: root.focusRow(0)
            onAccepted: root.focusRow(0)
            onTextEdited: (value) => {
                root.filter = value
                root.recipient = null
                list.currentIndex = -1
            }
            onEscapePressed: root.closeRequested()
            onClearRequested: {
                input.setText("")
                root.filter = ""
                input.forceInputFocus()
            }
        }

        Rectangle {
            id: divider
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: input.bottom
            anchors.leftMargin: 24
            anchors.rightMargin: 24
            anchors.topMargin: 10
            height: 1
            color: Theme.line
        }

        Text {
            anchors.centerIn: list
            visible: root.filteredRows.length === 0
            text: root.filter.length > 0 ? "没有匹配的会话" : "还没有可转发的会话"
            font.pixelSize: Theme.fontSmall
            color: Theme.textHint
        }

        ListView {
            id: list
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: divider.bottom
            anchors.bottom: footer.top
            anchors.leftMargin: 16
            anchors.rightMargin: 16
            anchors.topMargin: 6
            anchors.bottomMargin: 10
            clip: true
            spacing: 2
            model: root.filteredRows
            ScrollBar.vertical: ScrollBar {}

            delegate: Rectangle {
                width: ListView.view.width
                height: 56
                radius: Theme.controlRadius
                color: root.recipient && root.recipient.threadId === modelData.threadId
                    ? Theme.selected : rowArea.containsMouse || activeFocus ? Theme.panelAlt : "transparent"
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: modelData.displayName || ""
                function selectRecipient() {
                    root.recipient = modelData
                    sendButton.forceActiveFocus()
                }
                Accessible.onPressAction: selectRecipient()
                Keys.onDownPressed: root.focusRow(index + 1)
                Keys.onUpPressed: index > 0 ? root.focusRow(index - 1) : input.forceInputFocus()
                Keys.onReturnPressed: selectRecipient()
                Keys.onEnterPressed: selectRecipient()
                Keys.onSpacePressed: selectRecipient()

                Avatar {
                    id: rowAvatar
                    anchors.left: parent.left
                    anchors.leftMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    width: 36
                    height: 36
                    group: modelData.kind === 2
                    showGroupBadge: false
                    textSize: 13
                    name: modelData.displayName || ""
                    source: modelData.icon || ""
                }

                Column {
                    anchors.left: rowAvatar.right
                    anchors.leftMargin: 10
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 3
                    Text {
                        width: parent.width
                        text: modelData.displayName || ""
                        textFormat: Text.PlainText
                        font.pixelSize: Theme.fontBody
                        color: Theme.textPrimary
                        elide: Text.ElideRight
                    }
                    Text {
                        width: parent.width
                        text: modelData.kind === 2
                            ? (modelData.memberCount > 0
                                ? "群聊 · " + modelData.memberCount + " 人" : "群聊")
                            : "单聊"
                        font.pixelSize: Theme.fontCaption
                        color: Theme.textHint
                    }
                }

                MouseArea {
                    id: rowArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: parent.selectRecipient()
                }
            }
        }
        Item {
            id: footer
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 104
            Rectangle { width: parent.width; height: 1; color: Theme.lineSoft }
            Text {
                x: 24; y: 12; width: parent.width - 48
                text: root.recipient ? "发送给：" + root.recipient.displayName : "选择一个收件人"
                textFormat: Text.PlainText
                elide: Text.ElideRight
                font.pixelSize: Theme.fontSmall
                color: Theme.textSecondary
            }
            Row {
                x: 24; y: 40; width: parent.width - 48; spacing: 12
                AppButton {
                    width: (parent.width - 12) / 2; height: 40
                    text: "取消"; variant: "secondary"
                    onClicked: root.closeRequested()
                }
                AppButton {
                    width: (parent.width - 12) / 2; height: 40
                    id: sendButton
                    text: "发送"; enabled: root.recipient !== null
                    onClicked: {
                        root.targetChosen(root.recipient.threadId, root.recipient.kind, root.recipient.uid)
                    }
                }
            }
        }

    }
}
