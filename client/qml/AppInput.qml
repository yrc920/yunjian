import QtQuick

//表单标签固定在输入框顶部，输入和切换焦点时保持位置稳定。
// 密码可见开关只改 echoMode，不在 QML 里缓存明文。
Rectangle {
    id: root
    property alias text: input.text
    // 与服务端表的字符上限保持一致，超出部分直接输入不进去
    property alias maximumLength: input.maximumLength
    property string label: ""
    property string placeholder: ""
    property bool labeled: false
    property bool password: false
    property bool reveal: false
    property bool invalid: false
    readonly property bool inputFocused: input.activeFocus
    readonly property bool composing: input.inputMethodComposing
    signal textEdited()
    signal accepted()
    signal revealToggled()
    function forceInputFocus() { input.forceActiveFocus() }

    height: Theme.fieldHeight
    radius: Theme.controlRadius
    color: Theme.panelBg
    // 焦点只用 1px 主色描边与标签变色表达；外发光会让输入框看起来像描了两道边
    border.width: 1
    border.color: invalid ? Theme.danger : input.activeFocus ? Theme.focusRing : Theme.fieldBorder
    Behavior on border.color { ColorAnimation { duration: 130 } }

    Text {
        x: 14
        y: 5
        visible: root.labeled && root.label.length > 0
        text: root.label
        color: root.invalid ? Theme.danger : Theme.textSecondary
        font.pixelSize: Theme.fontCaption
    }

    // 密码框右侧留出显示开关的位置；输入法组字期间隐藏占位文案
    TextInput {
        id: input
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 14
        anchors.rightMargin: root.password ? 46 : 14
        y: root.labeled ? 20 : 0
        height: parent.height - y - (root.labeled ? 4 : 0)
        verticalAlignment: TextInput.AlignVCenter
        font.pixelSize: Theme.fontBody
        color: Theme.textPrimary
        selectionColor: Theme.selected
        selectedTextColor: Theme.textPrimary
        clip: true
        selectByMouse: true
        activeFocusOnTab: true
        echoMode: root.password && !root.reveal ? TextInput.Password : TextInput.Normal
        passwordCharacter: "•"
        passwordMaskDelay: 0
        inputMethodHints: root.password ? Qt.ImhHiddenText | Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase : Qt.ImhNone
        Accessible.name: root.label.length > 0 ? root.label : root.placeholder
        onTextEdited: root.textEdited()
        onAccepted: root.accepted()
    }
    //占位只提供输入示例，字段名称由固定标签承担。
    Text {
        anchors.left: parent.left
        anchors.leftMargin: 14
        y: input.y
        height: input.height
        verticalAlignment: Text.AlignVCenter
        visible: input.length === 0 && !input.inputMethodComposing
        text: root.placeholder
        color: Theme.textHint
        font.pixelSize: Theme.fontBody
    }
    TapHandler { onTapped: input.forceActiveFocus() }
    // 密码可见开关：只改 echoMode，不在 QML 里缓存密码明文
    Item {
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        width: 44
        height: parent.height
        visible: root.password
        activeFocusOnTab: visible
        Accessible.role: Accessible.Button
        Accessible.name: root.reveal ? "隐藏密码" : "显示密码"
        Keys.onReturnPressed: root.revealToggled()
        Keys.onSpacePressed: root.revealToggled()
        AppIcon { anchors.centerIn: parent; size: 19; name: root.reveal ? "eye" : "eyeOff"; color: eyeArea.containsMouse || parent.activeFocus ? Theme.textPrimary : Theme.textSecondary }
        MouseArea { id: eyeArea; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: root.revealToggled() }
    }
}
