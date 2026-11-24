import QtQuick

// 外观预览的场景：固定 176×96，按书脊 13 / 索引 48 / 信纸 115 切分。
// 只读传入的色板，不读 Theme 的当前模式：浅色卡在深色界面里也必须是浅色。
Item {
    id: root
    property var palette: Theme.lightPalette

    width: 176
    height: 96

    Rectangle { anchors.fill: parent; color: root.palette.sheet }
    Rectangle { width: 13; height: parent.height; color: root.palette.spine }
    Rectangle { x: 13; width: 48; height: parent.height; color: root.palette.indexBg }

    // 索引里的两行：头像方块 + 名字线
    Rectangle { x: 19; y: 14; width: 10; height: 10; radius: 3; color: root.palette.selected }
    Rectangle { x: 33; y: 17; width: 22; height: 3; radius: 1.5; color: root.palette.line }
    Rectangle { x: 19; y: 42; width: 10; height: 10; radius: 3; color: root.palette.selected }
    Rectangle { x: 33; y: 45; width: 16; height: 3; radius: 1.5; color: root.palette.line }

    // 信纸横线与一收一发两枚气泡
    Rectangle { x: 68; y: 20; width: 100; height: 1; color: root.palette.lineSoft }
    Rectangle { x: 68; y: 84; width: 100; height: 1; color: root.palette.lineSoft }
    Rectangle {
        x: 72; y: 32; width: 60; height: 14; radius: 7
        color: root.palette.bubbleOther
        border.width: 1
        border.color: root.palette.line
    }
    Rectangle { x: 100; y: 58; width: 56; height: 14; radius: 7; color: root.palette.bubbleSelf }
}
