#include "friendservice.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>

#include "tcpmgr.h"
#include "usermgr.h"
#include "userdata.h"

FriendService::FriendService()
{
    //搜索没有回包时也要解锁，否则下一次搜索会被 _searching 永久挡住
    _searchTimer.setSingleShot(true);
    _searchTimer.setInterval(8000);
    connect(&_searchTimer, &QTimer::timeout, this, [this] {
        if (!_searching)
            return;
        _searching = false;
        emit searchFailed(QStringLiteral("搜索超时，请检查网络后重试"));
    });

    // 申请同样要能超时退出，否则浮层会一直显示「发送中」
    _applyTimer.setSingleShot(true);
    _applyTimer.setInterval(10000);
    connect(&_applyTimer, &QTimer::timeout, this, [this] {
        if (_applying)
            finishApply(false, QStringLiteral("发送超时，请稍后到「新的朋友」确认"));
    });

    //搜索结果由 TcpMgr 解析成 SearchInfo，这里只负责转成界面能用的字段
    connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_user_search, this,
            [this](std::shared_ptr<SearchInfo> info) {
                if (!_searching)
                    return;
                _searching = false;
                _searchTimer.stop();
                if (!info || info->_uid <= 0) {
                    emit searchFailed(QStringLiteral("没有找到这个用户"));
                    return;
                }
                emit userFound(info->_uid, info->_name, info->_nick,
                               info->_desc, info->_icon, info->_sex);
            });
    connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_connection_closed,
            this, [this] {
                if (!_searching)
                    return;
                _searching = false;
                _searchTimer.stop();
                emit searchFailed(QStringLiteral("连接已断开，请重新登录后再试"));
            });

    // 申请回执：0 表示服务端已写入申请记录，其它错误码统一按失败提示
    connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_add_friend_rsp, this,
            [this](const QJsonObject &data) {
                if (!_applying)
                    return;
                const int error = data.value("error").toInt(ErrorCodes::ERR_JSON);
                finishApply(error == ErrorCodes::SUCCESS, error == ErrorCodes::SUCCESS
                    ? QString() : QStringLiteral("申请没有发出去，请稍后再试"));
            });
    connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_connection_closed, this, [this] {
        if (_applying)
            finishApply(false, QStringLiteral("连接已断开，申请没有发出"));
    });
}

// 统一结束一次申请：停表、放开重试，并把结果交给界面
void FriendService::finishApply(bool ok, const QString &message)
{
    _applying = false;
    _applyTimer.stop();
    emit applyFinished(ok, message);
}

FriendService::~FriendService()
{
}

// 搜索用户：纯数字按 uid 查，其它按用户名查
bool FriendService::searchUser(const QString &query)
{
    const QString text = query.trimmed();
    if (text.isEmpty() || _searching)
        return false;
    _searching = true;
    _searchTimer.start();

    QJsonObject body;
    body["uid"] = text; // 服务端按内容判断：纯数字当 uid，否则当用户名

    emit TcpMgr::GetInstance()->sig_send_data(
        ReqId::ID_SEARCH_USER_REQ,
        QJsonDocument(body).toJson(QJsonDocument::Compact));
    return true;
}

// 发起好友申请：applyname 是申请留言（默认欢迎语），bakname 是给对方设的备注
bool FriendService::applyFriend(int touid, const QString &applyName,
                                const QString &backName)
{
    if (touid <= 0 || _applying)
        return false;
    _applying = true;
    _applyTimer.start();

    QJsonObject body;
    body["uid"] = UserMgr::GetInstance()->GetUid();
    body["applyname"] = applyName;
    body["bakname"] = backName;
    body["touid"] = touid;

    emit TcpMgr::GetInstance()->sig_send_data(
        ReqId::ID_ADD_FRIEND_REQ,
        QJsonDocument(body).toJson(QJsonDocument::Compact));
    return true;
}

// 同意对方的申请：back 是给对方设置的备注名
void FriendService::authFriend(int touid, const QString &backName)
{
    const int myUid = UserMgr::GetInstance()->GetUid();

    QJsonObject body;
    body["fromuid"] = myUid;
    body["touid"] = touid;
    body["back"] = backName;

    emit TcpMgr::GetInstance()->sig_send_data(
        ReqId::ID_AUTH_FRIEND_REQ,
        QJsonDocument(body).toJson(QJsonDocument::Compact));
}
