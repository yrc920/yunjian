pragma Singleton
import QtQuick
import QtCore

// 纸、墨、印：纸是暖白信纸，墨是钢笔墨水，火漆红只做印记。
// 三层表面（桌面 / 信纸 / 面板）靠 1px 发丝线分层，不靠阴影；火漆面积不超过 5%。
// 外观三态（跟随系统 / 浅色 / 深色）保存在本机，手动选择沿用旧版的 dark 键。
QtObject {
    id: theme

    // 跟随系统时由主窗口写入 Qt.styleHints.colorScheme；Unknown 按浅色处理
    property bool systemDark: false
    property bool followSystem: true
    // 手动选择只在脱离跟随系统后生效
    property bool manualDark: false
    readonly property bool dark: followSystem ? systemDark : manualDark
    readonly property string mode: followSystem ? "system" : manualDark ? "dark" : "light"

    // 外观写入本机 Settings，切换后重启仍然保持
    property QtObject preferences: Settings {
        category: "Appearance"
        property alias followSystem: theme.followSystem
        property alias dark: theme.manualDark
    }

    // 手动切换：按当前实际外观翻到另一侧，并退出跟随系统。
    // 必须先取反再退出跟随：顺序反了会读到 manualDark 的旧值，系统深色时点了没反应
    function toggleMode() {
        manualDark = !dark
        followSystem = false
    }

    function setMode(value) {
        if (value === "system") {
            followSystem = true
            return
        }
        followSystem = false
        manualDark = value === "dark"
    }

    // 两套色板是唯一来源：真实界面与设置页的外观预览都读这里，避免预览和实际漂移。
    // 预览要同时拿到浅色与深色，所以不在这里按 dark 做三元，而是暴露两个对象。
    readonly property QtObject lightPalette: QtObject {
        readonly property color desk: "#EFE9DD"
        readonly property color sheet: "#FBF8F1"
        readonly property color panelBg: "#FFFDF8"
        readonly property color panelAlt: "#F5F0E6"
        readonly property color indexBg: "#F5F0E6"
        readonly property color selected: "#EDE4D3"
        readonly property color line: "#E3D8C6"
        readonly property color lineSoft: "#EFE8DB"
        readonly property color fieldBg: "#FFFDF8"
        readonly property color fieldBorder: "#DCD1BF"
        readonly property color spine: "#1E3B56"
        readonly property color bubbleSelf: "#24476B"
        readonly property color bubbleOther: "#FFFDF8"
        readonly property color primary: "#24476B"
        readonly property color paper: "#FBF8EF"
        readonly property color paperFold: "#EFE8D6"
        readonly property color textPrimary: "#1B2A38"
    }

    readonly property QtObject darkPalette: QtObject {
        // 夜纸：信纸比桌面亮，索引比信纸暗，面板最亮；次序与浅色一致
        readonly property color desk: "#0D1319"
        readonly property color sheet: "#171F27"
        readonly property color panelBg: "#1F2932"
        readonly property color panelAlt: "#26323C"
        readonly property color indexBg: "#121A21"
        readonly property color selected: "#2C3945"
        readonly property color line: "#2E3A45"
        readonly property color lineSoft: "#242E38"
        readonly property color fieldBg: "#1F2932"
        readonly property color fieldBorder: "#35414C"
        readonly property color spine: "#060A0F"
        readonly property color bubbleSelf: "#2A4A6A"
        readonly property color bubbleOther: "#1F2932"
        readonly property color primary: "#8FB3D4"
        readonly property color paper: "#D8D1C0"
        readonly property color paperFold: "#C4BBA6"
        readonly property color textPrimary: "#E9EEF3"
    }

    readonly property QtObject palette: dark ? darkPalette : lightPalette

    // 三层纸：桌面只在认证页与空态露出，信纸铺满工作区，面板是压在信纸上的卡纸。
    readonly property color desk: palette.desk
    readonly property color sheet: palette.sheet
    readonly property color panelBg: palette.panelBg
    readonly property color panelAlt: palette.panelAlt
    readonly property color indexBg: palette.indexBg
    readonly property color selected: palette.selected
    readonly property color line: palette.line
    readonly property color lineSoft: palette.lineSoft
    readonly property color surfaceFloating: panelBg

    // 墨：正文、次要、提示；提示色在信纸上仍有 4.5:1
    readonly property color textPrimary: palette.textPrimary
    readonly property color textSecondary: dark ? "#A6B2BD" : "#5A6875"
    readonly property color textHint: dark ? "#8794A0" : "#66717C"
    readonly property color ink: dark ? "#8FB3D4" : "#24476B"
    readonly property color danger: dark ? "#D97A5F" : "#A8412F"
    readonly property color success: dark ? "#8FD0AE" : "#3C6E54"
    readonly property color accent: dark ? "#D97A5F" : "#B0472F"
    readonly property color stamp: accent
    readonly property color seal: accent
    readonly property color onSeal: dark ? "#1A120E" : "#FFF8F2"

    // 书脊：一级导航是整块墨，图标与文字用纸色反白
    readonly property color spine: palette.spine
    readonly property color spineText: dark ? "#E9EEF3" : "#F7F3E9"
    readonly property color spineMuted: dark ? "#8FA0AF" : "#B7C6D4"
    // 书脊右缘：浅色靠明度差天然分离，深色需要一条 1px 棱线才不糊在索引上
    readonly property color spineEdge: dark ? "#1B2632" : "transparent"
    // 书脊上的悬停与选中：只提亮，不铺一整块纸色，避免顶部入口看起来像应用图标
    readonly property color spineHover: Qt.rgba(spineText.r, spineText.g, spineText.b, 0.06)
    readonly property color spineActive: Qt.rgba(spineText.r, spineText.g, spineText.b, 0.10)

    // 强调色：primary 用于主操作，accent 只做少量标记，danger 用于失败
    readonly property color primary: palette.primary
    readonly property color primaryHover: dark ? "#A6C6E3" : "#2E567E"
    readonly property color primaryPressed: dark ? "#6E97B8" : "#1A3550"
    readonly property color primaryText: dark ? "#0E1A24" : "#F7F3E9"
    readonly property color primarySoft: dark ? "#22384C" : "#E8EFF6"

    readonly property color bubbleSelf: palette.bubbleSelf
    readonly property color bubbleSelfText: dark ? "#E9EEF3" : "#F7F3E9"
    readonly property color bubbleOther: palette.bubbleOther
    readonly property color fieldBg: palette.fieldBg
    readonly property color fieldBorder: palette.fieldBorder
    readonly property color overlay: dark ? "#A60A0E12" : "#5E141C26"
    //媒体上的文字：始终是浅色，不跟随主题翻转
    readonly property color onMedia: "#FFFFFF"
    // 信纸横线：只比纸深一点，是质感不是表格
    readonly property color ruleLine: dark ? Qt.rgba(0.91, 0.93, 0.95, 0.05)
                                          : Qt.rgba(0.11, 0.16, 0.22, 0.045)
    // 悬停提示是反色小纸片：浅色用墨底纸字，深色用夜纸底亮字
    readonly property color tooltipBg: dark ? "#202B35" : "#1B2A38"
    readonly property color tooltipBorder: dark ? "#34414D" : "#2A3B4C"
    readonly property color tooltipText: dark ? "#E9EEF3" : "#F7F3E9"
    // 纸角色的投影：浅色用墨的透明，深色用纯黑，避免深色下墨色变亮
    readonly property color paperShadow: dark ? "#000000" : "#1B2A38"

    // 信纸与蜡印：吉祥物、信封表情与应用图标共用同一组颜色
    readonly property color paper: palette.paper
    readonly property color paperFold: palette.paperFold
    readonly property color paperInk: "#1E3A57"

    // 字号：一屏最多六档，页面不再各写像素值
    readonly property int fontHero: 26
    readonly property int fontDisplay: 20
    readonly property int fontTitle: 17
    readonly property int fontBody: 14
    readonly property int fontSmall: 12
    readonly property int fontCaption: 11

    readonly property int controlRadius: 8
    readonly property int panelRadius: 12
    readonly property int dialogRadius: 16
    readonly property real bubbleMaxWidth: 460
    readonly property int fieldHeight: 48
    readonly property int buttonHeight: 44
    readonly property int toolbarHeight: 64
    readonly property int listRowHeight: 64
    // 书脊 70 放 60 的带标签入口；窄窗时列表收成 60 的头像条，两者不共用同一个宽度
    readonly property int sidebarWidth: 64
    readonly property int compactIndexWidth: 72
    readonly property int indexWidth: 288
    readonly property color focusRing: dark ? "#89B9E8" : "#2E6EA1"
}
