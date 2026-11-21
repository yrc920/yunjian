import QtQuick

// 认证页共用的品牌行与表单容器：品牌与表单同宽左对齐，长表单可随内容滚动。
Item {
    id: root

    default property alias content: column.data
    property int formWidth: 328

    data: [
        Rectangle {
            anchors.fill: parent
            color: Theme.panelBg
        },
        Row {
            x: (root.width - root.formWidth) / 2
            y: 24
            spacing: 10

            BrandMark {
                size: 22
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "XiguChat"
                color: Theme.textPrimary
                font.pixelSize: 13
                font.weight: Font.Medium
            }
        },
        Flickable {
            id: scroll
            anchors.fill: parent
            anchors.topMargin: 76
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
