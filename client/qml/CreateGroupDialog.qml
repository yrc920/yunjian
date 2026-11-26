import QtQuick
import QtQuick.Controls
import YunJian

// 发起群聊：勾选好友 + 群名，创建成功后由 ChatBridge 直接进入新群。
// 至少选一位好友；群名留空时服务端回落到"群聊"。
Popup {
    id: root
    property var controller: null
    readonly property var candidates: controller ? controller.groupCandidates : []
    readonly property bool busy: controller ? controller.createGroupBusy : false
    readonly property string message: controller ? controller.createGroupMessage : ""
    property var selected: []

    function toggle(uid) {
        const next = []
        let found = false
        for (let i = 0; i < selected.length; ++i) {
            if (selected[i] === uid)
                found = true
            else
                next.push(selected[i])
        }
        if (!found)
            next.push(uid)
        selected = next
    }

    function reset() {
        selected = []
        groupName.text = ""
    }

    anchors.centerIn: parent
    modal: true
    focus: true
    width: 340
    height: form.implicitHeight + 48
    padding: 0
    Overlay.modal: Rectangle { color: Theme.overlay }
    closePolicy: busy ? Popup.NoAutoClose : Popup.CloseOnEscape
    background: Rectangle {
        radius: Theme.dialogRadius
        color: Theme.surfaceFloating
        border.width: 1
        border.color: Theme.line
    }

    Column {
        id: form
        x: 24
        y: 24
        width: parent.width - 48
        height: implicitHeight
        spacing: 12

        Text {
            text: "发起群聊"
            color: Theme.textPrimary
            font.pixelSize: Theme.fontTitle
            font.weight: Font.DemiBold
        }
        Text {
            width: parent.width
            text: "勾选好友，给群起个名字；不填就用「群聊」"
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.Wrap
        }
        AppInput {
            id: groupName
            width: parent.width
            maximumLength: 30
            placeholder: "群名称（可选）"
        }

        Item {
            width: parent.width
            height: 22
            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: "选择好友 · 已选 " + root.selected.length
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSmall
                font.weight: Font.Medium
            }
        }

        ListView {
            width: parent.width
            height: Math.min(3, Math.max(1, root.candidates.length)) * 50 - 2
            clip: true
            model: root.candidates
            spacing: 2
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            delegate: Item {
                width: ListView.view.width
                height: 48
                Rectangle {
                    anchors.fill: parent
                    radius: 8
                    color: pickArea.containsMouse ? Theme.panelAlt : "transparent"
                }
                Avatar {
                    x: 8
                    width: 32
                    height: 32
                    radius: 12
                    anchors.verticalCenter: parent.verticalCenter
                    source: modelData.icon
                    name: modelData.name
                }
                Text {
                    x: 52
                    width: parent.width - 96
                    anchors.verticalCenter: parent.verticalCenter
                    text: modelData.name
                    textFormat: Text.PlainText
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontSmall
                    elide: Text.ElideRight
                }
                Rectangle {
                    anchors.right: parent.right
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    width: 18
                    height: 18
                    radius: 4
                    color: root.selected.indexOf(modelData.uid) >= 0 ? Theme.primary : "transparent"
                    border.width: 1
                    border.color: root.selected.indexOf(modelData.uid) >= 0
                        ? Theme.primary : Theme.fieldBorder
                }
                MouseArea {
                    id: pickArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.toggle(modelData.uid)
                }
            }
        }

        Text {
            width: parent.width
            visible: root.message.length > 0
            height: visible ? implicitHeight : 0
            text: root.message
            color: root.busy ? Theme.textSecondary : Theme.danger
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.Wrap
        }

        Row {
            spacing: 12
            anchors.right: parent.right
            AppButton {
                width: 96
                height: 40
                text: "取消"
                variant: "secondary"
                enabled: !root.busy
                onClicked: {
                    root.reset()
                    controller.closeCreateGroup()
                }
            }
            AppButton {
                width: 112
                height: 40
                text: root.busy ? "正在创建…" : "创建"
                busy: root.busy
                enabled: root.selected.length > 0
                onClicked: controller.createGroup(groupName.text, root.selected)
            }
        }
    }

    // 关闭：和资料/好友弹层同一个 32 的图标位
    Rectangle {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.rightMargin: 20
        anchors.topMargin: 20
        width: 32
        height: 32
        radius: Theme.controlRadius
        color: closeArea.containsMouse || activeFocus ? Theme.panelAlt : "transparent"
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: "关闭"
        Accessible.onPressAction: controller.closeCreateGroup()
        Keys.onReturnPressed: controller.closeCreateGroup()
        Keys.onSpacePressed: controller.closeCreateGroup()
        AppIcon {
            anchors.centerIn: parent
            name: "close"
            size: 16
            color: Theme.textSecondary
        }
        MouseArea {
            id: closeArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: controller.closeCreateGroup()
        }
    }

    onClosed: {
        reset()
        //Esc 或点击弹层外部由 Popup 自己关闭，桥上的状态要跟着复位，
        //否则下次点「发起群聊」时桥仍认为弹层开着，界面看起来毫无反应
        if (controller && controller.createGroupOpen)
            controller.closeCreateGroup()
    }
}
