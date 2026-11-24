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
    // 必须显式给 Aqua：nil 的含义是“跟随系统”，系统深色时会把浅色应用画成深灰标题栏
    nsWindow.appearance = dark ? [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua]
                               : [NSAppearance appearanceNamed:NSAppearanceNameAqua];
}
