#include "windowchrome.h"

#include <QGuiApplication>
#include <QWindow>

#import <Cocoa/Cocoa.h>

// 只处理标题栏本身：不启用 FullSizeContentView，内容仍从标题栏下方开始，
// 因此拖拽、双击缩放与红绿灯位置都保持系统行为。
WindowChrome::WindowChrome(QObject *parent) : QObject(parent) {}

void WindowChrome::apply(QWindow *window, bool dark)
{
    if (!window)
        return;

    // 只有 cocoa 平台的 winId 才是 NSView*：离屏（offscreen）、minimal 等插件返回的是
    // 自增句柄，强转后给 Objective-C 发消息会直接段错误。日志里已见过 0x1 的崩溃
    if (QGuiApplication::platformName() != QStringLiteral("cocoa"))
        return;

    const WId handle = window->winId();
    if (handle == 0)
        return;

    NSView *view = (__bridge NSView *)(reinterpret_cast<void *>(handle));
    NSWindow *nsWindow = [view window];
    if (!nsWindow)
        return;

    nsWindow.titleVisibility = NSWindowTitleHidden;
    // 手动切到深色而系统仍是浅色时，标题栏也要跟着变深，避免白条压在深色窗口上
    nsWindow.appearance = dark ? [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua] : nil;
}
