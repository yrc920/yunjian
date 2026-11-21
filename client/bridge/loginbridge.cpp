#include "loginbridge.h"

#include <QDateTime>
#include <QRegularExpression>

#include "autologinstore.h"
#include "authservice.h"
#include "sessionservice.h"

LoginBridge::LoginBridge(QObject *parent) : PageBridge(parent)
{
    _remember = AutoLoginStore::Remembered();

    auto *auth = AuthService::GetInstance().get();
    auto *session = SessionService::GetInstance().get();

    connect(auth, &AuthService::loginSucceeded, this,
            [this](const ServerInfo &info, const QString &email) {
                // 先记下来，等长连接真正就绪再落盘：半途失败的会话不该被记住
                _session = info;
                _sessionEmail = email;
                setBusyText(tr("正在连接聊天服务…"));
                SessionService::GetInstance().get()->connectToChat(info);
            });

    connect(auth, &AuthService::loginFailed, this, [this](const QString &message) {
        setBusy(false);
        setError(message);
    });

    connect(session, &SessionService::connectFailed, this, [this](const QString &message) {
        if (_autoLogin) {
            _autoLogin = false;
            _session = {};
            _sessionEmail.clear();
            // 令牌过期、被注销、或服务端不可达：清掉记录，退回手工登录
            AutoLoginStore::Clear();
            setBusy(false);
            setError(tr("自动登录未成功，请重新输入密码"));
            emit autoLoginExpired();
            return;
        }
        setBusy(false);
        setError(message);
    });

    connect(session, &SessionService::sessionReady, this, [this] {
        // 会话真正就绪才算一次成功登录：刷新保存时间，令牌随使用滑动续期
        if (_remember && _session.Uid > 0 && !_sessionEmail.isEmpty()) {
            AutoLoginStore::Record record;
            record.Email = _sessionEmail;
            record.Info = _session;
            record.SavedAt = QDateTime::currentSecsSinceEpoch();
            AutoLoginStore::Save(record);
        } else if (!_remember) {
            AutoLoginStore::Clear();
        }
        _autoLogin = false;
        setBusy(false);
        setBusyText(QString());
    });
}

// 写入 Email，值有变化才发信号，避免 QML 绑定重复求值
void LoginBridge::setEmail(const QString &value)
{
    if (_email == value)
        return;
    _email = value;
    emit emailChanged();
    clearError();
}

// 写入 Password，值有变化才发信号，避免 QML 绑定重复求值
void LoginBridge::setPassword(const QString &value)
{
    if (_password == value)
        return;
    _password = value;
    emit passwordChanged();
    clearError();
}

// 校验通过后发起登录；请求期间按钮保持禁用，直到会话就绪或失败
void LoginBridge::login()
{
    if (_busy)
        return;

    clearError();
    if (!checkEmail() || !checkPassword())
        return;

    setBusy(true);
    setBusyText(tr("正在登录…"));
    AuthService::GetInstance()->login(_email, _password);
}

// 切换密码明文 / 密文显示
void LoginBridge::togglePasswordVisible()
{
    _passwordVisible = !_passwordVisible;
    emit passwordVisibleChanged();
}

// 记住登录：关掉时立刻清掉已保存的令牌，避免下次仍然自动进入
void LoginBridge::setRemember(bool value)
{
    if (_remember == value)
        return;

    _remember = value;
    AutoLoginStore::SetRemembered(value);
    if (!value)
        AutoLoginStore::Clear();
    emit rememberChanged();
}

// 启动时尝试用上次的令牌恢复会话：不经过 HTTP，也不需要密码
bool LoginBridge::tryAutoLogin()
{
    if (_busy)
        return false;

    AutoLoginStore::Record record;
    if (!AutoLoginStore::Load(record))
        return false;

    _email = record.Email;
    emit emailChanged();

    _session = record.Info;
    _sessionEmail = record.Email;
    _autoLogin = true;
    setBusy(true);
    setBusyText(tr("正在恢复上次登录…"));
    SessionService::GetInstance().get()->connectToChat(record.Info);
    return true;
}

// 清掉错误提示（用户重新输入时调用）
void LoginBridge::clearError()
{
    if (_errorText.isEmpty())
        return;
    _errorText.clear();
    emit errorTextChanged();
}

// 邮箱格式校验
bool LoginBridge::checkEmail()
{
    static const QRegularExpression regex(QStringLiteral(R"((\w+)(\.|_)?(\w*)@(\w+)(\.(\w+))+)"));
    if (!regex.match(_email).hasMatch()) {
        setError(tr("邮箱地址不正确"));
        return false;
    }
    return true;
}

// 密码长度与字符集校验
bool LoginBridge::checkPassword()
{
    if (_password.length() < 6 || _password.length() > 15) {
        setError(tr("密码长度应为 6~15"));
        return false;
    }

    static const QRegularExpression regex(QStringLiteral("^[a-zA-Z0-9!@#$%^&*.]{6,15}$"));
    if (!regex.match(_password).hasMatch()) {
        setError(tr("密码不能包含非法字符"));
        return false;
    }
    return true;
}

// 请求中标记：为真时按钮禁用并显示 busyText
void LoginBridge::setBusy(bool busy)
{
    if (_busy == busy)
        return;
    _busy = busy;
    emit busyChanged();
}

// 按钮上的过程文案（正在登录 / 正在连接聊天服务）
void LoginBridge::setBusyText(const QString &text)
{
    if (_busyText == text)
        return;
    _busyText = text;
    emit busyTextChanged();
}

// 错误文案，界面用淡入展示
void LoginBridge::setError(const QString &text)
{
    if (_errorText == text)
        return;
    _errorText = text;
    emit errorTextChanged();
}
