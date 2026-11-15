import QtQuick

// 账号字段由桥接层提供，头像保存后与侧栏及自己的消息同步刷新。
// 布局与好友资料页一致：标题栏 + 限宽 480 的详情列。
Item {
    id: root
    property var controller
    property bool avatarSaved: false
    readonly property var profile: controller ? controller.myProfile : ({})
    signal editAvatarRequested()
    // field 为空表示打开批量编辑；"nick"/"sex"/"desc" 表示只针对这一项
    signal editProfileRequested(string field)

    Rectangle {
        anchors.fill: parent
        color: Theme.chatBg
    }

    // 标题栏
    Item {
        id: header
        width: parent.width
        height: 74
        Text {
            x: 30
            anchors.verticalCenter: parent.verticalCenter
            text: "个人资料"
            color: Theme.textPrimary
            font.pixelSize: 16
            font.weight: Font.DemiBold
        }
        // 标题下用一条细线把页头和内容分开
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: Theme.line
        }
    }

    Flickable {
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        width: parent.width
        contentWidth: width
        contentHeight: details.height + 84
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: details
            x: Math.max(30, (parent.width - 480) / 2)
            y: 42
            width: Math.min(parent.width - 60, 480)

            Row {
                width: parent.width
                spacing: 22
                // 头像本身就是一个入口：悬停出现"更换头像"，点一下直接进头像编辑
                Item {
                    width: 88
                    height: 88
                    Avatar {
                        anchors.fill: parent
                        source: root.controller ? root.controller.myIcon : ""
                        name: root.controller ? root.controller.myName : ""
                        textSize: 30
                    }
                    Rectangle {
                        anchors.fill: parent
                        radius: 12
                        color: Qt.rgba(0, 0, 0, 0.42)
                        visible: avatarArea.containsMouse
                        Text {
                            anchors.centerIn: parent
                            text: "更换头像"
                            color: "#FFFFFF"
                            font.pixelSize: 11
                        }
                    }
                    MouseArea {
                        id: avatarArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            root.avatarSaved = false
                            root.editAvatarRequested()
                        }
                    }
                }
                Column {
                    width: parent.width - 110
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 10
                    Text {
                        width: parent.width
                        text: root.controller ? root.controller.myName : ""
                        textFormat: Text.PlainText
                        color: Theme.textPrimary
                        font.pixelSize: 25
                        font.weight: Font.DemiBold
                        wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                    }
                    Text {
                        text: root.profile.uid ? "ID  " + root.profile.uid : ""
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSmall
                        font.letterSpacing: 1
                    }
                }
            }

            Spacer { gap: 24 }
            // 单项修改在字段行上直接点；这里只保留批量编辑入口
            Row {
                width: parent.width
                spacing: 12
                AppButton {
                    objectName: "editProfileButton"
                    width: 146
                    height: 42
                    text: "编辑资料"
                    onClicked: {
                        forceActiveFocus()
                        root.editProfileRequested("")
                    }
                }
            }
            Text {
                width: parent.width
                topPadding: 12
                text: "头像已保存"
                visible: root.avatarSaved
                color: Theme.success
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
                Accessible.role: Accessible.StaticText
                Accessible.name: text
            }

            // 同步状态一行文字：上传中显示进度提示，完成后显示“头像已同步”，失败显示错误码
            Text {
                width: parent.width
                topPadding: 6
                text: root.controller ? root.controller.avatarMessage : ""
                visible: text.length > 0
                color: root.controller && root.controller.avatarUploading
                       ? Theme.textSecondary : Theme.success
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }
            // 进度条只在真正上传时出现；宽度按 0..100 的比例拉伸
            Rectangle {
                width: parent.width
                height: 4
                radius: 2
                color: Theme.line
                visible: root.controller ? root.controller.avatarUploading : false
                Rectangle {
                    width: parent.width * (root.controller ? root.controller.avatarProgress / 100 : 0)
                    height: parent.height
                    radius: parent.radius
                    color: Theme.accent
                    Behavior on width { NumberAnimation { duration: 120 } }
                }
            }
            // 暂停只停止下一片，不取消已经在途的分片；继续时客户端会先向服务器同步进度
            AppButton {
                width: 112
                height: 36
                visible: root.controller ? root.controller.avatarUploading : false
                text: root.controller && root.controller.avatarUploadPaused ? "继续同步" : "暂停同步"
                onClicked: if (root.controller) root.controller.toggleAvatarUpload()
            }

            Spacer { gap: 30 }
            Rectangle {
                width: parent.width
                height: 1
                color: Theme.line
                Rectangle {
                    width: 28
                    height: 2
                    color: Theme.accent
                }
            }
            Spacer { gap: 12 }

            Repeater {
                model: [
                    { label: "用户名", value: root.profile.name || "未设置", field: "" },
                    { label: "昵称", value: root.profile.nick || "未设置", field: "nick" },
                    { label: "性别", value: root.profile.sex === 1 ? "男" : (root.profile.sex === 2 ? "女" : "未设置"), field: "sex" },
                    { label: "个性签名", value: root.profile.desc || "未设置", field: "desc" }
                ]
                delegate: Item {
                    id: fieldRow
                    width: details.width
                    height: Math.max(48, valueText.implicitHeight + 26)
                    readonly property bool editable: modelData.field.length > 0
                    readonly property bool hovered: rowArea.containsMouse && editable

                    Text {
                        y: 13
                        text: modelData.label
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontBody
                    }
                    Text {
                        id: valueText
                        x: 82
                        y: 13
                        width: parent.width - x - 44
                        text: modelData.value
                        textFormat: Text.PlainText
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontBody
                        wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                    }
                    // 可改的字段悬停时右侧出现「修改」，点一下只改这一项
                    Text {
                        anchors.right: parent.right
                        anchors.rightMargin: 2
                        y: 13
                        text: "修改"
                        visible: fieldRow.hovered
                        color: Theme.primary
                        font.pixelSize: Theme.fontSmall
                    }
                    MouseArea {
                        id: rowArea
                        anchors.fill: parent
                        hoverEnabled: true
                        enabled: fieldRow.editable
                        cursorShape: fieldRow.editable ? Qt.PointingHandCursor : Qt.ArrowCursor
                        onClicked: root.editProfileRequested(modelData.field)
                    }
                }
            }
        }
    }
}
