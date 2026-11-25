import QtQuick
import QtQuick.Effects

// 视频与文件消息共用一张资源卡片：
// 视频用首帧做预览，未下载时点击卡片开始下载；文件按 QQ 的文档卡片展示，
// 右侧按钮负责下载/暂停/继续/打开，下载完成后额外提供“另存为”。
Item {
    id: root

    property string avatar: ""
    property string name: ""
    property bool showAvatar: true
    property string senderName: "" //群聊里别人发的消息：卡片上方显示成员名""
    property bool isSelf: false
    property int msgType: 3 //2 视频 / 3 文件
    property int status: 1
    property string fileName: ""
    property double fileSize: 0
    property string thumbSource: ""
    property int transferState: 0
    property int transferProgress: -1

    //与 ResourceTransferState 保持一致，QML 侧不直接依赖 C++ 枚举
    readonly property int stateNone: 0
    readonly property int statePreparing: 1
    readonly property int stateUploading: 2
    readonly property int stateUploadPaused: 3
    readonly property int stateDownloading: 4
    readonly property int stateDownloadPaused: 5
    readonly property int stateDone: 6
    readonly property int stateFailed: 7

    readonly property bool video: msgType === 2
    readonly property bool preparing: transferState === statePreparing
    readonly property bool uploading: transferState === stateUploading
    readonly property bool uploadPaused: transferState === stateUploadPaused
    readonly property bool downloading: transferState === stateDownloading
    readonly property bool downloadPaused: transferState === stateDownloadPaused
    readonly property bool done: transferState === stateDone
    readonly property bool failed: transferState === stateFailed
    readonly property bool active: preparing || uploading || downloading
        || uploadPaused || downloadPaused
    readonly property int percent: Math.max(0, Math.min(100, transferProgress))
    readonly property real cardWidth: video ? Math.min(250, width - 102)
                                             : Math.min(304, width - 102)
    readonly property real availableWidth: Math.max(1, Math.min(304, width - 102))

    readonly property string actionName: {
        if (preparing) return "noop" //首帧/落库准备中，先不接受操作
        if (done) return "open"
        if (uploading || downloading) return "toggle"
        if (uploadPaused || downloadPaused) return "toggle"
        if (failed) return isSelf ? "retryUpload" : "download"
        if (isSelf) return "download" //本机文件被清理后可从服务器重新拉取
        return "download"
    }

    readonly property string stateText: {
        if (preparing) return "准备中"
        if (uploading) return "上传中 " + percent + "%"
        if (downloadPaused) return "已暂停 " + percent + "%"
        if (downloading) return "下载中 " + percent + "%"
        if (uploadPaused) return "已暂停 " + percent + "%"
        if (done) return isSelf ? "已发送" : "已下载"
        if (failed) return isSelf ? "发送失败" : "下载失败"
        return isSelf ? "待发送" : "未下载"
    }

    //进度条只在真正传输或准备时出现，完成后整条隐藏
    readonly property bool showProgress: preparing || uploading || uploadPaused
        || downloading || downloadPaused

    //群聊里要在卡片上方让出一行名字；单聊保持原来的紧凑高度
    readonly property real nameOffset: senderName.length > 0 ? 18 : 0
    implicitHeight: nameOffset + Math.max(34, card.height)

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

    function formatSize(bytes) {
        if (!bytes || bytes <= 0)
            return "0 B"
        if (bytes < 1024)
            return bytes + " B"
        if (bytes < 1024 * 1024)
            return (bytes / 1024).toFixed(1) + " KB"
        if (bytes < 1024 * 1024 * 1024)
            return (bytes / (1024 * 1024)).toFixed(1) + " MB"
        return (bytes / (1024 * 1024 * 1024)).toFixed(2) + " GB"
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

    Rectangle {
        id: card
        x: root.isSelf ? userAvatar.x - 11 - width : userAvatar.width + 11
        y: root.nameOffset
        width: root.cardWidth
        height: root.video ? 190 : 82
        radius: 14
        color: Theme.panelBg
        border.width: 1
        border.color: Theme.line
        clip: true

        //视频：首帧预览 + 中央操作按钮
        Item {
            id: preview
            visible: root.video
            width: parent.width
            height: 135

            Rectangle {
                anchors.fill: parent
                radius: 10
                color: Theme.panelAlt
                gradient: Gradient {
                    GradientStop { position: 0; color: Theme.panelAlt }
                    GradientStop { position: 1; color: Theme.selected }
                }
            }

            Image {
                id: previewImage
                anchors.fill: parent
                source: root.thumbSource
                visible: root.thumbSource.length > 0
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                cache: true
                //卡片圆角不会自动裁切子项，用同尺寸圆角蒙版把首帧一起裁掉，
                //否则方形视频的截图会露出四个直角
                layer.enabled: true
                layer.effect: MultiEffect {
                    maskEnabled: true
                    maskSource: previewMask
                }
            }

            Rectangle {
                id: previewMask
                anchors.fill: parent
                radius: 10
                color: "white"
                visible: false
                layer.enabled: true
            }

            //没有首帧时用视频图标和文件名占位
            Column {
                visible: root.thumbSource.length === 0
                anchors.centerIn: parent
                spacing: 7
                AppIcon {
                    anchors.horizontalCenter: parent.horizontalCenter
                    name: "video"
                    size: 30
                    color: Theme.textHint
                }
                Text {
                    width: preview.width - 40
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: root.fileName
                    color: Theme.textHint
                    font.pixelSize: Theme.fontCaption
                    horizontalAlignment: Text.AlignHCenter
                    elide: Text.ElideMiddle
                }
            }

            //中央操作：播放、下载、暂停/继续
            Rectangle {
                id: playCircle
                anchors.centerIn: parent
                width: 42
                height: 42
                radius: 21
                color: Qt.rgba(1, 1, 1, 0.84)
                border.width: 1
                border.color: Qt.rgba(0, 0, 0, 0.08)

                AppIcon {
                    anchors.centerIn: parent
                    size: 20
                    color: Theme.ink
                    name: root.done ? "play"
                        : (root.preparing ? "video"
                        : (root.uploading || root.downloading ? "pause"
                        : (root.uploadPaused || root.downloadPaused ? "play" : "download")))
                }
            }

            Text {
                visible: !root.isSelf && (root.transferState === root.stateNone || root.failed)
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 12
                text: root.failed ? "下载失败，点击重试" : "点击下载视频"
                color: Theme.onMedia
                font.pixelSize: Theme.fontCaption
                style: Text.Outline
                styleColor: Qt.rgba(0, 0, 0, 0.35)
            }

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: root.actionRequested(root.actionName)
            }
        }

        //视频文件名与状态，放在预览下方
        Column {
            visible: root.video
            x: 10
            y: 142
            width: parent.width - 20
            spacing: 5

            Text {
                width: parent.width
                text: root.fileName
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSmall
                elide: Text.ElideMiddle
                maximumLineCount: 1
            }
            Text {
                width: parent.width
                text: root.formatSize(root.fileSize) + " · " + root.stateText
                color: root.failed ? Theme.danger : Theme.textHint
                font.pixelSize: Theme.fontCaption
                elide: Text.ElideRight
            }
        }

        //文件：扩展名图标 + 文件名/大小 + 右侧操作
        Item {
            id: fileBody
            visible: !root.video
            anchors.fill: parent

            Rectangle {
                id: fileTile
                x: 10
                anchors.verticalCenter: parent.verticalCenter
                width: 42
                height: 52
                radius: Theme.controlRadius
                color: Theme.primarySoft

                Text {
                    anchors.centerIn: parent
                    width: parent.width - 6
                    text: {
                        const parts = root.fileName.split(".")
                        if (parts.length < 2)
                            return "FILE"
                        return parts[parts.length - 1].toUpperCase().slice(0, 4)
                    }
                    color: Theme.primary
                    font.pixelSize: 10
                    font.bold: true
                    horizontalAlignment: Text.AlignHCenter
                }
            }

            Column {
                anchors.left: fileTile.right
                anchors.leftMargin: 10
                anchors.right: fileActions.left
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                spacing: 5

                Text {
                    width: parent.width
                    text: root.fileName
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontSmall
                    elide: Text.ElideMiddle
                    maximumLineCount: 1
                }
                Text {
                    width: parent.width
                    text: root.formatSize(root.fileSize) + " · " + root.stateText
                    color: root.failed ? Theme.danger : Theme.textHint
                    font.pixelSize: Theme.fontCaption
                    elide: Text.ElideRight
                }
            }

            Column {
                id: fileActions
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                spacing: 4

                //打开：下载完成后才出现
                Rectangle {
                    visible: root.done
                    width: 54
                    height: 24
                    radius: Theme.controlRadius
                    color: actionArea.containsMouse ? Theme.primaryHover : Theme.primary

                    Text {
                        anchors.centerIn: parent
                        text: "打开"
                        color: Theme.primaryText
                        font.pixelSize: Theme.fontCaption
                    }
                    MouseArea {
                        id: actionArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.actionRequested("open")
                    }
                }

                //打开文件夹：下载完成后直接定位到本地文件
                Rectangle {
                    visible: root.done
                    width: 54
                    height: 22
                    radius: Theme.controlRadius
                    color: revealArea.containsMouse ? Theme.panelAlt : "transparent"
                    border.width: 1
                    border.color: Theme.line

                    Text {
                        anchors.centerIn: parent
                        text: "文件夹"
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontCaption
                    }
                    MouseArea {
                        id: revealArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.actionRequested("reveal")
                    }
                }

                //另存为：未下载时也可以先选目标，下载完成后自动复制
                Rectangle {
                    visible: !root.done
                    width: 54
                    height: 22
                    radius: Theme.controlRadius
                    color: saveArea.containsMouse ? Theme.panelAlt : "transparent"
                    border.width: 1
                    border.color: Theme.line

                    Text {
                        anchors.centerIn: parent
                        text: "另存为"
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontCaption
                    }
                    MouseArea {
                        id: saveArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.actionRequested("saveAs")
                    }
                }

                //下载/暂停/继续/重试：没有完成时只显示这一颗按钮
                Rectangle {
                    visible: !root.done
                    width: 54
                    height: 28
                    radius: Theme.controlRadius
                    color: primaryArea.containsMouse ? Theme.primaryHover : Theme.primary
                    opacity: root.preparing ? 0.6 : 1

                    Text {
                        anchors.centerIn: parent
                        text: {
                            if (root.uploading || root.downloading) return "暂停"
                            if (root.uploadPaused || root.downloadPaused) return "继续"
                            if (root.failed) return "重试"
                            return "下载"
                        }
                        color: Theme.primaryText
                        font.pixelSize: Theme.fontCaption
                    }
                    MouseArea {
                        id: primaryArea
                        anchors.fill: parent
                        hoverEnabled: true
                        enabled: !root.preparing
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.actionRequested(root.actionName)
                    }
                }
            }
        }

        //底部细进度条：上传和下载共用，不显示百分比文字
        Rectangle {
            visible: root.showProgress
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            anchors.bottomMargin: 6
            height: 3
            radius: 1.5
            color: Theme.panelAlt

            Rectangle {
                width: parent.width * Math.max(0, Math.min(100, root.transferProgress)) / 100
                height: parent.height
                radius: parent.radius
                color: Theme.primary
                Behavior on width { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
            }
        }
    }

    //自己发送的消息在卡片左侧显示状态：转圈/暂停/对勾/失败
    Row {
        visible: root.isSelf
        spacing: 3
        anchors.right: card.left
        anchors.rightMargin: 7
        anchors.verticalCenter: card.verticalCenter

        AppIcon {
            visible: root.preparing || root.uploading
            name: "reset"
            size: 14
            color: Theme.textHint
            RotationAnimation on rotation {
                running: root.preparing || root.uploading
                loops: Animation.Infinite
                duration: 900
            }
        }
        AppIcon {
            visible: root.uploadPaused
            name: "pause"
            size: 14
            color: Theme.textHint
        }
        AppIcon {
            visible: root.done
            name: "check"
            size: 14
            color: Theme.primary
        }
        AppIcon {
            visible: root.failed
            name: "close"
            size: 14
            color: Theme.danger
        }
    }

    //卡片整体右键：只接管右键，左键仍落到播放/下载/文件按钮
    MouseArea {
        anchors.fill: card
        acceptedButtons: Qt.RightButton
        onPressed: (mouse) => {
            const p = mapToItem(root, mouse.x, mouse.y)
            root.contextRequested(p.x, p.y)
        }
    }

    signal actionRequested(string action)
    signal contextRequested(real x, real y)
}
