import QtQuick

// 设置页统一的可点击行：标题、说明和右侧状态同一条基线。
// 设置项不再逐行放图标，减少左侧噪音；选中态用一张纸卡 + 2px 墨线。
Rectangle {
    id: root

    property string title: ""
    property string subtitle: ""
    property string value: ""
    property bool selected: false
    property bool danger: false
    property bool showChevron: false
    property string chevronKind: "chevronRight"
    // 只读行：整行不响应点击，右侧给一个明确的动作按钮（清理这类不可逆操作）
    property bool actionMode: false
    property string actionText: ""
    signal actionTriggered()
    readonly property bool hasAction: actionMode && actionText.length > 0
    signal clicked()

    width: parent ? parent.width : 0
    height: Math.max(48, labels.implicitHeight + 16)
    radius: 8
    color: root.hasAction ? "transparent"
           : selected ? Theme.panelBg
                      : (area.containsMouse || activeFocus ? Theme.panelAlt : "transparent")
    border.width: selected ? 1 : (activeFocus && !root.hasAction ? 1 : 0)
    border.color: selected ? Theme.line : Theme.focusRing
    activeFocusOnTab: !root.hasAction
    Accessible.role: root.hasAction ? Accessible.Grouping : Accessible.Button
    Accessible.name: title
    Accessible.onPressAction: if (!root.hasAction) root.clicked()
    Keys.onReturnPressed: if (!root.hasAction) root.clicked()
    Keys.onSpacePressed: if (!root.hasAction) root.clicked()

    Rectangle {
        visible: root.selected
        x: 0
        y: 9
        width: 2
        height: parent.height - 18
        radius: 1
        color: Theme.primary
    }

    Column {
        id: labels
        x: 16
        anchors.verticalCenter: parent.verticalCenter
        width: Math.max(0, root.hasAction ? root.width - 108 : valueText.x - x - 12)
        spacing: root.subtitle.length > 0 ? 3 : 0

        Text {
            width: parent.width
            text: root.title
            color: root.danger ? Theme.danger : Theme.textPrimary
            font.pixelSize: Theme.fontBody
            font.weight: root.selected ? Font.Medium : Font.Normal
            elide: Text.ElideRight
        }
        Text {
            width: parent.width
            visible: root.subtitle.length > 0
            text: root.subtitle
            color: Theme.textSecondary
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.WrapAtWordBoundaryOrAnywhere
        }
    }

    Text {
        id: valueText
        width: root.value.length > 0 ? Math.min(implicitWidth, root.width * 0.34) : 0
        visible: !root.hasAction && root.value.length > 0
        anchors.right: root.showChevron ? chevron.left : parent.right
        anchors.rightMargin: root.showChevron ? 8 : 16
        anchors.verticalCenter: parent.verticalCenter
        text: root.value
        textFormat: Text.PlainText
        color: Theme.textSecondary
        font.pixelSize: Theme.fontCaption
        elide: Text.ElideRight
    }

    AppButton {
        visible: root.hasAction
        anchors.right: parent.right
        anchors.rightMargin: 16
        anchors.verticalCenter: parent.verticalCenter
        width: 64
        height: 32
        text: root.actionText
        variant: "outline"
        onClicked: root.actionTriggered()
    }

    AppIcon {
        id: chevron
        visible: root.showChevron
        anchors.right: parent.right
        anchors.rightMargin: 14
        anchors.verticalCenter: parent.verticalCenter
        name: root.chevronKind
        size: 14
        color: Theme.textHint
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        enabled: !root.hasAction
        cursorShape: root.hasAction ? Qt.ArrowCursor : Qt.PointingHandCursor
        onClicked: {
            root.forceActiveFocus(Qt.MouseFocusReason)
            root.clicked()
        }
    }
}
