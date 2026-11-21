import QtQuick
import QtQuick.Controls

// 多行输入：与 AppInput 共用边框、圆角与焦点色，用于群公告这类需要换行的文本。
// 不做输入拦截，超限时描边变红并由调用方决定是否允许保存。
Rectangle {
    id: root
    property alias text: area.text
    property string placeholder: ""
    property int maxLength: 200
    readonly property bool overLimit: area.length > maxLength
    readonly property bool inputFocused: area.activeFocus
    signal textEdited()

    implicitHeight: 108
    radius: Theme.radius
    color: Theme.panelBg
    border.width: 1
    border.color: overLimit ? Theme.danger
                : area.activeFocus ? Theme.primary : Theme.fieldBorder
    Behavior on border.color { ColorAnimation { duration: 130 } }

    function forceInputFocus() {
        area.forceActiveFocus()
    }

    ScrollView {
        anchors.fill: parent
        anchors.margins: 8
        anchors.bottomMargin: 24
        clip: true
        ScrollBar.vertical.policy: ScrollBar.AsNeeded

        TextArea {
            id: area
            placeholderText: root.placeholder
            placeholderTextColor: Theme.textHint
            color: Theme.textPrimary
            font.pixelSize: Theme.fontBody
            wrapMode: TextArea.Wrap
            selectByMouse: true
            background: null
            onTextChanged: root.textEdited()
        }
    }

    Text {
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 6
        visible: root.inputFocused || root.overLimit
        text: area.length + "/" + root.maxLength
        color: root.overLimit ? Theme.danger : Theme.textHint
        font.pixelSize: Theme.fontCaption
    }
}
