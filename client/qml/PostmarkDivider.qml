import QtQuick

// 日期分隔：一条发丝线、一枚火漆点、一句日期，像信纸上的邮戳。
Item {
    id: root
    property string text: ""

    width: parent ? parent.width : 0
    height: 22

    Text {
        id: label
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.verticalCenter: parent.verticalCenter
        text: root.text
        textFormat: Text.PlainText
        color: Theme.textHint
        font.pixelSize: Theme.fontCaption
    }

    Rectangle {
        id: dot
        anchors.right: label.left
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        width: 4
        height: 4
        radius: 2
        color: Theme.seal
        visible: label.text.length > 0
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: dot.left
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        height: 1
        color: Theme.lineSoft
    }

    Rectangle {
        anchors.left: label.right
        anchors.leftMargin: 8
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        height: 1
        color: Theme.lineSoft
    }
}
