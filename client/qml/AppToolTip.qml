import QtQuick
import QtQuick.Controls

// 全应用唯一的悬停提示：停留 450ms 出现，3s 自动收起，离开/切页/窗口失焦立即收起。
// 文字只走一条链路：Popup 的 padding + 可换行 Text，不再手动摆 x/y，避免文字跑出气泡。
Popup {
    id: root

    property string text: ""
    property bool hovered: false
    property int showDelay: 450
    property int autoHide: 3000
    property int maxTextWidth: 220

    // Popup 是窗口级图层，parent 指向它所描述的控件；x/y 相对父项。
    padding: 9
    margins: 0
    modal: false
    focus: false
    dim: false
    closePolicy: Popup.NoAutoClose
    visible: false
    width: Math.max(56, Math.min(Math.ceil(metrics.advanceWidth), maxTextWidth) + 2 * padding)

    function dismiss() {
        showTimer.stop()
        hideTimer.stop()
        visible = false
    }

    function reposition() {
        const target = root.parent
        if (!target)
            return
        const origin = target.mapToItem(null, 0, 0)
        const win = target.Window.window
        const winW = win ? win.width : origin.x + target.width
        const winH = win ? win.height : origin.y + target.height
        let px = origin.x + (target.width - root.width) / 2
        let py = origin.y - root.height - 8
        // 上方放不下就放到下方；两侧与上下都夹在窗口内
        if (py < 8)
            py = origin.y + target.height + 8
        px = Math.max(8, Math.min(px, winW - root.width - 8))
        py = Math.max(8, Math.min(py, winH - root.height - 8))
        root.x = px - origin.x
        root.y = py - origin.y
    }

    TextMetrics {
        id: metrics
        font: label.font
        text: root.text
    }

    Timer {
        id: showTimer
        interval: root.showDelay
        onTriggered: {
            if (!root.hovered || root.text.length === 0)
                return
            if (!root.parent || !root.parent.visible || !root.parent.enabled)
                return
            root.visible = true
            hideTimer.restart()
        }
    }
    Timer {
        id: hideTimer
        interval: root.autoHide
        onTriggered: root.visible = false
    }
    // 父项被切页或禁用时，最多半秒内收起，避免提示浮在新页面上
    Timer {
        interval: 500
        repeat: true
        running: root.visible
        onTriggered: {
            if (!root.parent || !root.parent.visible || !root.parent.enabled)
                root.dismiss()
        }
    }

    onHoveredChanged: {
        if (hovered) {
            hideTimer.stop()
            if (visible)
                hideTimer.start()
            else
                showTimer.start()
        } else {
            dismiss()
        }
    }
    onTextChanged: if (visible)
        hideTimer.restart()
    onVisibleChanged: {
        if (visible) {
            Qt.callLater(reposition)
        } else {
            showTimer.stop()
            hideTimer.stop()
        }
    }
    onHeightChanged: if (visible)
        Qt.callLater(reposition)
    onWidthChanged: if (visible)
        Qt.callLater(reposition)
    Component.onCompleted: if (hovered && text.length > 0)
        showTimer.start()

    background: Rectangle {
        color: Theme.dark ? "#202B35" : "#1B2A38"
        radius: 8
        border.width: 1
        border.color: Theme.dark ? "#34414D" : "#2A3B4C"
    }

    contentItem: Text {
        id: label
        width: root.availableWidth
        text: root.text
        textFormat: Text.PlainText
        color: Theme.dark ? "#E9EEF3" : "#F7F3E9"
        font.pixelSize: Theme.fontSmall
        font.weight: Font.Medium
        wrapMode: Text.WrapAtWordBoundaryOrAnywhere
        lineHeight: 1.3
        verticalAlignment: Text.AlignVCenter
    }

    // 窗口失焦时收起
    Connections {
        target: Qt.application
        function onStateChanged() {
            if (Qt.application.state !== Qt.ApplicationActive)
                root.dismiss()
        }
    }
}
