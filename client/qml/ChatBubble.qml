import QtQuick
import QtQuick.Effects
import XiguChat

// 文本用气泡，图片直接显示圆角缩略图；自己发的消息左侧留状态位。
// 图片上方没有百分比数字，只在底部叠一条随上传推进的细进度条，完成后整条消失。
Item {
    id: root

    property string avatar: ""
    property string name: ""
    property bool showAvatar: true
    property string senderName: "" //群聊里别人发的消息：气泡上方显示成员名，单聊为空""
    property string text: ""
    property bool isSelf: false
    property bool isImage: false
    // 合并转发：气泡换成聊天记录卡片，正文不再渲染
    property bool mergeRecord: false
    property string recordTitle: ""
    property int recordCount: 0
    property var recordLines: [] //最多三条预览，形如"张三：内容"
    signal recordClicked()
    property string imageSource: ""
    property int imageWidth: 0
    property int imageHeight: 0
    property int status: 1
    property int transferProgress: -1
    property string statusText: "" //「已读」/「未读」，只对自己发的最后一条有值
    property bool retryable: false //发送失败：给出可点击的重发入口
    //群聊里这条消息 @ 了我：气泡左侧给一条 accent 竖线，翻历史时一眼能看到
    property bool mentioned: false
    //搜索命中：短暂给气泡描边，帮助用户从搜索面板跳回后立刻找到目标
    property bool highlighted: false
    signal imageClicked() //图片气泡点击后交给查看器打开原图
    signal retryRequested() //点击失败提示，重新发送这条消息
    signal contextRequested(real x, real y) //右键菜单：坐标相对当前气泡

    readonly property bool localImage: imageSource.length > 0
    //图片消息但本地还没有缩略图：显示占位，等上传/下载完成后再换成图片
    readonly property bool imagePending: isImage && !localImage
    //上传和下载共用同一条进度：>=0 时显示，100 或 -1 时隐藏
    readonly property bool transferring: isImage && transferProgress >= 0 && transferProgress < 100
    //缩略图显示上限；窗口变窄时跟着收，避免压到头像或左侧状态位
    readonly property real imageMaxWidth: Math.max(72, Math.min(220, width - 102))
    readonly property real imageMaxHeight: 180
    readonly property real availableWidth: Math.max(1, Math.min(Theme.bubbleMaxWidth, width - 102))

    //优先用消息里保存的缩略图像素尺寸；没有尺寸时用固定占位，避免图片异步加载时
    //反复改写气泡高度，长会话里会出现列表抖动和自动滚底竞争。
    //宽高必须同比例缩放，不能分别套最小值：长图/宽图会被拉成另一个比例，
    //表现为聊天里的缩略图和点开后的原图不一致
    readonly property real sourceWidth: imageWidth > 0 ? imageWidth : 150
    readonly property real sourceHeight: imageHeight > 0 ? imageHeight : 96
    readonly property real pictureScale: Math.min(1,
        Math.min(imageMaxWidth / Math.max(1, sourceWidth), imageMaxHeight / Math.max(1, sourceHeight)))
    readonly property real pictureWidth: Math.max(1, sourceWidth * pictureScale)
    readonly property real pictureHeight: Math.max(1, sourceHeight * pictureScale)

    //气泡宽度直接取文档排完版的内容宽度：表情是等宽行内对象，混排、换行、
    //连续表情都算得准，不再用 token 原文宽度去猜。
    //窗口极窄时至少留出一个表情的宽度，否则图片会画到气泡外面
    readonly property real maxTextWidth: Math.max(1, availableWidth - 30)
    readonly property real textWidth: Math.max(emojiSpots.length > 0 ? Emoji.size : 1,
        Math.min(Math.ceil(label.implicitWidth), maxTextWidth))
    readonly property real bubbleWidth: Math.max(36, textWidth + 30)
    readonly property real bubbleHeight: label.contentHeight + 22
    readonly property real recordHeight: 40 + Math.max(1, recordLines.length) * 17 + 30
    readonly property real bodyWidth: mergeRecord ? 250 : (isImage ? pictureWidth : bubbleWidth)
    readonly property real bodyHeight: mergeRecord ? recordHeight
        : (isImage ? pictureHeight : bubbleHeight)
    readonly property color bubbleColor: isSelf ? Theme.bubbleSelf : Theme.bubbleOther

    //群聊里要在气泡上方让出一行名字；单聊保持原来的紧凑高度
    readonly property real nameOffset: senderName.length > 0 ? 18 : 0
    implicitHeight: nameOffset + Math.max(34, bodyHeight)

    Text {
        visible: root.senderName.length > 0
        x: userAvatar.width + 11
        y: 0
        width: Math.max(1, Math.min(implicitWidth, root.width - 140))
        text: root.senderName
        textFormat: Text.PlainText
        color: Theme.textSecondary
        font.pixelSize: Theme.fontCaption
        elide: Text.ElideRight
    }

    MessageText { id: messageText }

    // 表情在文档里的位置；正文只留等宽占位，图片由 textBubble 里的覆盖层画
    property var emojiSpots: []
    // 文字颜色随主题/气泡方向变化，颜色变了要重新排一次文档
    readonly property color labelColor: root.isSelf ? Theme.bubbleSelfText : Theme.textPrimary
    onLabelColorChanged: refreshMessageText()
    onTextChanged: refreshMessageText()
    Component.onCompleted: refreshMessageText()

    function refreshMessageText() {
        //先摘掉旧位置：apply 会清空文档，覆盖层不能拿着上一份 pos 去算矩形
        emojiSpots = []
        if (!label)
            return
        //空正文也要清文档：气泡复用到下一条消息时不能残留上一条的内容
        emojiSpots = messageText.apply(label.textDocument, root.text, root.labelColor,
                                       Theme.fontBody, Emoji.size, Emoji.items)
    }

    Avatar {
        id: userAvatar
        visible: root.showAvatar
        x: root.isSelf ? root.width - width : 0
        y: root.nameOffset
        width: 32
        height: 32
        radius: 10
        source: root.avatar
        name: root.name
    }

    // 自己发的消息在左侧留状态位；被 @ 的消息在气泡外侧画一条竖线
    Rectangle {
        visible: root.mentioned && !root.isSelf && !root.isImage
        x: textBubble.x - 6
        y: textBubble.y + 5
        width: 2
        height: Math.max(0, textBubble.height - 10)
        radius: 1
        color: Theme.accent
    }

    // 文字气泡：只有文本消息才有底色、边框和内边距
    Rectangle {
        id: textBubble
        visible: !root.isImage && !root.mergeRecord
        x: root.isSelf ? userAvatar.x - 11 - width : userAvatar.width + 11
        y: root.nameOffset
        width: root.bubbleWidth
        height: root.bubbleHeight
        radius: 12
        color: root.bubbleColor
        border.width: root.highlighted ? 2 : (root.isSelf ? 0 : 1)
        border.color: root.highlighted ? Theme.accent : Theme.line

        TextEdit {
            id: label
            visible: !root.mergeRecord
            x: 15
            y: 11
            width: root.textWidth
            height: contentHeight
            readOnly: true
            selectByMouse: true
            selectionColor: Theme.selected
            selectedTextColor: Theme.textPrimary
            //正文由 MessageText 排版：表情是不参与行高的行内对象，
            //纯文字、混排、纯表情的行高都等于字体本身的自然行高
            textFormat: TextEdit.PlainText
            wrapMode: TextEdit.WrapAtWordBoundaryOrAnywhere
            font.pixelSize: Theme.fontBody
            color: labelColor
            //TextEdit 默认给文档留 4px 边距，会把文字和表情整体推进去，
            //气泡按 15/11 的内边距排版，这里必须清零
            textMargin: 0
        }

        // 表情覆盖层：位置取自文本布局，尺寸独立于行高。
        // 图片按行盒垂直居中，画在文本节点之上，不会被行盒或对象矩形裁剪
        Repeater {
            model: root.emojiSpots
            delegate: Image {
                //读一下宽度与高度，文本重排时让位置跟着更新
                readonly property real layoutTick: label.width + label.contentHeight
                //横向位置取自光标矩形，纵向用行盒：纯表情行的光标矩形只有 1px 高，
                //拿它的 y/height 居中会整体偏下
                readonly property rect spot: {
                    layoutTick
                    return label.positionToRectangle(modelData.pos)
                }
                readonly property var lineInfo: {
                    layoutTick
                    return messageText.lineMetrics(label.textDocument, modelData.pos)
                }
                x: label.x + spot.x
                y: label.y + lineInfo.top + (lineInfo.height - Emoji.height) / 2
                width: Emoji.size
                height: Emoji.height
                source: modelData.url
                fillMode: Image.PreserveAspectFit
                smooth: true
                z: 2
                visible: lineInfo.height > 0
            }
        }

        //只接收右键，左键选择文字仍由上面的 TextEdit 处理
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.RightButton
            onPressed: (mouse) => {
                const p = mapToItem(root, mouse.x, mouse.y)
                root.contextRequested(p.x, p.y)
            }
        }
    }

    // 聊天记录卡片：独立浅色面板，标题 + 三行预览 + 查看入口
    Rectangle {
        id: recordFrame
        visible: root.mergeRecord
        x: root.isSelf ? userAvatar.x - 11 - width : userAvatar.width + 11
        y: root.nameOffset
        width: 250
        height: root.recordHeight
        radius: 12
        //深色下用比页面稍亮的气泡底色，纯 panelBg 会像在背景上挖了个洞
        color: Theme.dark ? Theme.bubbleOther : Theme.panelBg
        border.width: 1
        border.color: root.highlighted ? Theme.accent : Theme.line

        Item {
            id: recordHead
            x: 12
            y: 10
            width: parent.width - 24
            height: 20

            AppIcon {
                id: recordIcon
                anchors.verticalCenter: parent.verticalCenter
                name: "chat"
                size: 14
                color: Theme.primary
            }
            Text {
                anchors.left: recordIcon.right
                anchors.leftMargin: 6
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: root.recordTitle.length > 0 ? root.recordTitle : "聊天记录"
                textFormat: Text.PlainText
                color: Theme.textPrimary
                font.pixelSize: Theme.fontBody
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
        }

        Column {
            id: recordPreview
            x: 12
            y: 34
            width: parent.width - 24
            spacing: 3

            Repeater {
                model: root.recordLines
                delegate: Text {
                    width: recordPreview.width
                    text: modelData
                    textFormat: Text.PlainText
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontCaption
                    elide: Text.ElideRight
                }
            }
        }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: recordFoot.top
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            anchors.bottomMargin: 8
            height: 1
            color: Theme.line
        }

        Item {
            id: recordFoot
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            height: 24

            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: "查看 " + root.recordCount + " 条转发消息"
                color: Theme.primary
                font.pixelSize: Theme.fontCaption
            }
            AppIcon {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                name: "chevronRight"
                size: 12
                color: Theme.primary
            }
        }

        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: root.recordClicked()
        }
    }

    // 图片容器：没有气泡底色和文字内边距，仅保留圆角裁切
    Rectangle {
        id: pictureFrame
        visible: root.isImage
        x: root.isSelf ? userAvatar.x - 11 - width : userAvatar.width + 11
        y: root.nameOffset
        width: root.pictureWidth
        height: root.pictureHeight
        radius: 12
        color: Theme.fieldBg
        border.width: root.highlighted ? 2 : (root.localImage ? 0 : 1)
        border.color: root.highlighted ? Theme.accent : Theme.line
    }

    Image {
        id: preview
        anchors.fill: pictureFrame
        source: root.imageSource
        visible: root.localImage //圆角由下面的蒙版裁出来，软件渲染时退化为直角
        fillMode: Image.PreserveAspectFit //框已按缩略图比例计算，Fit 不再裁切长图/宽图
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

    MouseArea {
        anchors.fill: pictureFrame
        enabled: root.localImage
        cursorShape: Qt.PointingHandCursor
        onClicked: root.imageClicked()
    }

    //图片右键：查看、复制、另存为；左键仍走上面的点击预览
    MouseArea {
        anchors.fill: pictureFrame
        acceptedButtons: Qt.RightButton
        onPressed: (mouse) => {
            const p = mapToItem(root, mouse.x, mouse.y)
            root.contextRequested(p.x, p.y)
        }
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
            font.pixelSize: Theme.fontCaption
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

    // 状态区：不再画单勾/双勾——勾号分不清"已发送"和"已读"。
    // 只在自己最后一条消息旁边给两个字结论；失败时给一个可点的重发入口。
    Row {
        visible: root.isSelf && (root.statusText.length > 0 || root.retryable)
        spacing: 6
        anchors.right: root.isImage ? pictureFrame.left : textBubble.left
        anchors.rightMargin: 8
        anchors.verticalCenter: root.isImage ? pictureFrame.verticalCenter : textBubble.verticalCenter

        Text {
            visible: root.statusText.length > 0
            text: root.statusText
            color: (root.status === 4 || root.statusText === "已读") ? Theme.textSecondary : Theme.textHint
            font.pixelSize: Theme.fontCaption
        }
        Item {
            visible: root.retryable
            width: retryLabel.implicitWidth + 12
            height: 16
            Text {
                id: retryLabel
                anchors.centerIn: parent
                text: "! 点击重发"
                color: Theme.danger
                font.pixelSize: Theme.fontCaption
            }
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: root.retryRequested()
            }
        }
    }
}
