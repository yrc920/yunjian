import QtQuick

// 按钮四档：primary 主操作，secondary 填充次要，outline 描边次要，danger 不可逆操作。
// 颜色跟随悬停/按下/忙碌，焦点态用主色描边而不是换色。
Rectangle {
    id: root
    property string text: ""
    property string iconName: ""
    property string variant: "primary"
    property bool busy: false
    signal clicked()
    implicitHeight: Theme.buttonHeight
    height: implicitHeight
    radius: Theme.controlRadius
    activeFocusOnTab: enabled && !busy
    // 危险动作不抢主色：浅红底 + 红字，悬停加深，避免和"保存/确定"同一个视觉重量
    readonly property bool _danger: variant === "danger"
    readonly property bool _outline: variant === "outline"
    readonly property color _dangerBg: Qt.rgba(Theme.danger.r, Theme.danger.g, Theme.danger.b, 0.10)
    readonly property color _dangerHover: Qt.rgba(Theme.danger.r, Theme.danger.g, Theme.danger.b, 0.18)
    readonly property color _bg: !enabled ? Theme.panelAlt
                                      : _danger ? _dangerBg
                                      : _outline ? "transparent"
                                      : variant === "secondary" ? Theme.panelAlt : Theme.primary
    readonly property color _bgHover: _danger ? _dangerHover
                                     : variant === "secondary" ? Theme.selected
                                     : _outline ? Theme.panelAlt : Theme.primaryHover
    readonly property color _bgPressed: _danger ? _dangerHover
                                       : variant === "secondary" ? Theme.selected
                                       : _outline ? Theme.selected : Theme.primaryPressed
    readonly property color _fg: !enabled ? Theme.textHint
                                 : _danger ? Theme.danger
                                 : _outline ? Theme.textPrimary
                                 : variant === "secondary" ? Theme.textPrimary : Theme.primaryText
    // 忙碌时按钮仍保持主色，但不再响应点击，避免重复提交
    color: !enabled ? Theme.panelAlt
                    : busy && variant === "primary" ? Theme.primaryHover
                    : area.pressed ? _bgPressed
                    : area.containsMouse ? _bgHover : _bg
    border.width: activeFocus ? 2 : (_outline ? 1 : 0)
    border.color: activeFocus ? Theme.focusRing : (_outline ? Theme.line : "transparent")
    opacity: 1
    scale: area.pressed && !busy ? 0.99 : 1
    Accessible.role: Accessible.Button
    Accessible.name: text
    Accessible.onPressAction: if (enabled && !busy) clicked()
    Keys.onReturnPressed: if (!busy) clicked()
    Keys.onSpacePressed: if (!busy) clicked()
    Behavior on color { ColorAnimation { duration: 140 } }
    Behavior on scale { NumberAnimation { duration: 110 } }

    // 文字与图标作为一组居中，图标为空时不占宽度
    Row {
        anchors.centerIn: parent
        spacing: 8
        Text { visible: root.text.length > 0; text: root.text; color: root._fg; font.pixelSize: Theme.fontBody; font.weight: Font.Medium }
        AppIcon { visible: root.iconName.length > 0; name: root.iconName; color: root._fg; size: 17; anchors.verticalCenter: parent.verticalCenter }
    }
    MouseArea {
        id: area
        anchors.fill: parent
        enabled: root.enabled && !root.busy
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.clicked()
    }
}
