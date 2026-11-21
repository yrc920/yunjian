#ifndef LOGINBRIDGE_H
#define LOGINBRIDGE_H

#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

#include "pagebridge.h"
#include "global.h"

/******************************************************************************
 * @file       loginbridge.h
 * @brief      登录页与 C++ 之间的唯一边界
 *
 * QML 只声明结构：绑定 email / password 属性，点按钮调 login()。
 * 校验、请求、错误文案全部在这里，界面不出现任何协议字段。
 *****************************************************************************/

class LoginBridge : public PageBridge
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString email READ email WRITE setEmail NOTIFY emailChanged)
    Q_PROPERTY(QString password READ password WRITE setPassword NOTIFY passwordChanged)
    Q_PROPERTY(bool passwordVisible READ passwordVisible NOTIFY passwordVisibleChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString busyText READ busyText NOTIFY busyTextChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY errorTextChanged)
    Q_PROPERTY(bool remember READ remember WRITE setRemember NOTIFY rememberChanged)

public:
    explicit LoginBridge(QObject *parent = nullptr);

    QString email() const { return _email; }
    void setEmail(const QString &value);

    QString password() const { return _password; }
    void setPassword(const QString &value);

    bool passwordVisible() const { return _passwordVisible; }
    bool busy() const { return _busy; }
    QString busyText() const { return _busyText; }
    QString errorText() const { return _errorText; }
    bool remember() const { return _remember; }
    void setRemember(bool value);

    // 提交登录：先做本地校验，再走 HTTP 登录 + 长连接登录
    Q_INVOKABLE void login();
    Q_INVOKABLE void togglePasswordVisible();
    Q_INVOKABLE void clearError();
    // 有可用凭据时直接用令牌恢复会话；返回 false 表示需要手工登录
    Q_INVOKABLE bool tryAutoLogin();

signals:
    void emailChanged();
    void passwordChanged();
    void passwordVisibleChanged();
    void busyChanged();
    void busyTextChanged();
    void errorTextChanged();
    void rememberChanged();
    // 自动登录失败：凭据已清除，界面应把焦点交给密码框
    void autoLoginExpired();

private:
    bool navigationBlocked() const override { return _busy; }
    bool checkEmail();
    bool checkPassword();
    void setBusy(bool busy);
    void setBusyText(const QString &text);
    void setError(const QString &text);

    QString _email;
    QString _password;
    QString _busyText;
    QString _errorText;
    bool _passwordVisible = false;
    bool _busy = false;
    bool _remember = true;   // 默认记住，用户可在登录页取消
    bool _autoLogin = false; // 当前这次忙状态是否由自动登录触发
    ServerInfo _session;     // 登录成功后待落盘的会话信息
    QString _sessionEmail;
};

#endif // LOGINBRIDGE_H
