import QtQuick

// 认证与表单共用的输入框：标签平时就是占位词，聚焦或有内容时浮到框上沿。
// 这样空框只显示一行提示，不占两行；密码可见开关只改 echoMode，不缓存明文。
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
    readonly property bool hasLabel: labeled && label.length > 0
    // 聚焦、已有内容或正在组字时，标签离开输入区浮到上沿
    readonly property bool floating: hasLabel && (input.activeFocus || input.length > 0 || composing)
    signal textEdited()
    signal accepted()
    signal revealToggled()
    function forceInputFocus() { input.forceActiveFocus() }

    height: Theme.fieldHeight
    radius: Theme.controlRadius
    color: Theme.fieldBg
    // 焦点只用 1px 主色描边与标签变色表达；外发光会让输入框看起来像描了两道边
    border.width: 1
    border.color: invalid ? Theme.danger : input.activeFocus ? Theme.focusRing : Theme.fieldBorder
    Behavior on border.color { ColorAnimation { duration: 130 } }

    // 标签同时承担占位词：未聚焦时居中；聚焦或有内容后浮到上边框线上，
    // 用一小块卡片底色压住描边，像信封上的地址签，不再占输入区一行。
    Text {
        id: labelText
        x: 16
        z: 3
        text: root.label
        color: root.invalid ? Theme.danger
             : root.floating ? (root.inputFocused ? Theme.focusRing : Theme.textSecondary)
                             : Theme.textHint
        font.pixelSize: Theme.fontBody
        font.weight: root.floating ? Font.Medium : Font.Normal
        scale: root.floating ? 0.78 : 1
        transformOrigin: Item.TopLeft
        y: root.floating ? -(height * scale) / 2 : (root.height - height) / 2
        visible: root.hasLabel
        Behavior on y { NumberAnimation { duration: 130; easing.type: Easing.OutCubic } }
        Behavior on scale { NumberAnimation { duration: 130; easing.type: Easing.OutCubic } }
        Behavior on color { ColorAnimation { duration: 130 } }
    }

    // 标签浮起时垫一块与卡片同色的底，压住边框线，形成“字压在框上”的开口
    Rectangle {
        x: 12
        y: -1.5
        z: 2
        width: labelText.implicitWidth * labelText.scale + 8
        height: 3
        radius: 1.5
        color: Theme.panelBg
        visible: root.hasLabel
        opacity: root.floating ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 110 } }
    }

    // 密码框右侧留出显示开关的位置；输入法组字期间隐藏占位文案
    TextInput {
        id: input
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 16
        anchors.rightMargin: root.password ? 46 : 16
        // 标签浮到框上后，输入文字重新占满整格并垂直居中
        y: 0
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

    // 标签浮起后，placeholder 才作为补充提示出现；未浮起时标签自己就是提示词
    Text {
        anchors.left: parent.left
        anchors.leftMargin: 16
        anchors.right: parent.right
        anchors.rightMargin: root.password ? 46 : 16
        y: input.y
        height: input.height
        verticalAlignment: Text.AlignVCenter
        visible: root.floating && input.length === 0 && !input.inputMethodComposing
        text: root.placeholder
        color: Theme.textHint
        font.pixelSize: Theme.fontBody
        elide: Text.ElideRight
    }

    // 没有标签的输入框仍然用居中占位词
    Text {
        anchors.left: parent.left
        anchors.leftMargin: 16
        anchors.right: parent.right
        anchors.rightMargin: root.password ? 46 : 16
        anchors.verticalCenter: parent.verticalCenter
        visible: !root.hasLabel && input.length === 0 && !input.inputMethodComposing
        text: root.placeholder
        color: Theme.textHint
        font.pixelSize: Theme.fontBody
        elide: Text.ElideRight
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
