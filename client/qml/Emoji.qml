pragma Singleton
import QtQuick

// 内置表情目录：形象沿用应用图标里的小信封，只有一套本地资源，不发网络请求。
// 消息里存的是 token 文本（如 [微笑]），气泡按 token 换成图片；对方版本没有
// 这个资源时看到的是原文，不会乱码。
QtObject {
    id: emoji

    // 行内表情尺寸：宽 22、高 18，正好是素材 122×100 的等比结果。
    // 气泡里用同尺寸覆盖层绘制，输入框按同尺寸插入图片，两处都不变形、看起来一样大
    readonly property int size: 22
    readonly property int height: 18

    // 顺序就是面板里的显示顺序，常用的在前
    readonly property var items: [
        { token: "[微笑]", name: "微笑", file: "smile" },
        { token: "[大笑]", name: "大笑", file: "laugh" },
        { token: "[眨眼]", name: "眨眼", file: "wink" },
        { token: "[喜欢]", name: "喜欢", file: "love" },
        { token: "[惊讶]", name: "惊讶", file: "surprise" },
        { token: "[难过]", name: "难过", file: "sad" },
        { token: "[生气]", name: "生气", file: "angry" },
        { token: "[好困]", name: "好困", file: "sleepy" },
        { token: "[疑问]", name: "疑问", file: "question" },
        { token: "[收到]", name: "收到", file: "gotit" }
    ]

    function assetUrl(file) {
        return "qrc:/res/emoji/" + file + ".png"
    }
}
