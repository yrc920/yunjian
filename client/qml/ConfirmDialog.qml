import QtQuick
import QtQuick.Controls

// 全应用统一的确认弹层：标题、后果说明、取消 + 动词按钮。
// 危险操作的确认按钮用火漆色，不写"确定"；Esc 与点遮罩都算取消。
Popup {
    id: root

    property string title: ""
    property string body: ""
    property string confirmText: "确定"
    property string cancelText: "取消"
    property bool danger: false
    property bool busy: false
    signal confirmed()
    signal cancelled()

    property bool _confirmed: false

    anchors.centerIn: parent
    modal: true
    focus: true
    width: Math.min(420, parent ? parent.width - 64 : 420)
    height: content.implicitHeight + 48
    padding: 0
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    Overlay.modal: Rectangle { color: Theme.overlay }

    function ask() {
        _confirmed = false
        open()
    }

    onOpened: Qt.callLater(cancelButton.forceActiveFocus)
    onClosed: {
        if (!_confirmed)
            cancelled()
        _confirmed = false
    }

    background: Rectangle {
        radius: Theme.dialogRadius
        color: Theme.surfaceFloating
        border.width: 1
        border.color: Theme.line
    }

    Column {
        id: content
        x: 24
        y: 24
        width: parent.width - 48
        spacing: 12

        Text {
            width: parent.width
            text: root.title
            textFormat: Text.PlainText
            color: Theme.textPrimary
            font.pixelSize: Theme.fontTitle
            font.weight: Font.DemiBold
            wrapMode: Text.WrapAtWordBoundaryOrAnywhere
        }

        Text {
            width: parent.width
            text: root.body
            textFormat: Text.PlainText
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSmall
            lineHeight: 1.5
            wrapMode: Text.WrapAtWordBoundaryOrAnywhere
        }

        Item { width: 1; height: 4 }

        Row {
            width: parent.width
            spacing: 12

            CodeButton {
                id: cancelButton
                width: (parent.width - 12) / 2
                height: 40
                text: root.cancelText
                onClicked: root.close()
            }

            AppButton {
                width: (parent.width - 12) / 2
                height: 40
                variant: root.danger ? "danger" : "primary"
                text: root.busy ? "处理中" : root.confirmText
                busy: root.busy
                onClicked: {
                    if (root.busy)
                        return
                    root._confirmed = true
                    root.confirmed()
                }
            }
        }
    }
}
