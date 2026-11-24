import QtQuick
import XiguChat

// 标签筛选浮层：标签条放不下时从「更多」展开，避免把一行挤成横向滚动。
// 选中即生效并关闭；点浮层空白或按 Esc 收起。
Item {
    id: root

    required property ChatBridge controller
    signal tagChosen(string tag)
    signal dismissed()

    focus: true
    Keys.onEscapePressed: root.dismissed()

    readonly property int rowHeight: 37
    // 首项固定是「全部」，后面是全部标签；用同一种行结构，选中判断只看 tag 是否为空
    readonly property var rows: {
        const list = [{ tag: "", label: "全部" }]
        const options = controller.tagOptions
        for (let i = 0; i < options.length; ++i)
            list.push({ tag: options[i], label: options[i] })
        return list
    }

    implicitHeight: Math.min(340, 46 + rows.length * rowHeight + 8)
    height: implicitHeight

    Rectangle {
        anchors.fill: parent
        radius: Theme.panelRadius
        color: Theme.surfaceFloating
        border.width: 1
        border.color: Theme.line
    }

    // 空白区域负责收起；列表行在它上层，点击不会被这里截走
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onClicked: root.dismissed()
    }

    Text {
        id: title
        x: 14
        y: 12
        text: "按标签筛选"
        color: Theme.textSecondary
        font.pixelSize: Theme.fontSmall
    }
    Text {
        anchors.right: parent.right
        anchors.rightMargin: 14
        anchors.baseline: title.baseline
        text: root.rows.length - 1 + " 个标签"
        color: Theme.textHint
        font.pixelSize: Theme.fontCaption
    }

    Flickable {
        id: listView
        x: 8
        y: 34
        width: parent.width - 16
        height: Math.min(parent.height - 42, contentHeight)
        contentHeight: listColumn.height
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: listColumn
            width: listView.width
            spacing: 1

            Repeater {
                model: root.rows

                delegate: Rectangle {
                    id: rowItem
                    width: listColumn.width
                    height: root.rowHeight
                    radius: Theme.controlRadius
                    color: rowArea.containsMouse ? Theme.selected : "transparent"
                    readonly property bool chosen: root.controller.tagFilter === modelData.tag

                    Text {
                        x: 10
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - 44
                        text: modelData.label
                        textFormat: Text.PlainText
                        color: rowItem.chosen ? Theme.textPrimary : Theme.textSecondary
                        font.pixelSize: Theme.fontBody
                        font.weight: rowItem.chosen ? Font.DemiBold : Font.Normal
                        elide: Text.ElideRight
                    }
                    AppIcon {
                        visible: rowItem.chosen
                        anchors.right: parent.right
                        anchors.rightMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        name: "check"
                        size: 14
                        color: Theme.primary
                    }
                    MouseArea {
                        id: rowArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.tagChosen(modelData.tag)
                    }
                }
            }
        }
    }
}
