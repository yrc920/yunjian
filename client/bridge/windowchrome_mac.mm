#include "windowchrome.h"

#include <QWindow>

#import <Cocoa/Cocoa.h>

// 只处理标题栏本身：不启用 FullSizeContentView，内容仍从标题栏下方开始，
// 因此拖拽、双击缩放与红绿灯位置都保持系统行为。
WindowChrome::WindowChrome(QObject *parent) : QObject(parent) {}

void WindowChrome::apply(QWindow *window, bool dark)
{
    if (!window)
        return;

    NSView *view = (__bridge NSView *)(reinterpret_cast<void *>(window->winId()));
    NSWindow *nsWindow = [view window];
    if (!nsWindow)
        return;

    nsWindow.titleVisibility = NSWindowTitleHidden;
    // 手动切到深色而系统仍是浅色时，标题栏也要跟着变深，避免白条压在深色窗口上
    nsWindow.appearance = dark ? [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua] : nil;
}
