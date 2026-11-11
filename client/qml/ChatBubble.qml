import QtQuick

// 文字按自然宽度排版，图片优先用消息里的尺寸，缺失时读图片自身大小。
// 自己发的消息靠右，状态行只在文字消息左侧出现。
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
    readonly property bool localImage: imageSource.length > 0
    //图片消息但本地还没有资源：显示占位，等资源下载/上传完成后再换成图片
    readonly property bool imagePending: isImage && !localImage
    readonly property real paddingH: localImage ? 5 : 15
    readonly property real paddingV: localImage ? 5 : 11
    readonly property real availableWidth: Math.max(1, Math.min(Theme.bubbleMaxWidth, width - 102))
    readonly property real pictureWidth: imageWidth > 0 ? imageWidth
        : (preview.sourceSize.width > 0 ? preview.sourceSize.width : 200)
    readonly property real pictureRatio: {
        if (imageWidth > 0 && imageHeight > 0)
            return imageHeight / imageWidth
        if (preview.sourceSize.width > 0 && preview.sourceSize.height > 0)
            return preview.sourceSize.height / preview.sourceSize.width
        return 0.66
    }
    readonly property real contentWidth: localImage
        ? Math.max(1, Math.min(pictureWidth, availableWidth - paddingH * 2))
        : Math.max(1, Math.min(Math.ceil(metrics.advanceWidth), availableWidth - paddingH * 2))
    readonly property real bodyWidth: Math.max(36, contentWidth + paddingH * 2)
    readonly property real bodyHeight: localImage
        ? preview.height + paddingV * 2
        : (imagePending ? 96 : label.implicitHeight + paddingV * 2)
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

    Rectangle {
        id: body
        visible: !root.imagePending
        x: root.isSelf ? userAvatar.x - 11 - width : userAvatar.width + 11
        width: root.bodyWidth
        height: root.bodyHeight
        radius: 9
        color: root.bubbleColor
        border.width: root.localImage || root.isSelf ? 0 : 1
        border.color: Theme.line
        Behavior on color { ColorAnimation { duration: 160 } }

        Image {
            id: preview
            visible: root.localImage
            x: root.paddingH
            y: root.paddingV
            width: root.contentWidth
            height: root.localImage ? width * root.pictureRatio : 0
            source: root.imageSource
            fillMode: Image.PreserveAspectFit
        }

        TextEdit {
            id: label
            x: root.paddingH
            y: root.paddingV
            width: root.contentWidth
            visible: !root.localImage
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

    //图片还没有本地资源时用中性占位，避免显示成空文本气泡
    Rectangle {
        visible: root.imagePending
        x: root.isSelf ? userAvatar.x - 11 - width : userAvatar.width + 11
        width: 150
        height: 96
        radius: 9
        color: Theme.fieldBg
        border.width: 1
        border.color: Theme.line

        Text {
            anchors.centerIn: parent
            text: "图片"
            color: Theme.textHint
            font.pixelSize: 12
        }
    }

    // 状态行：0 发送中（转圈）、1 已发送、2 失败、3 未读、4 已读。
    // 图片不在文字状态行里展示上传进度，只有失败时提示
    Row {
        visible: root.isSelf && (!root.isImage || root.status === 2)
        spacing: 3
        anchors.right: body.left
        anchors.rightMargin: 7
        anchors.verticalCenter: body.verticalCenter

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
