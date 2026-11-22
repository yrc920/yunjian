import QtQuick

// 设置页统一的可点击行：标题、说明和右侧状态保持同一条基线。
Rectangle {
    id: root

    property string iconName: ""
    property string title: ""
    property string subtitle: ""
    property string value: ""
    property bool selected: false
    property bool danger: false
    property bool showChevron: false
    signal clicked()

    width: parent ? parent.width : 0
    height: Math.max(56, labels.implicitHeight + 20)
    radius: Theme.controlRadius
    color: selected ? Theme.selected : (area.containsMouse || activeFocus ? Theme.panelAlt : "transparent")
    border.width: activeFocus ? 1 : 0
    border.color: Theme.focusRing
    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Accessible.name: title
    Accessible.onPressAction: root.clicked()
    Keys.onReturnPressed: root.clicked()
    Keys.onSpacePressed: root.clicked()

    AppIcon {
        x: 16
        anchors.verticalCenter: parent.verticalCenter
        name: root.iconName
        size: 20
        color: root.danger ? Theme.danger : root.selected ? Theme.primary : Theme.textSecondary
    }

    Column {
        id: labels
        x: 52
        anchors.verticalCenter: parent.verticalCenter
        width: Math.max(0, valueText.x - x - 12)
        spacing: root.subtitle.length > 0 ? 3 : 0

        Text {
            width: parent.width
            text: root.title
            color: root.danger ? Theme.danger : Theme.textPrimary
            font.pixelSize: Theme.fontBody
            font.weight: root.selected ? Font.DemiBold : Font.Normal
            elide: Text.ElideRight
        }
        Text {
            width: parent.width
            visible: root.subtitle.length > 0
            text: root.subtitle
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.WrapAtWordBoundaryOrAnywhere
        }
    }

    Text {
        id: valueText
        width: root.value.length > 0 ? Math.min(implicitWidth, root.width * 0.32) : 0
        visible: root.value.length > 0
        anchors.right: root.showChevron ? chevron.left : parent.right
        anchors.rightMargin: root.showChevron ? 8 : 16
        anchors.verticalCenter: parent.verticalCenter
        text: root.value
        color: Theme.textSecondary
        font.pixelSize: Theme.fontSmall
        elide: Text.ElideRight
    }

    AppIcon {
        id: chevron
        visible: root.showChevron
        anchors.right: parent.right
        anchors.rightMargin: 14
        anchors.verticalCenter: parent.verticalCenter
        name: "chevronRight"
        size: 15
        color: Theme.textHint
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: {
            root.forceActiveFocus(Qt.MouseFocusReason)
            root.clicked()
        }
    }
}
