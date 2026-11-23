import QtQuick

// 书脊顶端的书签：从窗口上沿垂下的一小段布带，底端留一个缺口。
// 只负责占位与气质，不带文字、不做导航，避免被读成应用 logo。
Canvas {
    id: root

    property color color: Theme.spineMuted
    property real bandWidth: 16
    property real bandHeight: 38

    implicitWidth: bandWidth
    implicitHeight: bandHeight

    onColorChanged: requestPaint()
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()

    onPaint: {
        const c = getContext("2d")
        c.reset()
        const w = width
        const h = height
        const notch = Math.min(9, h * 0.26)
        // 左右各留 0.6：描边完整落在画布内，视觉中心才和书脊中心重合
        const inset = 0.6
        c.beginPath()
        c.moveTo(inset, 0)
        c.lineTo(w - inset, 0)
        c.lineTo(w - inset, h)
        c.lineTo(w / 2, h - notch)
        c.lineTo(inset, h)
        c.closePath()
        c.fillStyle = Qt.rgba(root.color.r, root.color.g, root.color.b, 0.14)
        c.fill()
        c.strokeStyle = Qt.rgba(root.color.r, root.color.g, root.color.b, 0.5)
        c.lineWidth = 1.1
        c.stroke()

        // 布带中间的压线，让它在纯色书脊上不是一块色块
        c.beginPath()
        c.moveTo(w / 2, 3)
        c.lineTo(w / 2, h - notch - 3)
        c.strokeStyle = Qt.rgba(root.color.r, root.color.g, root.color.b, 0.2)
        c.lineWidth = 1
        c.stroke()
    }
}
