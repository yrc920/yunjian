import QtQuick
import QtQuick.Controls
import XiguChat

// 多选转发目标：列出已有会话（含群聊），选中即转发到该会话。
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
    property string filter: ""
    signal targetChosen(double threadId, int kind, int uid)
    signal closeRequested()

    readonly property var allRows: {
        if (!controller)
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

    onOpenChanged: {
        if (open) {
            filter = ""
            input.setText("")
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
        width: Math.min(420, parent.width - 48)
        height: Math.min(520, parent.height - 40)
        radius: 12
        color: Theme.panelBg
        border.width: 1
        border.color: Theme.line

        //吞掉面板内的点击，避免点到空白处关闭
        MouseArea { anchors.fill: parent }

        Item {
            id: head
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 18
            height: 32

            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: (root.mergeMode ? "合并转发 " : "转发 ") + root.count + " 条消息到"
                font.pixelSize: 15
                font.bold: true
                color: Theme.textPrimary
            }

            Rectangle {
                id: closeButton
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: 30
                height: 30
                radius: 6
                color: closeArea.containsMouse ? Theme.panelAlt : "transparent"
                AppIcon {
                    anchors.centerIn: parent
                    name: "close"
                    size: 14
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
            anchors.leftMargin: 18
            anchors.rightMargin: 18
            anchors.topMargin: 12
            placeholder: "搜索会话"
            inputObjectName: "forwardSearchInput"
            onTextEdited: (value) => root.filter = value
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
            anchors.leftMargin: 18
            anchors.rightMargin: 18
            anchors.topMargin: 10
            height: 1
            color: Theme.line
        }

        Text {
            anchors.centerIn: list
            visible: root.filteredRows.length === 0
            text: root.filter.length > 0 ? "没有匹配的会话" : "还没有可转发的会话"
            font.pixelSize: 11
            color: Theme.textHint
        }

        ListView {
            id: list
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: divider.bottom
            anchors.bottom: parent.bottom
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            anchors.topMargin: 6
            anchors.bottomMargin: 10
            clip: true
            spacing: 2
            model: root.filteredRows
            ScrollBar.vertical: ScrollBar {}

            delegate: Rectangle {
                width: ListView.view.width
                height: 54
                radius: 8
                color: rowArea.containsMouse ? Theme.selected : "transparent"

                Avatar {
                    id: rowAvatar
                    anchors.left: parent.left
                    anchors.leftMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    width: 34
                    height: 34
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
                        font.pixelSize: 12
                        color: Theme.textPrimary
                        elide: Text.ElideRight
                    }
                    Text {
                        width: parent.width
                        text: modelData.kind === 2
                            ? (modelData.memberCount > 0
                                ? "群聊 · " + modelData.memberCount + " 人" : "群聊")
                            : "单聊"
                        font.pixelSize: 10
                        color: Theme.textHint
                    }
                }

                MouseArea {
                    id: rowArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        root.targetChosen(modelData.threadId, modelData.kind, modelData.uid)
                        root.closeRequested()
                    }
                }
            }
        }
    }
}
