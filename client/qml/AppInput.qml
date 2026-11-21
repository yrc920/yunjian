import QtQuick

// 表单输入：默认保持占位文案的老样子，labeled 打开后标签只在聚焦或有内容时浮到边框上。
// 密码可见开关只改 echoMode，不在 QML 里缓存明文。
Rectangle {
    id: root
    property alias text: input.text
    // 与服端表的字符上限保持一致，超出部分直接输入不进去
    property alias maximumLength: input.maximumLength
    property string label: ""
    property string placeholder: ""
    property bool labeled: false
    property bool password: false
    property bool reveal: false
    property bool invalid: false
    readonly property bool inputFocused: input.activeFocus
    readonly property bool composing: input.inputMethodComposing
    // 浮起条件：开了标签，且已经聚焦或已有内容；空态由框内提示承担
    readonly property bool floating: labeled && label.length > 0
        && (input.activeFocus || input.length > 0)
    signal textEdited()
    signal accepted()
    signal revealToggled()
    function forceInputFocus() { input.forceActiveFocus() }

    height: Theme.fieldHeight
    radius: Theme.radius
    color: Theme.panelBg
    // 焦点只用 1px 主色描边与标签变色表达；外发光会让输入框看起来像描了两道边
    border.width: 1
    border.color: invalid ? Theme.danger : input.activeFocus ? Theme.primary : Theme.fieldBorder
    Behavior on border.color { ColorAnimation { duration: 130 } }

    // 浮动标签：用底色在边框线上挖出缺口，标签始终压在这条线上
    Item {
        x: 8
        width: labelText.implicitWidth + 10
        height: 16
        y: root.floating ? -8 : -2
        opacity: root.floating ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 120 } }
        Behavior on y { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }

        Rectangle {
            anchors.fill: parent
            color: Theme.panelBg
        }
        Text {
            id: labelText
            anchors.centerIn: parent
            text: root.label
            color: root.invalid ? Theme.danger : root.inputFocused ? Theme.primary : Theme.textSecondary
            font.pixelSize: Theme.fontCaption
        }
    }

    // 右侧留出眼睛按钮的位置，密码可见时再收回；组合态期间隐藏占位文案
    TextInput {
        id: input
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 14
        anchors.rightMargin: root.password ? 46 : 14
        anchors.verticalCenter: parent.verticalCenter
        height: parent.height
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
    // 框内提示：未浮起时写标签词；浮起后若还留了占位文案（如「6–15 位」），在聚焦空态继续给出
    Text {
        anchors.left: parent.left
        anchors.leftMargin: 14
        anchors.verticalCenter: parent.verticalCenter
        visible: input.length === 0 && !input.inputMethodComposing
        text: root.floating ? root.placeholder
                            : (root.labeled && root.label.length > 0 ? root.label : root.placeholder)
        color: Theme.textHint
        font.pixelSize: Theme.fontBody
    }
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
