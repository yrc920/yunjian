#include "traycontroller.h"

#include <QAction>
#include <QApplication>
#include <QIcon>
#include <QMenu>
#include <QSystemTrayIcon>

TrayController::TrayController(QObject *parent) : QObject(parent)
{
    if (!QSystemTrayIcon::isSystemTrayAvailable())
        return;

    _tray = new QSystemTrayIcon(this);
    QIcon icon = QIcon(QStringLiteral(":/res/yunjian.png"));
    if (icon.isNull())
        icon = QApplication::windowIcon();
    _tray->setIcon(icon);
    _tooltip = QStringLiteral("yunjian");
    _tray->setToolTip(_tooltip);

    _menu = new QMenu();
    QAction *open = _menu->addAction(tr("打开 yunjian"));
    connect(open, &QAction::triggered, this, &TrayController::openRequested);
    _menu->addSeparator();
    QAction *quit = _menu->addAction(tr("退出"));
    connect(quit, &QAction::triggered, this, &TrayController::quitRequested);
    _tray->setContextMenu(_menu);

    connect(_tray, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                //右键只弹菜单；左键/双击回窗口
                if (reason == QSystemTrayIcon::Trigger
                    || reason == QSystemTrayIcon::DoubleClick)
                    emit openRequested();
            });
    connect(_tray, &QSystemTrayIcon::messageClicked,
            this, &TrayController::messageClicked);
    _tray->show();
}

TrayController::~TrayController()
{
    if (_tray) {
        _tray->setContextMenu(nullptr);
        _tray->hide();
    }
    delete _menu;
    _menu = nullptr;
}

void TrayController::setTooltip(const QString &tooltip)
{
    const QString next = tooltip.isEmpty() ? QStringLiteral("yunjian") : tooltip;
    if (_tooltip == next)
        return;
    _tooltip = next;
    if (_tray)
        _tray->setToolTip(_tooltip);
    emit tooltipChanged();
}

void TrayController::showMessage(const QString &title, const QString &body, int msecs)
{
    if (!_tray || !_tray->isVisible())
        return;
    _tray->showMessage(title, body, QSystemTrayIcon::Information, msecs);
}

void TrayController::hide()
{
    if (_tray)
        _tray->hide();
}
