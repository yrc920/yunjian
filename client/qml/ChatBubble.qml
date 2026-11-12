import QtQuick
import QtQuick.Effects

// 文本用气泡，图片直接显示圆角缩略图；自己发的消息左侧留状态位。
// 图片上方没有百分比数字，只在底部叠一条随上传推进的细进度条，完成后整条消失。
Item {
    id: root

    property string avatar: ""
    property string name: ""
    property string text: ""
    property bool isSelf: false
    property bool isImage: false
    property string imageSource: ""
    property int imageWidth: 0
    property int imageHeight: 0
    property int status: 1
    property int transferProgress: -1

    readonly property bool localImage: imageSource.length > 0
    //图片消息但本地还没有缩略图：显示占位，等上传/下载完成后再换成图片
    readonly property bool imagePending: isImage && !localImage
    //上传和下载共用同一条进度：>=0 时显示，100 或 -1 时隐藏
    readonly property bool transferring: isImage && transferProgress >= 0 && transferProgress < 100
    //缩略图显示上限；窗口变窄时跟着收，避免压到头像或左侧状态位
    readonly property real imageMaxWidth: Math.max(72, Math.min(220, width - 102))
    readonly property real imageMaxHeight: 180
    readonly property real availableWidth: Math.max(1, Math.min(Theme.bubbleMaxWidth, width - 102))

    //优先用消息里保存的缩略图像素尺寸；缺失时读已加载的图片，最后才用占位尺寸
    readonly property real sourceWidth: imageWidth > 0 ? imageWidth
        : (preview.sourceSize.width > 0 ? preview.sourceSize.width : 150)
    readonly property real sourceHeight: imageHeight > 0 ? imageHeight
        : (preview.sourceSize.height > 0 ? preview.sourceSize.height : 96)
    readonly property real pictureScale: Math.min(1,
        Math.min(imageMaxWidth / Math.max(1, sourceWidth), imageMaxHeight / Math.max(1, sourceHeight)))
    readonly property real pictureWidth: Math.max(48, sourceWidth * pictureScale)
    readonly property real pictureHeight: Math.max(36, sourceHeight * pictureScale)

    readonly property real textWidth: Math.max(1,
        Math.min(Math.ceil(metrics.advanceWidth), Math.max(1, availableWidth - 30)))
    readonly property real bubbleWidth: Math.max(36, textWidth + 30)
    readonly property real bubbleHeight: label.implicitHeight + 22
    readonly property real bodyWidth: isImage ? pictureWidth : bubbleWidth
    readonly property real bodyHeight: isImage ? pictureHeight : bubbleHeight
    readonly property color bubbleColor: isSelf ? Theme.bubbleSelf : Theme.bubbleOther

    implicitHeight: Math.max(34, bodyHeight)

    TextMetrics {
        id: metrics
        font: label.font
        text: root.text
    }

    Avatar {
        id: userAvatar
        x: root.isSelf ? root.width - width : 0
        width: 32
        height: 32
        radius: 10
        source: root.avatar
        name: root.name
    }

    // 文字气泡：只有文本消息才有底色、边框和内边距
    Rectangle {
        id: textBubble
        visible: !root.isImage
        x: root.isSelf ? userAvatar.x - 11 - width : userAvatar.width + 11
        y: 0
        width: root.bubbleWidth
        height: root.bubbleHeight
        radius: 9
        color: root.bubbleColor
        border.width: root.isSelf ? 0 : 1
        border.color: Theme.line
        Behavior on color { ColorAnimation { duration: 160 } }

        TextEdit {
            id: label
            x: 15
            y: 11
            width: root.textWidth
            readOnly: true
            selectByMouse: true
            selectionColor: Theme.selected
            selectedTextColor: Theme.textPrimary
            text: root.text
            textFormat: TextEdit.PlainText
            wrapMode: TextEdit.WrapAtWordBoundaryOrAnywhere
            font.pixelSize: Theme.fontBody
            color: root.isSelf ? Theme.bubbleSelfText : Theme.textPrimary
        }
    }

    // 图片容器：没有气泡底色和文字内边距，仅保留圆角裁切
    Rectangle {
        id: pictureFrame
        visible: root.isImage
        x: root.isSelf ? userAvatar.x - 11 - width : userAvatar.width + 11
        y: 0
        width: root.pictureWidth
        height: root.pictureHeight
        radius: 10
        color: Theme.fieldBg
        border.width: root.localImage ? 0 : 1
        border.color: Theme.line
    }

    Image {
        id: preview
        anchors.fill: pictureFrame
        source: root.imageSource
        visible: root.localImage //圆角由下面的蒙版裁出来，软件渲染时退化为直角
        fillMode: Image.PreserveAspectCrop //尺寸与缩略图一致时等于原样铺满，尺寸缺失时也不会留白
        asynchronous: true
        cache: true
        layer.enabled: GraphicsInfo.api !== GraphicsInfo.Software
        layer.effect: MultiEffect {
            maskEnabled: true
            maskSource: pictureMask
        }
    }

    // 圆角蒙版：与 Avatar 共用同一套做法，白色圆角矩形提供 alpha
    Rectangle {
        id: pictureMask
        anchors.fill: pictureFrame
        radius: pictureFrame.radius
        color: "white"
        visible: false
        layer.enabled: true
    }

    // 图片还没到本机时的占位，不显示文件名
    Column {
        visible: root.imagePending
        anchors.centerIn: pictureFrame
        spacing: 6
        AppIcon {
            anchors.horizontalCenter: parent.horizontalCenter
            name: "image"
            size: 20
            color: Theme.textHint
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "图片"
            color: Theme.textHint
            font.pixelSize: 11
        }
    }

    // 图片传输进度：上传和下载共用，贴在图片/占位底部推进，完成后自动隐藏
    Rectangle {
        id: progressTrack
        visible: root.transferring
        anchors.left: pictureFrame.left
        anchors.right: pictureFrame.right
        anchors.bottom: pictureFrame.bottom
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        anchors.bottomMargin: 10
        height: 3
        radius: 1.5
        color: Qt.rgba(1, 1, 1, 0.55)

        Rectangle {
            width: parent.width * Math.max(0, Math.min(100, root.transferProgress)) / 100
            height: parent.height
            radius: parent.radius
            color: Theme.primary
            Behavior on width { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
        }
    }

    // 状态行：0 发送中（转圈）、1 已发送、2 失败；文本和图片共用同一套图标
    Row {
        visible: root.isSelf
        spacing: 3
        anchors.right: root.isImage ? pictureFrame.left : textBubble.left
        anchors.rightMargin: 7
        anchors.verticalCenter: root.isImage ? pictureFrame.verticalCenter : textBubble.verticalCenter

        AppIcon {
            visible: root.status === 0
            name: "reset"
            size: 14
            color: Theme.textHint
            RotationAnimation on rotation {
                running: root.status === 0
                loops: Animation.Infinite
                duration: 900
            }
        }
        AppIcon {
            visible: root.status === 1
            name: "check"
            size: 14
            color: Theme.primary
        }
        AppIcon {
            visible: root.status === 2
            name: "close"
            size: 14
            color: Theme.danger
        }
        Text {
            visible: root.status === 3
            text: "未读"
            color: Theme.textHint
            font.pixelSize: 10
        }
        Text {
            visible: root.status === 4
            text: "已读"
            color: Theme.primary
            font.pixelSize: 10
        }
    }
}
