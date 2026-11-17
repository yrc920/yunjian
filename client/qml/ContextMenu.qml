import QtQuick

// 轻量右键菜单：条目由调用方给出 {text, enabled, action}，点击后先关闭再执行。
// 覆盖整个父项，菜单外的点击或 Esc 都只关闭菜单，不把事件传给下层。
Item {
    id: root

    property var items: []
    property int activeIndex: -1
    visible: false
    z: 600
    focus: visible

    function open(px, py, list) {
        items = list || []
        if (items.length === 0)
            return
        activeIndex = 0
        panel.x = Math.max(8, Math.min(px, width - panel.width - 8))
        const below = py + panel.height + 8 <= height
        panel.y = below ? py : Math.max(8, py - panel.height)
        visible = true
        forceActiveFocus()
    }

    function close() {
        visible = false
        items = []
        activeIndex = -1
    }

    function move(delta) {
        if (items.length === 0)
            return
        let next = activeIndex
        for (let i = 0; i < items.length; ++i) {
            next = (next + delta + items.length) % items.length
            if (items[next].enabled !== false)
                break
        }
        activeIndex = next
    }

    function activate(index) {
        if (index < 0 || index >= items.length)
            return
        const item = items[index]
        if (!item || item.enabled === false)
            return
        const action = item.action
        close()
        if (action)
            action()
    }

    Keys.onEscapePressed: close()
    Keys.onUpPressed: move(-1)
    Keys.onDownPressed: move(1)
    Keys.onReturnPressed: activate(activeIndex)
    Keys.onEnterPressed: activate(activeIndex)

    MouseArea {
        anchors.fill: parent
        onClicked: root.close()
        onWheel: root.close()
    }

    Rectangle {
        id: panel
        width: 150
        height: column.implicitHeight + 12
        radius: 9
        color: Theme.panelBg
        border.width: 1
        border.color: Theme.line

        Column {
            id: column
            x: 4
            y: 6
            width: parent.width - 8
            spacing: 1

            Repeater {
                model: root.items

                delegate: Rectangle {
                    width: column.width
                    height: 31
                    radius: 5
                    readonly property bool highlighted: index === root.activeIndex
                    color: (itemArea.containsMouse || highlighted) && modelData.enabled !== false
                        ? Theme.panelAlt : "transparent"
                    opacity: modelData.enabled === false ? 0.4 : 1

                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 10
                        anchors.right: parent.right
                        anchors.rightMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData.text || ""
                        color: Theme.textPrimary
                        font.pixelSize: 12
                        elide: Text.ElideRight
                    }

                    MouseArea {
                        id: itemArea
                        anchors.fill: parent
                        hoverEnabled: true
                        enabled: modelData.enabled !== false
                        cursorShape: Qt.PointingHandCursor
                        onEntered: root.activeIndex = index
                        onClicked: root.activate(index)
                    }
                }
            }
        }
    }
}
