import QtQuick

// 联系人资料：信头（头像、名字、ID）在上，字段在下；内容列上限 680，宽窗只居中。
// 备注与标签可就地改，也保留"编辑备注与标签"批量入口；删除好友只留一条文字链。
Item {
    id: root

    property var profile: ({})
    property var controller: null
    signal chatRequested()
    signal editRelationRequested()
    signal deleteRequested()
    signal backRequested()

    property string savingField: ""

    readonly property int sex: profile.sex === undefined ? 0 : profile.sex
    readonly property string display: profile.displayName || profile.name || ""
    readonly property var tags: profile.tags || []
    readonly property string remark: profile.remark || ""
    readonly property bool friend: profile.friend !== false
    readonly property string sexText: sex === 1 ? "男" : (sex === 2 ? "女" : "不公开")
    readonly property string descText: profile.desc || ""

    // 和群资料同一套信纸版式：内容列固定上限 680，左右至少各留 48
    readonly property real contentWidth: Math.min(width - 96, 680)
    readonly property real inset: Math.max(48, (width - contentWidth) / 2)

    function closeOthers(except) {
        if (except !== "remark")
            remarkField.cancel()
        if (except !== "tags")
            tagsField.cancel()
    }

    function saveRelation(remarkValue, tagsValue) {
        if (!controller || !profile.uid || !friend)
            return
        controller.saveFriendRemark(profile.uid, remarkValue, tagsValue)
    }

    Connections {
        target: root.controller
        function onRemarkSaved(ok, uid) {
            if (!root.profile || uid !== root.profile.uid || root.savingField.length === 0)
                return
            const field = root.savingField === "tags" ? tagsField : remarkField
            if (ok)
                field.finish()
            else
                field.fail(root.controller.remarkMessage)
            root.savingField = ""
        }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.sheet
    }

    PageHeader {
        id: header
        title: "联系人资料"
        inset: 24
        backVisible: true
        onBackRequested: root.backRequested()
    }

    Flickable {
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        width: parent.width
        contentWidth: width
        contentHeight: body.height + 64
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Item {
            id: body
            x: root.inset
            y: 36
            width: root.contentWidth
            height: identityBlock.height + 28 + fields.height

            // 信头：头像与名字横排，字段始终在下方
            Item {
                id: identityBlock
                width: parent.width
                height: 92

                Row {
                    width: parent.width
                    spacing: 16

                    Avatar {
                        width: 72
                        height: 72
                        radius: 18
                        source: root.profile.icon || ""
                        name: root.display
                        textSize: 22
                    }

                    Column {
                        width: parent.width - 88
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 6

                        Text {
                            width: parent.width
                            text: root.display
                            textFormat: Text.PlainText
                            color: Theme.textPrimary
                            font.pixelSize: 20
                            font.weight: Font.DemiBold
                            elide: Text.ElideRight
                        }
                        Text {
                            text: root.profile.uid ? "ID  " + root.profile.uid : ""
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontCaption
                            font.letterSpacing: 1
                        }
                    }
                }
            }

            Column {
                id: fields
                x: 0
                y: identityBlock.height + 28
                width: root.contentWidth
                spacing: 0

                Item {
                    width: parent.width
                    height: 32

                    Text {
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        text: "资料"
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSmall
                        font.weight: Font.Medium
                    }

                    Row {
                        anchors.right: parent.right
                        anchors.rightMargin: 6
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 12

                        AppButton {
                            width: 92
                            height: 32
                            text: "发消息"
                            onClicked: root.chatRequested()
                        }
                        Text {
                            height: 32
                            verticalAlignment: Text.AlignVCenter
                            text: "编辑备注"
                            color: Theme.primary
                            font.pixelSize: Theme.fontSmall
                            visible: root.friend
                            MouseArea {
                                anchors.fill: parent
                                anchors.margins: -6
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.editRelationRequested()
                            }
                        }
                    }
                }

                Rectangle {
                    width: parent.width
                    height: 1
                    color: Theme.lineSoft
                }

                EditableField {
                    label: "昵称"
                    value: root.profile.nick || ""
                    placeholder: "未设置"
                    editable: false
                }

                EditableField {
                    label: "性别"
                    value: root.sexText
                    editable: false
                }

                EditableField {
                    id: remarkField
                    label: "备注"
                    value: root.remark
                    placeholder: "未设置"
                    maxLength: 20
                    editable: root.friend
                    busy: root.savingField === "remark" && root.controller && root.controller.remarkSaving
                    onEditStarted: {
                        root.closeOthers("remark")
                        if (root.controller)
                            root.controller.clearProfileMessages()
                    }
                    onCommitted: (value) => {
                        root.savingField = "remark"
                        root.saveRelation(value, root.tags)
                    }
                }

                EditableField {
                    id: tagsField
                    label: "标签"
                    mode: "tags"
                    tags: root.tags
                    placeholder: "未设置"
                    editable: root.friend
                    busy: root.savingField === "tags" && root.controller && root.controller.remarkSaving
                    onEditStarted: {
                        root.closeOthers("tags")
                        if (root.controller)
                            root.controller.clearProfileMessages()
                    }
                    onTagsCommitted: (list) => {
                        root.savingField = "tags"
                        root.saveRelation(root.remark, list)
                    }
                }

                EditableField {
                    label: "个性签名"
                    value: root.descText
                    placeholder: "还没有签名"
                    mode: "multiline"
                    maxLines: 4
                    editable: false
                }

                EditableField {
                    label: "用户名"
                    value: root.profile.name || ""
                    placeholder: "未设置"
                    editable: false
                }

                Rectangle {
                    width: parent.width
                    height: 1
                    color: Theme.lineSoft
                    visible: root.friend
                }

                Item { width: 1; height: 16; visible: root.friend }

                Column {
                    width: parent.width
                    visible: root.friend
                    spacing: 4

                    Text {
                        text: "删除好友"
                        color: Theme.danger
                        font.pixelSize: Theme.fontSmall
                        font.weight: Font.Medium
                        MouseArea {
                            anchors.fill: parent
                            anchors.margins: -6
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.deleteRequested()
                        }
                    }
                    Text {
                        text: "对方不会收到通知，聊天记录仍会保留在这台设备。"
                        color: Theme.textHint
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.WordWrap
                        width: parent.width
                    }
                }
            }
        }
    }
}
