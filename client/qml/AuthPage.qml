import QtQuick

// 认证页：桌面底上放一张信纸卡，卡内只有角色与表单。
// 右上角留给外观切换按钮，不再放邮戳，避免两个控件互相遮挡。
// 表单列与卡片同宽居中；内容超过窗口时在卡内滚动。
Item {
    id: root

    default property alias content: column.data
    property int formWidth: 328
    property bool backVisible: false
    signal backRequested()

    data: [
        Rectangle {
            anchors.fill: parent
            color: Theme.desk
        },
        // 返回登录：固定在卡片左上角，与表单共用 32px 内缘
        Item {
            visible: root.backVisible
            x: 30
            y: 22
            width: backLabel.implicitWidth + 30
            height: 32
            z: 2
            activeFocusOnTab: visible
            Accessible.role: Accessible.Button
            Accessible.name: "返回登录"
            Accessible.onPressAction: root.backRequested()
            Keys.onReturnPressed: root.backRequested()
            Keys.onSpacePressed: root.backRequested()

            AppIcon {
                id: backIcon
                x: 0
                anchors.verticalCenter: parent.verticalCenter
                name: "back"
                size: 14
                color: backArea.containsMouse ? Theme.textPrimary : Theme.textSecondary
            }
            Text {
                id: backLabel
                x: 20
                anchors.verticalCenter: parent.verticalCenter
                text: "返回登录"
                color: backArea.containsMouse ? Theme.textPrimary : Theme.textSecondary
                font.pixelSize: Theme.fontSmall
            }
            MouseArea {
                id: backArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.backRequested()
            }
        },

        Rectangle {
            id: card
            anchors.fill: parent
            anchors.margins: 14
            radius: Theme.dialogRadius
            color: Theme.panelBg
            border.width: 1
            border.color: Theme.line
        },
        Flickable {
            id: scroll
            anchors.fill: card
            // 三页共用同一顶距：表单自己居中，返回链接只占页眉，不再把内容整体推低
            anchors.topMargin: 24
            clip: true
            contentWidth: width
            contentHeight: Math.max(height, column.y + column.height + 16)
            interactive: contentHeight > height
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: column
                width: Math.min(root.formWidth, scroll.width - 48)
                x: (scroll.width - width) / 2
                y: Math.max(8, (scroll.height - height - 16) / 2)
                spacing: 0
            }
        }
    ]
}
