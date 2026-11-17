#ifndef TRAYCONTROLLER_H
#define TRAYCONTROLLER_H

#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

class QMenu;
class QSystemTrayIcon;

/**
 * @brief 系统托盘：QtWidgets 只作为平台集成层，UI 仍然是 QML/Quick
 *
 * QSystemTrayIcon/QMenu 负责原生托盘与菜单；不创建任何 QWidget 页面。
 * 没有系统托盘时 available 为 false，外壳自动退回普通关窗行为。
 */
class TrayController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(QString tooltip READ tooltip WRITE setTooltip NOTIFY tooltipChanged)

public:
    explicit TrayController(QObject *parent = nullptr);
    ~TrayController() override;

    bool available() const { return _tray != nullptr; }
    QString tooltip() const { return _tooltip; }
    void setTooltip(const QString &tooltip);

    //没有托盘时静默失败；调用方仍可更新标题/角标
    Q_INVOKABLE void showMessage(const QString &title, const QString &body, int msecs = 6000);
    Q_INVOKABLE void hide();

signals:
    //托盘菜单/左键：把主窗口带回来
    void openRequested();
    //托盘菜单退出：先隐藏托盘再真正退出
    void quitRequested();
    //点击系统通知
    void messageClicked();
    void tooltipChanged();

private:
    QSystemTrayIcon *_tray = nullptr;
    QMenu *_menu = nullptr;
    QString _tooltip;
};

#endif // TRAYCONTROLLER_H
