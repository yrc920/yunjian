#ifndef WINDOWCHROME_H
#define WINDOWCHROME_H

#include <QObject>
#include <QWindow>
#include <QtQml/qqmlregistration.h>

/******************************************************************************
 * @file       windowchrome.h
 * @brief      窗口装饰：让系统标题栏与界面外观一致
 *
 * macOS 上隐藏标题栏文字、让标题栏跟随应用的深浅色；内容仍排在标题栏下方，
 * 拖拽与红绿灯保持系统默认行为。Windows 保持原生标题栏，这里不做任何事。
 *****************************************************************************/

class WindowChrome : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit WindowChrome(QObject *parent = nullptr);

    Q_INVOKABLE void apply(QWindow *window, bool dark);
};

#endif // WINDOWCHROME_H
