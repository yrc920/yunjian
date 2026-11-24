import QtQuick
import QtQuick.Effects

// 有头像时显示图片；没有头像时用名字回退：
// 单聊取首字，群聊取前两字并按名字/thread 派生一个稳定的底色，
// 这样折叠成头像条、或好几个群名字都以"群"开头时仍然分得开。
Rectangle {
    id: root
    property string source: ""
    property string name: ""
    property bool group: false
    property bool showGroupBadge: true //群资料页由相机角标接管时不画
    property color textColor: Theme.ink
    property int textSize: 13

    // 小尺寸只放一个字：34px 以下两个方字会挤在一起
    readonly property int fallbackChars: group ? (width >= 40 ? 2 : 1) : 1

    // 名字/种子派生色相：只动色相，饱和度与明度固定；深色抬到 .42，避免群头像变成脏色块
    function hueOf(text) {
        let hash = 0
        for (let i = 0; i < text.length; ++i)
            hash = (hash * 31 + text.charCodeAt(i)) % 360
        return hash / 360
    }

    radius: width * 0.24
    color: group
        ? Qt.hsla(hueOf(name), 0.30, Theme.dark ? 0.42 : 0.78, 1)
        : Theme.selected

    Text {
        anchors.centerIn: parent
        visible: picture.status !== Image.Ready
        text: root.name.length > 0
            ? root.name.substring(0, root.fallbackChars) : "?"
        color: Theme.dark && root.group ? "#F2F5F8" : root.textColor
        font.pixelSize: root.group ? Math.round(root.width * 0.30) : root.textSize
        font.weight: root.group ? Font.DemiBold : Font.Normal
        font.letterSpacing: root.group && root.fallbackChars > 1 ? -1 : 0
    }

    // 群标记：和单聊区分开，尺寸太小（<32）时不画，避免糊成一团
    Rectangle {
        visible: root.group && root.showGroupBadge && root.width >= 44
            && picture.status === Image.Ready
        width: Math.round(root.width * 0.30)
        height: width
        radius: width / 2
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 0
        color: Theme.panelBg
        border.width: 1
        border.color: Theme.line

        Text {
            anchors.centerIn: parent
            text: "群"
            color: Theme.primary
            font.pixelSize: Math.max(8, Math.round(parent.width * 0.58))
            font.weight: Font.Medium
        }
    }

    Image {
        id: picture
        anchors.fill: parent
        source: root.source.startsWith(":/") ? "qrc" + root.source : root.source
        fillMode: Image.PreserveAspectCrop
        visible: status === Image.Ready
        layer.enabled: GraphicsInfo.api !== GraphicsInfo.Software
        layer.effect: MultiEffect {
            maskEnabled: true
            maskSource: mask
        }
    }

    Rectangle {
        id: mask
        anchors.fill: parent
        radius: root.radius
        visible: false
        layer.enabled: true
    }
}
