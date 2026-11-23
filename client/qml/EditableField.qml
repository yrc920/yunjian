import QtQuick

// 资料页唯一的字段行：左侧标签、右侧值，铅笔只在悬停/聚焦时贴着值出现。
// 整行可点即编辑，但不铺整行选中色——鼠标扫过时只有铅笔浮现，内容保持稳定。
// 单项编辑在原地展开，Enter 保存、Esc 取消；失败留在原处，不弹整表。
// mode: text 单行 / multiline 多行 / choice 选项 / tags 标签。
Column {
    id: root

    property string label: ""
    property string value: ""
    property string placeholder: "未设置"
    property bool editable: true
    property string mode: "text"
    property var options: []
    property var choiceValue: 0
    property var tags: []
    property int maxLength: 40
    property int maxLines: 4
    property int labelWidth: 96
    property int valueSize: Theme.fontBody
    property int valueWeight: Font.Normal
    property bool showLabel: true
    property bool busy: false
    property string error: ""
    property string busyText: "保存中"

    signal committed(var value)
    signal tagsCommitted(var list)
    signal editStarted()
    signal editFinished()

    readonly property bool editing: editor.visible
    // 铅笔本身也要算进悬停：鼠标移到铅笔上时底层 MouseArea 会失去 containsMouse
    readonly property bool hovered: displayArea.containsMouse
        || pencilArea.containsMouse || tagPencilArea.containsMouse
    readonly property string shownText: value.length > 0 ? value : placeholder
    readonly property bool empty: value.length === 0

    width: parent ? parent.width : implicitWidth
    spacing: 0

    function begin() {
        if (!editable || editing)
            return
        draft = value
        draftTags = (tags || []).slice()
        choiceDraft = choiceValue
        localError = ""
        textInput.text = value
        textArea.text = value
        editor.visible = true
        editStarted()
        Qt.callLater(function() {
            if (mode === "multiline")
                textArea.forceActiveFocus()
            else if (mode === "text")
                textInput.forceActiveFocus()
        })
    }

    function finish() {
        if (!editing)
            return
        editor.visible = false
        localError = ""
        // 收起后不再保留行焦点，避免铅笔和底线一直留在这一行
        displayRow.focus = false
        editFinished()
    }

    function fail(message) {
        localError = message.length > 0 ? message : "没有保存，检查网络后重试"
    }

    function cancel() {
        if (!editing)
            return
        editor.visible = false
        localError = ""
        displayRow.focus = false
        editFinished()
    }

    function save() {
        if (busy)
            return
        if (mode === "tags")
            tagsCommitted(draftTags)
        else if (mode === "choice")
            committed(choiceDraft)
        else
            committed(draft)
    }

    property string draft: ""
    property var draftTags: []
    property var choiceDraft: 0
    property string localError: ""
    readonly property string errorText: localError.length > 0 ? localError : error
    readonly property bool hasError: errorText.length > 0 && !editing

    // 展示态
    Item {
        id: display
        width: parent.width
        height: root.editing ? 0 : displayRow.height + (root.hasError ? 20 : 0)
        visible: !root.editing

        Item {
            id: displayRow
            width: parent.width
            height: root.mode === "multiline"
                ? Math.max(56, valueText.implicitHeight + 28)
                : root.mode === "tags" ? Math.max(52, tagFlow.height + 22) : 50

            FontMetrics {
                id: valueFm
                font: valueText.font
            }
            FontMetrics {
                id: labelFm
                font: labelText.font
            }
            TextMetrics {
                id: valueMetrics
                font: valueText.font
                text: root.shownText
            }

            MouseArea {
                id: displayArea
                anchors.fill: parent
                enabled: root.editable
                hoverEnabled: true
                cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: {
                    displayRow.forceActiveFocus(Qt.MouseFocusReason)
                    root.begin()
                }
            }

            // 键盘聚焦只画一条底线，不做包围盒：整行描边会把资料页切成一格一格
            Rectangle {
                visible: displayRow.activeFocus
                x: root.showLabel ? root.labelWidth : 0
                width: displayRow.width - x
                height: 1
                y: displayRow.height - 1
                color: Theme.focusRing
                opacity: 0.55
            }

            Text {
                id: labelText
                x: 0
                y: root.mode === "tags"
                    ? Math.max(0, (displayRow.height - labelFm.height) / 2)
                    : valueText.y + valueFm.ascent - labelFm.ascent
                visible: root.showLabel
                width: root.labelWidth
                text: root.label
                textFormat: Text.PlainText
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSmall
                elide: Text.ElideRight
            }

            Text {
                id: valueText
                x: root.showLabel ? root.labelWidth : 0
                // 单行按行高垂直居中；多行保留 14 的上下呼吸，和标签第一行对齐
                y: root.mode === "multiline"
                    ? 14
                    : Math.max(0, (displayRow.height - valueFm.height) / 2)
                width: parent.width - x - (root.mode === "multiline" ? 4 : 30)
                visible: root.mode !== "tags"
                text: root.shownText
                textFormat: Text.PlainText
                color: root.empty ? Theme.textHint : Theme.textPrimary
                font.pixelSize: root.valueSize
                font.weight: root.valueWeight
                wrapMode: root.mode === "multiline" ? Text.WrapAtWordBoundaryOrAnywhere
                                                    : Text.NoWrap
                elide: root.mode === "multiline" ? Text.ElideNone : Text.ElideRight
                maximumLineCount: root.mode === "multiline" ? root.maxLines : 1
                lineHeight: 1.35
            }

            Flow {
                id: tagFlow
                x: root.showLabel ? root.labelWidth : 0
                y: Math.max(0, (displayRow.height - tagFlow.height) / 2)
                width: parent.width - x
                spacing: 6
                visible: root.mode === "tags"

                Repeater {
                    model: root.tags || []
                    delegate: Rectangle {
                        width: tagText.implicitWidth + 20
                        height: 26
                        radius: 6
                        color: Theme.selected
                        Text {
                            id: tagText
                            anchors.centerIn: parent
                            text: modelData
                            textFormat: Text.PlainText
                            color: Theme.textPrimary
                            font.pixelSize: Theme.fontCaption
                        }
                    }
                }

                Text {
                    visible: (root.tags || []).length === 0
                    height: 26
                    verticalAlignment: Text.AlignVCenter
                    text: root.placeholder
                    color: Theme.textHint
                    font.pixelSize: Theme.fontBody
                }

                // 标签的铅笔跟着最后一枚标签走，换行时自然落到下一行
                Item {
                    width: 24
                    height: 26
                    // 始终占位，只淡入淡出：悬停时标签不会因为铅笔出现而换行重排
                    visible: root.editable
                    opacity: root.hovered || displayRow.activeFocus ? 1 : 0
                    Behavior on opacity { NumberAnimation { duration: 120 } }
                    Rectangle {
                        anchors.fill: parent
                        radius: 6
                        color: "transparent"
                        AppIcon {
                            anchors.centerIn: parent
                            name: "edit"
                            size: 16
                            color: tagPencilArea.containsMouse ? Theme.primary : Theme.textSecondary
                        }
                        MouseArea {
                            id: tagPencilArea
                            anchors.fill: parent
                            anchors.margins: -4
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                displayRow.forceActiveFocus(Qt.MouseFocusReason)
                                root.begin()
                            }
                        }
                    }
                }
            }

            // 文本/选项的铅笔：贴在第一行文字的末尾，只随悬停或键盘聚焦浮现
            Item {
                id: pencilSlot
                width: 24
                height: 24
                visible: root.editable && root.mode !== "tags"
                    && (root.hovered || displayRow.activeFocus)
                x: Math.min(valueText.x + Math.min(valueMetrics.advanceWidth, valueText.width - 4) + 6,
                            displayRow.width - 28)
                // 居中到文字行的中线：图标比文字高时允许负偏移，不能夹到 0 让笔往下坠
                y: valueText.y + (valueFm.height - height) / 2

                Rectangle {
                    anchors.fill: parent
                    radius: 6
                    color: "transparent"

                    AppIcon {
                        anchors.centerIn: parent
                        name: "edit"
                        size: 16
                        color: pencilArea.containsMouse ? Theme.primary : Theme.textSecondary
                    }
                }

                MouseArea {
                    id: pencilArea
                    anchors.fill: parent
                    anchors.margins: -4
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        displayRow.forceActiveFocus(Qt.MouseFocusReason)
                        root.begin()
                    }
                }
            }

            activeFocusOnTab: root.editable
            Accessible.role: Accessible.Button
            Accessible.name: root.label.length > 0 ? root.label : root.value
            Accessible.onPressAction: root.begin()
            Keys.onReturnPressed: root.begin()
            Keys.onSpacePressed: root.begin()
        }

        Text {
            anchors.left: parent.left
            anchors.leftMargin: root.showLabel ? root.labelWidth : 0
            anchors.top: displayRow.bottom
            width: parent.width - anchors.leftMargin
            height: root.hasError ? 18 : 0
            visible: root.hasError
            text: root.errorText
            color: Theme.danger
            font.pixelSize: Theme.fontCaption
            elide: Text.ElideRight
        }
    }

    // 编辑态：标签仍在左列，输入框在右列，标题与内容保持同一条基线
    Item {
        id: editor
        width: parent.width
        height: visible ? editorColumn.height : 0
        visible: false

        Column {
            id: editorColumn
            width: parent.width
            spacing: 8

            Item {
                width: parent.width
                height: root.mode === "text" ? 40
                    : root.mode === "multiline" ? 96
                    : root.mode === "choice" ? 30
                    : tagEditor.implicitHeight

                Text {
                    id: editorLabel
                    x: 0
                    y: root.mode === "multiline" ? 10
                        : root.mode === "tags" ? 0
                        : Math.max(0, (parent.height - height) / 2)
                    visible: root.showLabel && root.label.length > 0
                    width: root.labelWidth
                    text: root.label
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSmall
                    elide: Text.ElideRight
                }

                Rectangle {
                    id: textBox
                    x: root.showLabel ? root.labelWidth : 0
                    width: parent.width - x
                    height: 40
                    visible: root.mode === "text"
                    radius: Theme.controlRadius
                    color: Theme.fieldBg
                    border.width: 1
                    border.color: textInput.activeFocus ? Theme.focusRing : Theme.fieldBorder

                    TextInput {
                        id: textInput
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.leftMargin: 11
                        anchors.rightMargin: 11
                        anchors.verticalCenter: parent.verticalCenter
                        height: 28
                        verticalAlignment: TextInput.AlignVCenter
                        font.pixelSize: Theme.fontBody
                        color: Theme.textPrimary
                        selectionColor: Theme.selected
                        selectedTextColor: Theme.textPrimary
                        maximumLength: root.maxLength
                        selectByMouse: true
                        clip: true
                        onTextEdited: root.draft = text
                        Keys.onReturnPressed: root.save()
                        Keys.onEnterPressed: root.save()
                        Keys.onEscapePressed: root.cancel()
                    }
                }

                Rectangle {
                    id: multiBox
                    x: root.showLabel ? root.labelWidth : 0
                    width: parent.width - x
                    height: 96
                    visible: root.mode === "multiline"
                    radius: Theme.controlRadius
                    color: Theme.fieldBg
                    border.width: 1
                    border.color: textArea.activeFocus ? Theme.focusRing : Theme.fieldBorder

                    Flickable {
                        anchors.fill: parent
                        anchors.margins: 10
                        contentWidth: width
                        contentHeight: textArea.contentHeight
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds

                        TextEdit {
                            id: textArea
                            width: parent.width
                            textFormat: TextEdit.PlainText
                            wrapMode: TextEdit.WrapAtWordBoundaryOrAnywhere
                            font.pixelSize: Theme.fontBody
                            color: Theme.textPrimary
                            selectionColor: Theme.selected
                            selectedTextColor: Theme.textPrimary
                            textMargin: 0
                            selectByMouse: true
                            onTextChanged: root.draft = text
                            Keys.onEscapePressed: root.cancel()
                        }
                    }
                }

                Flow {
                    x: root.showLabel ? root.labelWidth : 0
                    width: parent.width - x
                    spacing: 8
                    visible: root.mode === "choice"

                    Repeater {
                        model: root.options
                        delegate: Rectangle {
                            id: choiceChip
                            readonly property bool chosen: root.choiceDraft === modelData.value
                            width: choiceLabel.implicitWidth + 28
                            height: 30
                            radius: 6
                            color: chosen ? Theme.selected : Theme.panelAlt
                            border.width: 1
                            border.color: chosen ? Theme.primary : "transparent"

                            Text {
                                id: choiceLabel
                                anchors.centerIn: parent
                                text: modelData.label
                                color: choiceChip.chosen ? Theme.textPrimary : Theme.textSecondary
                                font.pixelSize: Theme.fontSmall
                            }

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    root.choiceDraft = modelData.value
                                    root.save()
                                }
                            }
                        }
                    }
                }

                TagEditor {
                    id: tagEditor
                    x: root.showLabel ? root.labelWidth : 0
                    width: parent.width - x
                    visible: root.mode === "tags"
                    tags: root.draftTags
                    onCommitted: (list) => root.draftTags = list
                }
            }

            Item {
                width: parent.width
                height: 30

                Row {
                    x: root.showLabel ? root.labelWidth : 0
                    height: parent.height
                    spacing: 18

                    Text {
                        height: 30
                        verticalAlignment: Text.AlignVCenter
                        text: root.busy ? root.busyText : "保存"
                        color: root.busy ? Theme.textHint : Theme.primary
                        font.pixelSize: Theme.fontSmall
                        font.weight: Font.Medium
                        MouseArea {
                            anchors.fill: parent
                            anchors.leftMargin: -8
                            anchors.rightMargin: -8
                            enabled: !root.busy
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.save()
                        }
                    }
                    Text {
                        height: 30
                        verticalAlignment: Text.AlignVCenter
                        text: "取消"
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSmall
                        MouseArea {
                            anchors.fill: parent
                            anchors.leftMargin: -8
                            anchors.rightMargin: -8
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.cancel()
                        }
                    }
                }
            }

            Text {
                x: root.showLabel ? root.labelWidth : 0
                width: parent.width - x
                visible: root.localError.length > 0
                text: root.localError
                color: Theme.danger
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.WordWrap
            }
        }
    }

    onVisibleChanged: if (!visible && editing)
        cancel()
    onTagsChanged: if (editing && mode === "tags")
        draftTags = (tags || []).slice()
}
