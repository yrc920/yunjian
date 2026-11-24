import QtQuick

// 验证码按钮：与输入框同高，作为行内次要动作，倒计时期间置灰并阻止重复点击。
Rectangle {
    id: root
    property string text: ""
    property bool usable: true
    signal clicked()
    height: Theme.fieldHeight
    radius: Theme.controlRadius
    color: area.containsMouse && usable ? Theme.selected : Theme.panelAlt
    border.width: activeFocus ? 1 : 0
    border.color: Theme.primary
    activeFocusOnTab: usable
    opacity: usable ? 1 : 0.6
    Accessible.role: Accessible.Button
    Accessible.name: text
    Keys.onReturnPressed: if (usable) clicked()
    Keys.onSpacePressed: if (usable) clicked()
    Behavior on color { ColorAnimation { duration: 130 } }
    Text {
        anchors.centerIn: parent
        text: root.text
        color: root.usable ? Theme.textPrimary : Theme.textSecondary
        font.pixelSize: Theme.fontSmall
        font.weight: Font.Medium
    }
    MouseArea { id: area; anchors.fill: parent; enabled: root.usable; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: root.clicked() }
}
