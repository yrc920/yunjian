import QtQuick

// 信纸横线：只画 1px、只有 4.5% 墨色，滚动时是质感，不是表格。
// 尺寸变化或主题切换时重绘一次，不逐帧绘制。
Canvas {
    id: root
    property int step: 32
    antialiasing: false

    onPaint: {
        const c = getContext("2d")
        c.reset()
        c.strokeStyle = Theme.ruleLine
        c.lineWidth = 1
        for (let y = step; y < height; y += step) {
            c.beginPath()
            c.moveTo(0, y + 0.5)
            c.lineTo(width, y + 0.5)
            c.stroke()
        }
    }
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()
    Component.onCompleted: requestPaint()

    Connections {
        target: Theme
        function onDarkChanged() { root.requestPaint() }
    }
}
