import QtQuick
import QtQuick.Controls
import XiguChat

// 关于：一封写给用户的短信。品牌、版本、作者的话都在这里，边栏不再挂版本号。
Popup {
    id: root

    anchors.centerIn: parent
    modal: true
    focus: true
    width: 440
    height: body.implicitHeight + 48
    padding: 0
    Overlay.modal: Rectangle { color: Theme.overlay }
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    background: Rectangle {
        radius: Theme.dialogRadius
        color: Theme.surfaceFloating
        border.width: 1
        border.color: Theme.line
    }

    Column {
        id: body
        x: 24
        y: 24
        width: parent.width - 48
        spacing: 0

        Item {
            width: parent.width
            height: 32

            // 品牌、标题、版本同一条基线；版本当后缀，不再单独占一行
            Row {
                spacing: 12

                AppIcon {
                    anchors.verticalCenter: parent.verticalCenter
                    name: "brand"
                    size: 24
                    color: Theme.primary
                }
                Text {
                    id: appTitle
                    text: "信笺"
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontDisplay
                    font.weight: Font.DemiBold
                }
                Text {
                    anchors.baseline: appTitle.baseline
                    text: "版本 " + Qt.application.version
                    color: Theme.textHint
                    font.pixelSize: Theme.fontCaption
                }
            }
        }

        Item { width: parent.width; height: 18 }

        Rectangle {
            width: parent.width
            height: 1
            color: Theme.lineSoft
        }

        Item { width: parent.width; height: 22 }

        Column {
            width: parent.width
            spacing: 12

            Text {
                width: parent.width
                text: "写给打开信笺的你："
                color: Theme.textPrimary
                font.pixelSize: Theme.fontBody
                font.weight: Font.Medium
            }
            Text {
                width: parent.width
                text: "做这个应用的起点，是想有一个不催人的聊天工具。没有已读回执，也没有一直跳的红点，消息安安静静地落在纸上，等你有空的时候再回。"
                color: Theme.textSecondary
                font.pixelSize: Theme.fontBody
                lineHeight: 1.55
                wrapMode: Text.Wrap
            }
            Text {
                width: parent.width
                text: "如果它让你和在意的人多说了几句，或者在某个夜里陪你坐了一会儿，那它就没有白做。"
                color: Theme.textSecondary
                font.pixelSize: Theme.fontBody
                lineHeight: 1.55
                wrapMode: Text.Wrap
            }
            Text {
                width: parent.width
                text: "谢谢你愿意打开它。"
                color: Theme.textSecondary
                font.pixelSize: Theme.fontBody
                lineHeight: 1.55
                wrapMode: Text.Wrap
            }
        }

        Item { width: parent.width; height: 20 }

        Text {
            width: parent.width
            horizontalAlignment: Text.AlignRight
            text: "—— 西故"
            color: Theme.textHint
            font.pixelSize: Theme.fontSmall
        }

        Item { width: parent.width; height: 22 }

        Item {
            width: parent.width
            height: 40

            AppButton {
                anchors.right: parent.right
                width: 80
                height: 40
                text: "关闭"
                variant: "secondary"
                onClicked: root.close()
            }
        }
    }

    // 关闭键与发起群聊同一规格：32、右上 20、图标 16、hover panelAlt
    Rectangle {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.rightMargin: 20
        anchors.topMargin: 20
        width: 32
        height: 32
        radius: Theme.controlRadius
        color: closeArea.containsMouse || closeArea.activeFocus ? Theme.panelAlt : "transparent"
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: "关闭"
        Accessible.onPressAction: root.close()
        Keys.onReturnPressed: root.close()
        Keys.onSpacePressed: root.close()

        AppIcon {
            anchors.centerIn: parent
            name: "close"
            size: 16
            color: Theme.textSecondary
        }
        MouseArea {
            id: closeArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: root.close()
        }
    }
}
