import QtQuick
import QtQuick.Controls

// 外观三选一：跟随系统 / 浅色 / 深色。点外部或 Esc 关闭，选中项用主色点标记。
Popup {
    id: root
    width: 168
    padding: 6
    modal: false
    dim: false
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    readonly property var options: [
        { value: "system", label: "跟随系统" },
        { value: "light", label: "浅色" },
        { value: "dark", label: "深色" }
    ]

    background: Rectangle {
        color: Theme.panelBg
        radius: 10
        border.width: 1
        border.color: Theme.line
    }

    contentItem: Column {
        width: root.availableWidth

        Repeater {
            model: root.options

            delegate: Rectangle {
                width: parent.width
                height: 34
                radius: 7
                color: rowArea.containsMouse ? Theme.panelAlt : "transparent"

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    text: modelData.label
                    color: Theme.textPrimary
                    font.pixelSize: 13
                }

                Rectangle {
                    visible: Theme.mode === modelData.value
                    width: 6
                    height: 6
                    radius: 3
                    color: Theme.primary
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                }

                MouseArea {
                    id: rowArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        Theme.setMode(modelData.value)
                        root.close()
                    }
                }
            }
        }
    }
}
