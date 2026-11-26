import QtQuick
import QtQuick.Controls
import YunJian

// 消息搜索：检索本机已同步的文本消息，点结果回到原会话并定位到那一条。
// 面板只负责输入与展示，查询、会话切换、上下文装载都在 ChatBridge 里。
FocusScope {
    id: root
    anchors.fill: parent
    visible: open
    z: 90
    focus: open

    property var controller: null
    property bool open: false
    property double currentThreadId: 0
    property int currentKind: 1
    //范围：默认只搜当前会话（从聊天页进来时一定有），没有当前会话才退到全部
    property bool currentOnly: true
    readonly property string query: input.text.trim()
    readonly property int resultCount: controller ? controller.messageSearchCount : 0
    signal closeRequested()

    onOpenChanged: {
        if (open) {
            currentOnly = currentThreadId > 0
            input.setText("")
            if (controller)
                controller.clearMessageSearch()
            Qt.callLater(input.forceInputFocus)
        } else {
            debounce.stop()
            if (controller)
                controller.clearMessageSearch()
        }
    }

    function runSearch() {
        if (!controller)
            return
        if (query.length === 0) {
            controller.clearMessageSearch()
            return
        }
        controller.searchMessages(query, currentOnly)
    }

    function chooseScope(only) {
        if (currentOnly === only)
            return
        currentOnly = only
        if (query.length > 0)
            runSearch()
        Qt.callLater(input.forceInputFocus)
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
        width: Math.min(700, parent.width - 48)
        height: Math.min(560, parent.height - 32)
        radius: Theme.dialogRadius
        color: Theme.surfaceFloating
        border.width: 1
        border.color: Theme.line

        //面板自身吞掉点击，避免点输入区时被背景层关闭
        MouseArea { anchors.fill: parent }

        Item {
            id: head
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 20
            height: 38

            SearchField {
                id: input
                anchors.left: parent.left
                anchors.right: closeButton.left
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                placeholder: "搜索消息内容"
                inputObjectName: "messageSearchInput"
                onTextEdited: debounce.restart()
                onAccepted: root.runSearch()
                onEscapePressed: root.closeRequested()
                onClearRequested: {
                    input.setText("")
                    if (root.controller)
                        root.controller.clearMessageSearch()
                    debounce.stop()
                    input.forceInputFocus()
                }
            }

            Rectangle {
                id: closeButton
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: 34
                height: 34
                radius: 8
                color: closeArea.containsMouse ? Theme.panelAlt : "transparent"
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: "关闭消息搜索"
                Keys.onReturnPressed: root.closeRequested()
                Keys.onSpacePressed: root.closeRequested()
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

        Row {
            id: scopeRow
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: head.bottom
            anchors.leftMargin: 20
            anchors.rightMargin: 20
            anchors.topMargin: 12
            spacing: 8

            Rectangle {
                width: 78
                height: 28
                radius: 14
                enabled: root.currentThreadId > 0
                opacity: enabled ? 1 : 0.45
                color: root.currentOnly ? Theme.selected : "transparent"
                border.width: 1
                border.color: root.currentOnly ? Theme.primary : Theme.line
                Text {
                    anchors.centerIn: parent
                    text: "当前会话"
                    font.pixelSize: Theme.fontCaption
                    color: root.currentOnly ? Theme.primary : Theme.textSecondary
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.chooseScope(true)
                }
            }
            Rectangle {
                width: 78
                height: 28
                radius: 14
                color: !root.currentOnly ? Theme.selected : "transparent"
                border.width: 1
                border.color: !root.currentOnly ? Theme.primary : Theme.line
                Text {
                    anchors.centerIn: parent
                    text: "全部会话"
                    font.pixelSize: Theme.fontCaption
                    color: !root.currentOnly ? Theme.primary : Theme.textSecondary
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.chooseScope(false)
                }
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: root.query.length > 0 ? root.resultCount + " 条结果" : "仅搜索本机已同步文本"
                font.pixelSize: Theme.fontCaption
                color: Theme.textHint
            }
        }

        Rectangle {
            id: divider
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: scopeRow.bottom
            anchors.leftMargin: 20
            anchors.rightMargin: 20
            anchors.topMargin: 10
            height: 1
            color: Theme.line
        }

        Text {
            anchors.centerIn: resultList
            visible: root.query.length === 0
            text: "输入关键词搜索消息"
            font.pixelSize: Theme.fontSmall
            color: Theme.textHint
        }
        Text {
            anchors.centerIn: resultList
            visible: root.query.length > 0 && root.resultCount === 0
            text: "没有找到相关消息"
            font.pixelSize: Theme.fontSmall
            color: Theme.textHint
        }

        ListView {
            id: resultList
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: divider.bottom
            anchors.bottom: parent.bottom
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            anchors.topMargin: 8
            anchors.bottomMargin: 12
            clip: true
            spacing: 2
            model: root.controller ? root.controller.messageSearchResults : []
            // 右侧留 12px 给滚动条，行与时间都不被压住
            ScrollBar.vertical: ScrollBar { width: 8 }

            delegate: Rectangle {
                width: ListView.view.width - 12
                height: 66
                radius: 8
                color: rowArea.containsMouse ? Theme.selected : "transparent"

                Column {
                    anchors.left: parent.left
                    anchors.leftMargin: 12
                    anchors.right: rowTime.left
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 5

                    Text {
                        width: parent.width
                        text: modelData.threadTitle
                        textFormat: Text.PlainText
                        font.pixelSize: Theme.fontSmall
                        font.weight: Font.Medium
                        color: Theme.textPrimary
                        elide: Text.ElideRight
                    }
                    Text {
                        width: parent.width
                        text: (modelData.isGroup ? modelData.senderName + "：" : "")
                              + modelData.snippet
                        textFormat: Text.PlainText
                        font.pixelSize: Theme.fontCaption
                        color: Theme.textSecondary
                        elide: Text.ElideRight
                    }
                }

                Text {
                    id: rowTime
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    text: modelData.timeText
                    font.pixelSize: Theme.fontCaption
                    color: Theme.textHint
                }

                MouseArea {
                    id: rowArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        if (!root.controller)
                            return
                        root.controller.openMessageResult(modelData.threadId, modelData.kind,
                                                          modelData.peerUid, modelData.messageId)
                        root.closeRequested()
                    }
                }
            }
        }
    }

    //面板空白区吞掉滚轮；结果列表自己的滚动仍交给 Flickable，聊天页由 ChatPage 锁住
    WheelHandler {
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        blocking: true
    }

    //输入防抖：连续打字只查最后一次
    Timer {
        id: debounce
        interval: 220
        onTriggered: root.runSearch()
    }
}
