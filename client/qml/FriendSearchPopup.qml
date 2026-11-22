import QtQuick
import XiguChat

// 聊天页的好友过滤结果：浮在会话列表上方，只读本地好友，不发服务端搜索。
Rectangle {
    id: root

    required property ChatBridge controller
    signal addFriendRequested()

    readonly property var matches: controller.friendMatches

    height: matches.length === 0 ? 84 : Math.min(matches.length, 5) * 52 + 12
    radius: 9
    color: Theme.surfaceFloating
    border.width: 1
    border.color: Theme.line
    clip: true

    // 空白区域也吃掉点击，避免穿透到下面的列表遮罩
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
    }

    ListView {
        id: resultList
        visible: root.matches.length > 0
        anchors.fill: parent
        anchors.margins: 6
        model: root.matches
        clip: true
        spacing: 2
        boundsBehavior: Flickable.StopAtBounds
        interactive: contentHeight > height

        delegate: Item {
            width: resultList.width
            height: 52

            Rectangle {
                anchors.fill: parent
                radius: 6
                color: rowArea.containsMouse ? Theme.selected : "transparent"
            }
            Avatar {
                x: 8
                width: 30
                height: 30
                radius: 9
                anchors.verticalCenter: parent.verticalCenter
                source: modelData.icon || ""
                name: modelData.displayName || modelData.name || ""
                textSize: 12
            }
            Column {
                anchors.left: parent.left
                anchors.leftMargin: 48
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                spacing: 3

                Text {
                    width: parent.width
                    text: modelData.displayName || modelData.name || ""
                    textFormat: Text.PlainText
                    color: Theme.textPrimary
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
                Text {
                    width: parent.width
                    text: modelData.lastText || "还没有消息"
                    textFormat: Text.PlainText
                    color: Theme.textSecondary
                    font.pixelSize: 10
                    elide: Text.ElideRight
                    maximumLineCount: 1
                }
            }
            MouseArea {
                id: rowArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.controller.openChat(modelData.uid)
            }
        }
    }

    Column {
        visible: root.matches.length === 0
        anchors.centerIn: parent
        width: parent.width - 24
        spacing: 6

        Text {
            width: parent.width
            text: "没有找到这位好友"
            horizontalAlignment: Text.AlignHCenter
            color: Theme.textSecondary
            font.pixelSize: 11
        }
        AppLink {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "去联系人添加"
            onClicked: root.addFriendRequested()
        }
    }
}
