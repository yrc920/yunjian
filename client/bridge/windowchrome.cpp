#include "windowchrome.h"

// Windows 保持原生标题栏：标题文字由系统绘制，深浅色由系统决定，这里无需处理。
#ifndef Q_OS_MACOS

WindowChrome::WindowChrome(QObject *parent) : QObject(parent) {}

void WindowChrome::apply(QWindow *, bool) {}

#endif // !Q_OS_MACOS
