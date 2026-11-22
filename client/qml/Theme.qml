pragma Singleton
import QtQuick
import QtCore

// 墨蓝是钢笔墨水，纸白是信纸，火漆红只做印记：界面里唯一的暖色。
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

    // 基础界面色：页面、面板、悬停和选中只拉开一小级明度差。
    readonly property color bgTop: dark ? "#0F151B" : "#F5F7F9"
    readonly property color panelBg: dark ? "#161E26" : "#FFFFFF"
    readonly property color panelAlt: dark ? "#1D2731" : "#EEF2F6"
    readonly property color chatBg: dark ? "#10171E" : "#F7F9FB"
    readonly property color sidebarBg: dark ? "#131A21" : "#EEF2F6"
    readonly property color selected: dark ? "#22384C" : "#E5EEF7"
    readonly property color line: dark ? "#2A3540" : "#DFE5EB"
    readonly property color lineSoft: dark ? "#202B35" : "#EEF1F4"
    readonly property color surfaceRaised: dark ? "#1B252E" : "#FFFFFF"
    readonly property color surfaceFloating: dark ? "#202C37" : "#FFFFFF"

    readonly property color textPrimary: dark ? "#E9EFF5" : "#111A24"
    readonly property color textSecondary: dark ? "#A7B4C1" : "#55636F"
    readonly property color textHint: dark ? "#82909D" : "#71808C"
    readonly property color ink: dark ? "#BBD0E2" : "#24476B"
    readonly property color danger: dark ? "#E0806A" : "#B4453A"
    readonly property color success: dark ? "#8FD0AE" : "#2E7D5B"
    readonly property color accent: dark ? "#E0806A" : "#B44A34"
    readonly property color stamp: accent

    // 强调色：primary 用于主操作，accent 只做少量标记，danger 用于失败
    readonly property color primary: dark ? "#7FA9CF" : "#24476B"
    readonly property color primaryHover: dark ? "#93B9DA" : "#2E5680"
    readonly property color primaryPressed: dark ? "#6C97BE" : "#1A3550"
    readonly property color primaryText: dark ? "#0E1A24" : "#F7FAFC"
    readonly property color primarySoft: dark ? "#22384C" : "#E7EFF7"

    readonly property color bubbleSelf: dark ? "#22384C" : "#E7EFF7"
    readonly property color bubbleSelfText: textPrimary
    readonly property color bubbleOther: dark ? "#1E2832" : "#FFFFFF"
    readonly property color fieldBg: dark ? "#171F27" : "#FFFFFF"
    readonly property color fieldBorder: dark ? "#34414D" : "#D7DFE7"
    readonly property color overlay: dark ? "#A60A0E12" : "#5E0E1620"
    // 信纸与蜡印：吉祥物、信封表情与应用图标共用同一组颜色
    readonly property color paper: dark ? "#E6E1D2" : "#FBF8EF"
    readonly property color paperFold: dark ? "#D6CDB4" : "#EFE8D6"
    readonly property color paperInk: "#1E3A57"

    // 字号与控件尺寸，页面不再各写像素值
    readonly property int fontHero: 24
    readonly property int fontDisplay: 22
    readonly property int fontBody: 14
    readonly property int fontSmall: 12
    readonly property int fontCaption: 11
    readonly property int radius: 10
    readonly property int controlRadius: 8
    readonly property int panelRadius: 12
    readonly property int dialogRadius: 14
    readonly property int pillRadius: 999
    readonly property real bubbleMaxWidth: 460
    readonly property int fieldHeight: 48
    readonly property int buttonHeight: 44
    readonly property int toolbarHeight: 64
    readonly property int listRowHeight: 64
    readonly property int sidebarWidth: 64
    readonly property int indexWidth: 280
    readonly property int contentInset: 24
    readonly property int sectionGap: 24
    readonly property color focusRing: dark ? "#89B9E8" : "#2E6EA1"
}
