import QtQuick

// 联系人页与聊天页共用的搜索框：只负责输入、焦点和清空，查询由调用方决定。
// text 是只读的输入内容，外部要改内容时调用 setText，避免绑定被打断后不同步。
Rectangle {
    id: root

    property string placeholder: "搜索"
    property string inputObjectName: ""
    readonly property string text: input.text
    readonly property bool inputFocused: input.activeFocus
    signal textEdited(string value)
    signal accepted()
    signal escapePressed()
    signal focusGained()
    signal clearRequested()

    implicitHeight: 36
    radius: Theme.controlRadius
    color: Theme.fieldBg
    border.width: 1
    border.color: input.activeFocus || clearButton.activeFocus ? Theme.focusRing : Theme.lineSoft

    function forceInputFocus() {
        input.forceActiveFocus()
    }

    function clearInputFocus() {
        input.focus = false
    }

    function setText(value) {
        if (input.text !== value)
            input.text = value
    }

    AppIcon {
        x: 11
        anchors.verticalCenter: parent.verticalCenter
        name: "search"
        size: 15
        color: Theme.textHint
    }

    TextInput {
        id: input
        objectName: root.inputObjectName
        anchors.left: parent.left
        anchors.leftMargin: 35
        anchors.right: parent.right
        anchors.rightMargin: clearButton.visible ? 32 : 10
        anchors.verticalCenter: parent.verticalCenter
        height: 28
        verticalAlignment: TextInput.AlignVCenter
        font.pixelSize: 13
        color: Theme.textPrimary
        selectionColor: Theme.selected
        selectedTextColor: Theme.textPrimary
        clip: true
        selectByMouse: true
        activeFocusOnTab: true
        Accessible.name: root.placeholder
        onTextEdited: root.textEdited(text)
        onAccepted: root.accepted()
        onActiveFocusChanged: if (activeFocus) root.focusGained()
        Keys.onEscapePressed: {
            root.escapePressed()
            root.clearInputFocus()
        }
    }

    Text {
        x: 35
        anchors.verticalCenter: parent.verticalCenter
        visible: input.length === 0 && !input.inputMethodComposing
        text: root.placeholder
        color: Theme.textHint
        font.pixelSize: 13
    }

    Rectangle {
        id: clearButton
        visible: input.length > 0
        anchors.right: parent.right
        anchors.rightMargin: 5
        anchors.verticalCenter: parent.verticalCenter
        width: 26
        height: 26
        radius: 6
        color: clearArea.containsMouse || activeFocus ? Theme.selected : "transparent"
        activeFocusOnTab: visible
        Accessible.role: Accessible.Button
        Accessible.name: "清空搜索"
        Accessible.onPressAction: root.clearRequested()
        Keys.onSpacePressed: root.clearRequested()
        Keys.onReturnPressed: root.clearRequested()
        Keys.onEnterPressed: root.clearRequested()
        AppIcon {
            anchors.centerIn: parent
            name: "close"
            size: 13
            color: Theme.textSecondary
        }
        MouseArea {
            id: clearArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: root.clearRequested()
        }
    }
}
