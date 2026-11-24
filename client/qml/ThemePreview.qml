import QtQuick
import QtQuick.Effects

// 外观卡的缩略图：浅/深色各画一张场景，跟随系统则左右各取一半。
// 场景固定 176×96 后整体缩放，窄窗里也不会糊；圆角用 MultiEffect 蒙版，避免书脊方角顶出圆角。
Item {
    id: root
    property string mode: "light"
    property int previewRadius: 8
    readonly property bool split: mode === "system"
    readonly property real sceneWidth: 176
    readonly property real sceneHeight: 96

    Rectangle {
        id: frame
        anchors.fill: parent
        radius: root.previewRadius
        color: "transparent"
        layer.enabled: GraphicsInfo.api !== GraphicsInfo.Software
        layer.effect: MultiEffect {
            maskEnabled: true
            maskSource: mask
        }

        Repeater {
            model: root.split ? 2 : 1
            delegate: Item {
                id: half
                width: root.split ? root.width / 2 : root.width
                height: root.height
                clip: true
                x: root.split ? index * width : 0
                readonly property bool useDark: root.mode === "dark"
                    || (root.split && index === 1)

                ThemePreviewScene {
                    width: root.sceneWidth
                    height: root.sceneHeight
                    scale: root.width / root.sceneWidth
                    transformOrigin: Item.TopLeft
                    x: root.split && index === 1 ? -root.width / 2 : 0
                    palette: half.useDark ? Theme.darkPalette : Theme.lightPalette
                }
            }
        }
    }

    Rectangle {
        id: mask
        anchors.fill: parent
        radius: root.previewRadius
        visible: false
        layer.enabled: true
    }
}
