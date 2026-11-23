import QtQuick

// 本人、好友、群聊共用的头像入口：右下角相机角标常驻，上传进度画在外圈。
// 编辑中的遮罩不盖住头像本身，避免"为了换头像先看不见自己"。
Item {
    id: root
    property string source: ""
    property string name: ""
    property bool group: false
    property bool editable: true
    property bool uploading: false
    property int progress: 0
    property int size: 80
    signal editRequested()

    implicitWidth: size
    implicitHeight: size

    readonly property int ringInset: 1

    Avatar {
        id: picture
        anchors.fill: parent
        source: root.source
        name: root.name
        group: root.group
        textSize: Math.round(root.size * 0.30)
    }

    // 悬停/聚焦给一圈墨线，不压暗头像
    Rectangle {
        anchors.fill: parent
        radius: picture.radius
        color: "transparent"
        border.width: 2
        border.color: Theme.primary
        visible: root.editable && (area.containsMouse || root.activeFocus)
    }

    Canvas {
        id: ring
        anchors.fill: parent
        z: 3
        visible: root.uploading
        onPaint: {
            const c = getContext("2d")
            c.reset()
            const w = width
            const h = height
            const cx = w / 2
            const cy = h / 2
            const r = Math.max(1, Math.min(w, h) / 2 - root.ringInset)
            c.lineWidth = 2
            c.strokeStyle = Theme.line
            c.beginPath()
            c.arc(cx, cy, r, 0, Math.PI * 2)
            c.stroke()
            const p = Math.max(0, Math.min(100, root.progress)) / 100
            if (p <= 0)
                return
            c.strokeStyle = Theme.primary
            c.lineCap = "round"
            c.beginPath()
            c.arc(cx, cy, r, -Math.PI / 2, -Math.PI / 2 + Math.PI * 2 * p)
            c.stroke()
        }
        Connections {
            target: root
            function onProgressChanged() { ring.requestPaint() }
            function onUploadingChanged() { ring.requestPaint() }
        }
        Connections {
            target: Theme
            function onDarkChanged() { ring.requestPaint() }
        }
    }

    Rectangle {
        visible: root.editable
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.rightMargin: -3
        anchors.bottomMargin: -3
        width: Math.max(20, Math.round(root.size * 0.28))
        height: width
        radius: width / 2
        color: Theme.panelBg
        border.width: 1
        border.color: Theme.line

        AppIcon {
            anchors.centerIn: parent
            name: "camera"
            size: Math.round(parent.width * 0.52)
            color: Theme.primary
        }
    }

    MouseArea {
        id: area
        anchors.fill: parent
        enabled: root.editable
        hoverEnabled: true
        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked: {
            root.forceActiveFocus(Qt.MouseFocusReason)
            root.editRequested()
        }
    }

    activeFocusOnTab: root.editable
    Accessible.role: Accessible.Button
    Accessible.name: "更换头像"
    Accessible.onPressAction: if (root.editable) root.editRequested()
    Keys.onReturnPressed: if (root.editable) root.editRequested()
    Keys.onSpacePressed: if (root.editable) root.editRequested()

    AppToolTip {
        text: "更换头像"
        hovered: area.containsMouse && root.editable
    }
}
