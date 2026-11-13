import QtQuick
import QtMultimedia

// 内置查看器：图片支持滚轮缩放、拖拽与双击放大；视频使用独立播放器全屏预览。
// 由 ChatWindow 覆盖在整个聊天区上，打开时拦截键盘与鼠标。
Item {
    id: root

    property string kind: "" //image / video
    property string source: ""
    property string title: ""
    property string errorText: ""
    property real zoom: 1
    property real panX: 0
    property real panY: 0
    property var items: [] //当前会话的图片与已下载视频，按消息顺序
    property int index: -1
    property string notice: ""

    visible: false
    enabled: visible
    focus: visible
    z: 200

    function openImage(url, name) {
        player.stop()
        player.source = ""
        kind = "image"
        source = url
        title = name
        errorText = ""
        zoom = 1
        panX = 0
        panY = 0
        visible = true
        forceActiveFocus()
    }

    function openVideo(url, name) {
        kind = "video"
        source = url
        title = name
        errorText = ""
        visible = true
        forceActiveFocus()
        Qt.callLater(function() {
            if (root.kind !== "video" || root.source !== url)
                return //连续翻页时旧回调不能覆盖当前视频
            player.stop()
            player.source = url
            player.play()
        })
    }

    //打开一组媒体：start 是当前消息在列表里的下标
    function openPlaylist(list, start) {
        items = list || []
        if (items.length === 0)
            return
        index = Math.max(0, Math.min(start || 0, items.length - 1))
        showCurrent()
    }

    function showCurrent() {
        const item = items[index]
        if (!item)
            return
        //标题只显示当前媒体的名字；不拼“第几张/共几张”，避免大列表里反复算总数
        const label = item.title || ""
        if (item.type === 2)
            openVideo(item.url, label)
        else
            openImage(item.url, label)
    }

    //首尾不循环，明确提示已经到头，避免误以为没有上一张/下一张
    function step(delta) {
        if (items.length === 0)
            return
        const next = index + delta
        if (next < 0) {
            showNotice("已经是第一张")
            return
        }
        if (next >= items.length) {
            showNotice("已经是最后一张")
            return
        }
        index = next
        showCurrent()
    }

    function showNotice(text) {
        notice = text
        noticeTimer.restart()
    }

    function close() {
        player.stop()
        player.source = ""
        items = []
        index = -1
        notice = ""
        visible = false
    }

    Keys.onEscapePressed: root.close()
    Keys.onLeftPressed: if (visible && items.length > 1) root.step(-1)
    Keys.onRightPressed: if (visible && items.length > 1) root.step(1)

    Timer {
        id: noticeTimer
        interval: 1500
        onTriggered: root.notice = ""
    }

    Rectangle {
        anchors.fill: parent
        color: "#F20C120E"
    }

    //点击空白处关闭；图片/视频控件在上层，不会被这个区域抢走
    MouseArea {
        anchors.fill: parent
        onClicked: root.close()
    }

    //图片：视口内缩放与拖拽
    Item {
        id: imageViewport
        visible: root.kind === "image"
        anchors.fill: parent
        anchors.topMargin: 20
        anchors.bottomMargin: 64
        anchors.leftMargin: 40
        anchors.rightMargin: 40
        clip: true

        Image {
            id: picture
            source: root.kind === "image" ? root.source : ""
            fillMode: Image.PreserveAspectFit
            asynchronous: true
            smooth: true
            width: Math.min(imageViewport.width * 0.94, implicitWidth)
            height: Math.min(imageViewport.height * 0.94, implicitHeight)
            x: (imageViewport.width - width) / 2 + root.panX
            y: (imageViewport.height - height) / 2 + root.panY
            scale: root.zoom
            transformOrigin: Item.Center
        }

        MouseArea {
            anchors.fill: parent
            property point lastPoint
            cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
            onPressed: (mouse) => { lastPoint = Qt.point(mouse.x, mouse.y) }
            onPositionChanged: {
                if (!pressed)
                    return
                root.panX += mouse.x - lastPoint.x
                root.panY += mouse.y - lastPoint.y
                lastPoint = Qt.point(mouse.x, mouse.y)
            }
            onDoubleClicked: root.zoom = root.zoom > 1.01 ? 1 : 2.2
        }

        WheelHandler {
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            onWheel: (event) => {
                const step = event.angleDelta.y > 0 ? 0.12 : -0.12
                root.zoom = Math.max(0.5, Math.min(5, root.zoom + step))
            }
        }
    }

    //视频画面
    VideoOutput {
        id: videoOutput
        visible: root.kind === "video"
        anchors.fill: parent
        anchors.topMargin: 0
        anchors.bottomMargin: 78
        fillMode: VideoOutput.PreserveAspectFit
    }

    AudioOutput {
        id: audioOutput
        volume: 0.85
    }

    MediaPlayer {
        id: player
        audioOutput: audioOutput
        videoOutput: videoOutput
        onErrorOccurred: root.errorText = "无法播放该视频"
    }

    Text {
        visible: root.kind === "video" && root.errorText.length > 0
        anchors.centerIn: videoOutput
        text: root.errorText
        color: "#E7EDDF"
        font.pixelSize: 13
    }

    //图片底部缩放工具
    Row {
        visible: root.kind === "image"
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 18
        spacing: 8

        Rectangle {
            width: 34
            height: 30
            radius: 7
            color: zoomOut.containsMouse ? "#33FFFFFF" : "#22FFFFFF"
            Text { anchors.centerIn: parent; text: "−"; color: "#E7EDDF"; font.pixelSize: 18 }
            MouseArea {
                id: zoomOut
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.zoom = Math.max(0.5, root.zoom - 0.2)
            }
        }
        Rectangle {
            width: 54
            height: 30
            radius: 7
            color: zoomReset.containsMouse ? "#33FFFFFF" : "#22FFFFFF"
            Text {
                anchors.centerIn: parent
                text: Math.round(root.zoom * 100) + "%"
                color: "#E7EDDF"
                font.pixelSize: 11
            }
            MouseArea {
                id: zoomReset
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    root.zoom = 1
                    root.panX = 0
                    root.panY = 0
                }
            }
        }
        Rectangle {
            width: 34
            height: 30
            radius: 7
            color: zoomIn.containsMouse ? "#33FFFFFF" : "#22FFFFFF"
            Text { anchors.centerIn: parent; text: "+"; color: "#E7EDDF"; font.pixelSize: 17 }
            MouseArea {
                id: zoomIn
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.zoom = Math.min(5, root.zoom + 0.2)
            }
        }
    }

    //视频播放控制
    Item {
        id: videoControls
        visible: root.kind === "video"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 66
        anchors.leftMargin: 28
        anchors.rightMargin: 28

        Rectangle {
            id: playButton
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            width: 34
            height: 34
            radius: 17
            color: playArea.containsMouse ? "#33FFFFFF" : "#22FFFFFF"

            AppIcon {
                anchors.centerIn: parent
                name: player.playbackState === MediaPlayer.PlayingState ? "pause" : "play"
                size: 16
                color: "#E7EDDF"
            }
            MouseArea {
                id: playArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: player.playbackState === MediaPlayer.PlayingState
                    ? player.pause() : player.play()
            }
        }

        Text {
            id: positionText
            anchors.left: playButton.right
            anchors.leftMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            text: formatTime(player.position)
            color: "#A8B7AA"
            font.pixelSize: 10
        }

        //进度条：点击任意位置跳转
        Rectangle {
            id: progressTrack
            anchors.left: positionText.right
            anchors.leftMargin: 10
            anchors.right: durationText.left
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            height: 4
            radius: 2
            color: "#44FFFFFF"

            Rectangle {
                width: player.duration > 0
                    ? parent.width * Math.min(1, player.position / player.duration) : 0
                height: parent.height
                radius: parent.radius
                color: "#BCD2AD"
            }

            MouseArea {
                anchors.fill: parent
                anchors.topMargin: -8
                anchors.bottomMargin: -8
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    if (player.duration > 0)
                        player.position = player.duration * Math.max(0, Math.min(1, mouse.x / width))
                }
            }
        }

        Text {
            id: durationText
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: formatTime(player.duration)
            color: "#A8B7AA"
            font.pixelSize: 10
        }
    }

    //左右切换：同一会话的图片与已下载视频串成一个列表，首尾给出提示
    Rectangle {
        id: prevButton
        visible: root.items.length > 1
        anchors.left: parent.left
        anchors.leftMargin: 18
        anchors.verticalCenter: parent.verticalCenter
        width: 44
        height: 44
        radius: 22
        color: prevArea.containsMouse ? "#44FFFFFF" : "#22FFFFFF"
        opacity: root.index <= 0 ? 0.45 : 1

        AppIcon {
            anchors.centerIn: parent
            name: "back"
            size: 20
            color: "#E7EDDF"
        }
        MouseArea {
            id: prevArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: root.step(-1)
        }
    }

    Rectangle {
        id: nextButton
        visible: root.items.length > 1
        anchors.right: parent.right
        anchors.rightMargin: 18
        anchors.verticalCenter: parent.verticalCenter
        width: 44
        height: 44
        radius: 22
        color: nextArea.containsMouse ? "#44FFFFFF" : "#22FFFFFF"
        opacity: root.index >= root.items.length - 1 ? 0.45 : 1

        AppIcon {
            anchors.centerIn: parent
            name: "chevronRight"
            size: 20
            color: "#E7EDDF"
        }
        MouseArea {
            id: nextArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: root.step(1)
        }
    }

    //关闭按钮浮动在右上角，不再单独占一行
    Rectangle {
        id: closeButton
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.rightMargin: 18
        anchors.topMargin: 18
        width: 38
        height: 38
        radius: 19
        color: closeArea.containsMouse ? "#44FFFFFF" : "#22FFFFFF"

        AppIcon {
            anchors.centerIn: parent
            name: "close"
            size: 17
            color: "#E7EDDF"
        }
        MouseArea {
            id: closeArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: root.close()
        }
    }

    Rectangle {
        visible: root.notice.length > 0
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 16
        width: noticeLabel.width + 26
        height: 30
        radius: 8
        color: "#CC000000"

        Text {
            id: noticeLabel
            anchors.centerIn: parent
            text: root.notice
            color: "#E7EDDF"
            font.pixelSize: 12
        }
    }

    function formatTime(ms) {
        if (!ms || ms <= 0)
            return "00:00"
        const total = Math.floor(ms / 1000)
        const minutes = Math.floor(total / 60)
        const seconds = total % 60
        return (minutes < 10 ? "0" : "") + minutes + ":" + (seconds < 10 ? "0" : "") + seconds
    }
}
