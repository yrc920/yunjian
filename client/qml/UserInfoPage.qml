import QtQuick

// 个人资料：信头（头像、名字、ID）在上，字段在下；内容列上限 680，宽窗只居中。
// 每个字段可就地改，"编辑资料"保留批量入口，批量弹层里也能换头像。
Item {
    id: root
    property var controller
    property bool avatarSaved: false
    readonly property var profile: controller ? controller.myProfile : ({})
    signal editAvatarRequested()
    signal editProfileRequested(string field)

    property string savingField: ""

    // 和群资料同一套信纸版式：内容列固定上限 680，左右至少各留 48
    readonly property real contentWidth: Math.min(width - 96, 680)
    readonly property real inset: Math.max(48, (width - contentWidth) / 2)

    readonly property string nickText: profile.nick || ""
    readonly property int sexValue: profile.sex === undefined ? 0 : profile.sex
    readonly property string sexText: sexValue === 1 ? "男" : (sexValue === 2 ? "女" : "不公开")
    readonly property string descText: profile.desc || ""
    readonly property string avatarStatus: {
        if (!controller)
            return ""
        if (controller.avatarUploading)
            return controller.avatarUploadPaused
                ? "同步已暂停" : "正在同步 " + controller.avatarProgress + "%"
        if (root.avatarSaved)
            return "头像已更新"
        return controller.avatarMessage
    }

    function closeOthers(except) {
        if (except !== "nick")
            nickField.cancel()
        if (except !== "sex")
            sexField.cancel()
        if (except !== "desc")
            descField.cancel()
    }

    function beginField(key) {
        closeOthers(key)
        if (controller)
            controller.clearProfileMessages()
    }

    function saveField(key, value) {
        if (!controller)
            return
        savingField = key
        if (key === "nick")
            controller.saveMyProfile(String(value), sexValue, descText)
        else if (key === "sex")
            controller.saveMyProfile(nickText, Number(value), descText)
        else
            controller.saveMyProfile(nickText, sexValue, String(value))
    }

    Connections {
        target: root.controller
        function onProfileSaved(ok) {
            if (root.savingField.length === 0)
                return
            const field = root.savingField === "nick" ? nickField
                        : root.savingField === "sex" ? sexField : descField
            if (ok)
                field.finish()
            else
                field.fail(root.controller.profileMessage)
            root.savingField = ""
        }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.sheet
    }

    PageHeader {
        id: header
        title: "个人资料"
        inset: 24
    }

    Flickable {
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        width: parent.width
        contentWidth: width
        contentHeight: body.height + 72
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

                    AvatarAction {
                        width: 72
                        height: 72
                        source: root.controller ? root.controller.myIcon : ""
                        name: root.controller ? root.controller.myName : ""
                        uploading: root.controller ? root.controller.avatarUploading : false
                        progress: root.controller ? root.controller.avatarProgress : 0
                        onEditRequested: {
                            root.avatarSaved = false
                            root.editAvatarRequested()
                        }
                    }

                    Column {
                        width: parent.width - 88
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 6

                        Text {
                            width: parent.width
                            text: root.controller ? root.controller.myName : ""
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
                        Text {
                            width: parent.width
                            visible: root.avatarStatus.length > 0
                            text: root.avatarStatus
                            color: root.controller && root.controller.avatarUploading
                                   ? Theme.textSecondary : Theme.success
                            font.pixelSize: Theme.fontCaption
                            elide: Text.ElideRight
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

                    AppButton {
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        width: 108
                        height: 32
                        text: "编辑资料"
                        variant: "outline"
                        onClicked: {
                            if (!root.controller)
                                return
                            root.closeOthers("")
                            root.editProfileRequested("")
                        }
                    }
                }

                Rectangle {
                    width: parent.width
                    height: 1
                    color: Theme.lineSoft
                }

                EditableField {
                    id: nickField
                    label: "昵称"
                    value: root.nickText
                    placeholder: "未设置"
                    maxLength: 20
                    busy: root.savingField === "nick" && root.controller && root.controller.profileSaving
                    onEditStarted: root.beginField("nick")
                    onCommitted: (value) => root.saveField("nick", value)
                }

                EditableField {
                    id: sexField
                    label: "性别"
                    value: root.sexText
                    mode: "choice"
                    options: [{ label: "不公开", value: 0 }, { label: "男", value: 1 }, { label: "女", value: 2 }]
                    choiceValue: root.sexValue
                    busy: root.savingField === "sex" && root.controller && root.controller.profileSaving
                    onEditStarted: root.beginField("sex")
                    onCommitted: (value) => root.saveField("sex", value)
                }

                EditableField {
                    id: descField
                    label: "个性签名"
                    value: root.descText
                    placeholder: "还没有签名"
                    mode: "multiline"
                    maxLength: 60
                    maxLines: 3
                    busy: root.savingField === "desc" && root.controller && root.controller.profileSaving
                    onEditStarted: root.beginField("desc")
                    onCommitted: (value) => root.saveField("desc", value)
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
                }

                Text {
                    width: parent.width - 96
                    x: 96
                    topPadding: 10
                    text: "登录与搜索用，不可修改。"
                    color: Theme.textHint
                    font.pixelSize: Theme.fontCaption
                    wrapMode: Text.WordWrap
                }
            }
        }
    }
}
