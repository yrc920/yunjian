import QtQuick

// 轻量右键菜单：条目由调用方给出 {text, icon, enabled, action}，点击后先关闭再执行。
// 覆盖整个父项，菜单外的点击或 Esc 都只关闭菜单，不把事件传给下层。
Item {
    id: root

    property var items: []
    property int activeIndex: -1
    // 宽度按最长一项算，和加号菜单同一套：左右各 12
    property real panelWidth: 120
    // 关闭后通知调用方同步自己的开关状态；点击条目、按 Esc、点外部都会走到这里
    signal closed()
    visible: false
    z: 600
    focus: visible

    function open(px, py, list) {
        items = list || []
        if (items.length === 0)
            return
        let widest = 0
        let hasIcon = false
        for (let i = 0; i < items.length; ++i) {
            widest = Math.max(widest, metrics.advanceWidth(String(items[i].text || "")))
            if (items[i].icon)
                hasIcon = true
        }
        // 有图标时左侧固定 16 图标 + 8 间距；左右各 12
        root.panelWidth = Math.max(72,
            Math.min(240, Math.ceil(widest) + (hasIcon ? 48 : 26)))
        // 刚打开时不高亮第一项：鼠标菜单没有默认动作，方向键再开始选
        activeIndex = -1
        panel.x = Math.max(8, Math.min(px, width - panel.width - 8))
        const below = py + panel.height + 8 <= height
        panel.y = below ? py : Math.max(8, py - panel.height)
        visible = true
        forceActiveFocus()
    }

    function close() {
        if (!visible) {
            items = []
            activeIndex = -1
            return
        }
        visible = false
        items = []
        activeIndex = -1
        closed()
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

    // FontMetrics 同步返回当前文字的宽度，不会像隐藏 Text 那样读到上一轮的旧值
    FontMetrics {
        id: metrics
        font.pixelSize: Theme.fontSmall
        font.weight: Font.Normal
    }

    Rectangle {
        id: panel
        width: root.panelWidth
        height: column.implicitHeight + 12
        radius: Theme.panelRadius
        color: Theme.surfaceFloating
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
                    height: 36
                    radius: Theme.controlRadius
                    readonly property bool highlighted: index === root.activeIndex
                    color: (itemArea.containsMouse || highlighted) && modelData.enabled !== false
                        ? Theme.selected : "transparent"
                    opacity: modelData.enabled === false ? 0.4 : 1

                    AppIcon {
                        x: 8
                        anchors.verticalCenter: parent.verticalCenter
                        name: modelData.icon || ""
                        size: 16
                        color: modelData.enabled === false ? Theme.textHint : Theme.textSecondary
                    }

                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: modelData.icon ? 32 : 10
                        anchors.right: parent.right
                        anchors.rightMargin: modelData.icon ? 4 : 8
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData.text || ""
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontSmall
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
