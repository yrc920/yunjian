import QtQuick

// 右侧内容页共用标题栏，切换资料、申请和设置时标题基线保持不变。
Item {
    id: root

    property string title: ""
    property string subtitle: ""
    property bool backVisible: false
    property string backLabel: "返回"
    // 页面内容列的左边距；标题与内容对齐，不各自为政
    property real inset: 20
    signal backRequested()

    width: parent ? parent.width : 0
    height: Theme.toolbarHeight

    Rectangle {
        anchors.fill: parent
        color: Theme.sheet
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: Theme.lineSoft
        }
    }

    Rectangle {
        id: backButton
        visible: root.backVisible
        anchors.left: parent.left
        anchors.leftMargin: root.inset
        anchors.verticalCenter: parent.verticalCenter
        width: 32
        height: 32
        radius: Theme.controlRadius
        color: backArea.containsMouse || activeFocus ? Theme.panelAlt : "transparent"
        border.width: activeFocus ? 1 : 0
        border.color: Theme.focusRing
        activeFocusOnTab: visible
        Accessible.role: Accessible.Button
        Accessible.name: root.backLabel
        Accessible.onPressAction: root.backRequested()
        Keys.onReturnPressed: root.backRequested()
        Keys.onSpacePressed: root.backRequested()
        AppIcon {
            anchors.centerIn: parent
            name: "back"
            size: 17
            color: Theme.textSecondary
        }
        MouseArea {
            id: backArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: root.backRequested()
        }
    }

    Column {
        anchors.left: parent.left
        anchors.leftMargin: root.backVisible ? root.inset + 40 : root.inset
        anchors.right: parent.right
        anchors.rightMargin: 24
        anchors.verticalCenter: parent.verticalCenter
        spacing: root.subtitle.length > 0 ? 2 : 0

        Text {
            width: parent.width
            text: root.title
            color: Theme.textPrimary
            font.pixelSize: Theme.fontTitle
            font.weight: Font.DemiBold
            elide: Text.ElideRight
        }
        Text {
            width: parent.width
            visible: root.subtitle.length > 0
            text: root.subtitle
            color: Theme.textSecondary
            font.pixelSize: Theme.fontCaption
            elide: Text.ElideRight
        }
    }

}
