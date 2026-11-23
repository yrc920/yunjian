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

    // 三层纸：桌面只在认证页与空态露出，信纸铺满工作区，面板是压在信纸上的卡纸。
    readonly property color desk: dark ? "#0B1015" : "#EFE9DD"
    readonly property color sheet: dark ? "#121A21" : "#FBF8F1"
    readonly property color panelBg: dark ? "#18212A" : "#FFFDF8"
    readonly property color panelAlt: dark ? "#1E2933" : "#F5F0E6"
    readonly property color indexBg: dark ? "#161F28" : "#F5F0E6"
    readonly property color selected: dark ? "#24313D" : "#EDE4D3"
    readonly property color line: dark ? "#2A3540" : "#E3D8C6"
    readonly property color lineSoft: dark ? "#202A34" : "#EFE8DB"
    readonly property color surfaceFloating: panelBg

    // 墨：正文、次要、提示；提示色在信纸上仍有 4.5:1
    readonly property color textPrimary: dark ? "#E9EEF3" : "#1B2A38"
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
    readonly property color spine: dark ? "#0A1218" : "#1E3B56"
    readonly property color spineText: dark ? "#E9EEF3" : "#F7F3E9"
    readonly property color spineMuted: dark ? "#8FA0AF" : "#B7C6D4"
    // 书脊上的悬停与选中：只提亮，不铺一整块纸色，避免顶部入口看起来像应用图标
    readonly property color spineHover: Qt.rgba(spineText.r, spineText.g, spineText.b, 0.06)
    readonly property color spineActive: Qt.rgba(spineText.r, spineText.g, spineText.b, 0.10)

    // 强调色：primary 用于主操作，accent 只做少量标记，danger 用于失败
    readonly property color primary: dark ? "#8FB3D4" : "#24476B"
    readonly property color primaryHover: dark ? "#A6C6E3" : "#2E567E"
    readonly property color primaryPressed: dark ? "#6E97B8" : "#1A3550"
    readonly property color primaryText: dark ? "#0E1A24" : "#F7F3E9"
    readonly property color primarySoft: dark ? "#22384C" : "#E8EFF6"

    readonly property color bubbleSelf: dark ? "#2A4A6A" : "#24476B"
    readonly property color bubbleSelfText: dark ? "#E9EEF3" : "#F7F3E9"
    readonly property color bubbleOther: dark ? "#18212A" : "#FFFDF8"
    readonly property color fieldBg: dark ? "#141C24" : "#FFFDF8"
    readonly property color fieldBorder: dark ? "#33404C" : "#DCD1BF"
    readonly property color overlay: dark ? "#A60A0E12" : "#5E141C26"
    // 信纸横线：只比纸深一点，是质感不是表格
    readonly property color ruleLine: dark ? Qt.rgba(0.91, 0.93, 0.95, 0.05)
                                          : Qt.rgba(0.11, 0.16, 0.22, 0.045)
    // 信纸与蜡印：吉祥物、信封表情与应用图标共用同一组颜色
    readonly property color paper: dark ? "#E6E1D2" : "#FBF8EF"
    readonly property color paperFold: dark ? "#D6CDB4" : "#EFE8D6"
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
    readonly property int sidebarWidth: 70
    readonly property int compactIndexWidth: 60
    readonly property int indexWidth: 288
    readonly property color focusRing: dark ? "#89B9E8" : "#2E6EA1"
}
