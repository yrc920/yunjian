import QtQuick
import YunJian

// 好友标签编辑器：已选标签可点 × 移除，回车或点预设标签增删。
// 申请好友与编辑好友资料共用同一套交互，避免两处行为不一致。
Column {
    id: root

    // 当前已选标签，由父级持有；每次变化通过 committed 回传新数组
    property var tags: []
    property var presets: ["同学", "家人", "同事", "朋友", "游戏伙伴", "客户",
                           "读书", "运动", "旅行", "音乐", "电影", "其他"]
    property int maxTags: 10
    property int maxTagLength: 12

    signal committed(var list)

    spacing: 10
    width: parent ? parent.width : implicitWidth

    function forceInputFocus() {
        tagInput.forceInputFocus()
    }

    function addTag(text) {
        const value = text.trim()
        if (value.length === 0 || value.length > maxTagLength)
            return
        if (root.tags.length >= maxTags || root.tags.indexOf(value) >= 0)
            return
        root.committed(root.tags.concat([value]))
    }

    function removeTag(text) {
        const index = root.tags.indexOf(text)
        if (index < 0)
            return
        const next = root.tags.slice()
        next.splice(index, 1)
        root.committed(next)
    }

    function toggleTag(text) {
        if (root.tags.indexOf(text) >= 0)
            removeTag(text)
        else
            addTag(text)
    }

    // 已选标签：内容过长时按容器宽度收缩并省略
    Flow {
        id: selectedFlow
        width: root.width
        spacing: 7
        visible: root.tags.length > 0
        Repeater {
            model: root.tags
            delegate: Rectangle {
                width: Math.min(selectedFlow.width, selectedText.implicitWidth + 42)
                height: 29
                radius: Theme.controlRadius
                color: Theme.selected
                Text {
                    id: selectedText
                    x: 10
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - 35
                    text: modelData
                    textFormat: Text.PlainText
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontSmall
                    elide: Text.ElideRight
                }
                AppIcon {
                    anchors.right: parent.right
                    anchors.rightMargin: 7
                    anchors.verticalCenter: parent.verticalCenter
                    name: "close"
                    size: 12
                    color: Theme.textSecondary
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.removeTag(modelData)
                }
            }
        }
    }

    // 自定义标签：回车加入，输入内容超长时输入框本身就会截断
    AppInput {
        id: tagInput
        width: root.width
        maximumLength: root.maxTagLength
        placeholder: root.tags.length >= root.maxTags ? "最多 10 个标签" : "输入标签后按回车"
        enabled: root.tags.length < root.maxTags
        onAccepted: {
            root.addTag(text)
            text = ""
        }
    }

    // 预设标签：已选的再点一次即取消
    Flow {
        width: root.width
        spacing: 7
        Repeater {
            model: root.presets
            delegate: Rectangle {
                id: presetChip
                readonly property bool chosen: root.tags.indexOf(modelData) >= 0
                width: presetLabel.implicitWidth + 22
                height: 29
                radius: Theme.controlRadius
                color: chosen ? Theme.selected : Theme.panelAlt
                border.width: 1
                border.color: chosen ? Theme.primary : "transparent"
                Text {
                    id: presetLabel
                    anchors.centerIn: parent
                    text: modelData
                    color: presetChip.chosen ? Theme.textPrimary : Theme.textSecondary
                    font.pixelSize: Theme.fontSmall
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.toggleTag(modelData)
                }
            }
        }
    }
}
