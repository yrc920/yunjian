#include "profileservice.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFileInfo>
#include <QDebug>

#include "tcpmgr.h"
#include "usermgr.h"
#include "userdata.h"
#include "fileservice.h"
#include "global.h"

namespace {
// 与服务端 ChatServer 的 ErrorCodes 对应，这里只区分对用户有意义的两种
constexpr int kErrorInvalid = 1001; //请求字段不合法，通常是长度超限
constexpr int kErrorUidInvalid = 1011; //目标用户无效或好友关系已不存在

// 服务端下发的标签数组转成去重后的字符串列表
QStringList toTagList(const QJsonArray &array)
{
    QStringList tags;
    for (const auto &value : array) {
        const QString tag = value.toString();
        if (!tag.isEmpty() && !tags.contains(tag))
            tags.append(tag);
    }
    return tags;
}
}

ProfileService::ProfileService()
{
    // 回包丢失时也要释放按钮，否则界面会一直停在保存中
    _profileTimer.setSingleShot(true);
    _profileTimer.setInterval(8000);
    connect(&_profileTimer, &QTimer::timeout, this, [this] {
        if (_updating)
            finishProfile(false, QStringLiteral("保存超时，请检查网络后重试"));
    });

    _remarkTimer.setSingleShot(true);
    _remarkTimer.setInterval(8000);
    connect(&_remarkTimer, &QTimer::timeout, this, [this] {
        if (_remarking)
            finishRemark(false, _pendingRemarkUid, _pendingRemark, _pendingTags,
                         QStringLiteral("保存超时，请检查网络后重试"));
    });

    connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_profile_updated, this,
            [this](const QJsonObject &data) {
                if (!_updating)
                    return;
                const int error = data.value("error").toInt(kErrorInvalid);
                if (error != ErrorCodes::SUCCESS) {
                    finishProfile(false, error == kErrorUidInvalid
                        ? QStringLiteral("账号状态异常，请重新登录后再试")
                        : QStringLiteral("资料没有保存成功，请稍后重试"));
                    return;
                }
                // 服务端回包带的是最终落库内容，本地只认这一份，避免界面显示未生效的草稿
                UserMgr::GetInstance()->UpdateMyProfile(data.value("nick").toString(),
                                                        data.value("sex").toInt(),
                                                        data.value("desc").toString());
                finishProfile(true, QString());
            });

    connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_friend_remark_updated, this,
            [this](const QJsonObject &data) {
                if (!_remarking)
                    return;
                const int uid = data.value("touid").toInt();
                const int error = data.value("error").toInt(kErrorInvalid);
                if (error != ErrorCodes::SUCCESS) {
                    finishRemark(false, uid, _pendingRemark, _pendingTags, error == kErrorUidInvalid
                        ? QStringLiteral("好友关系已失效，请重新登录")
                        : QStringLiteral("资料没有保存成功，请稍后重试"));
                    return;
                }
                const QString remark = data.value("remark").toString();
                const QStringList tags = toTagList(data.value("tags").toArray());
                UserMgr::GetInstance()->UpdateFriendRelation(uid, remark, tags);
                finishRemark(true, uid, remark, tags, QString());
            });

    // 拉取好友资料失败时保持本地缓存，不弹错误：这是后台刷新，用户没有主动触发
    // 1050 只覆盖文字资料，头像仍用本地已下载的路径，避免显示闪回文件名
    connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_profile_loaded, this,
            [this](const QJsonObject &data) {
                if (data.value("error").toInt(kErrorInvalid) != ErrorCodes::SUCCESS)
                    return;
                const int uid = data.value("uid").toInt();
                if (uid <= 0)
                    return;
                UserMgr::GetInstance()->UpdateFriendProfile(uid, data.value("nick").toString(),
                                                            data.value("desc").toString(),
                                                            data.value("sex").toInt(-1));
                emit profileLoaded(uid);
            });

    // 好友主动改资料：文字字段整体覆盖；头像文件名变了才重新下载
    connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_friend_profile_changed, this,
            [this](const QJsonObject &data) {
                if (data.value("error").toInt(kErrorInvalid) != ErrorCodes::SUCCESS)
                    return;
                const int uid = data.value("uid").toInt();
                if (uid <= 0)
                    return;

                auto *mgr = UserMgr::GetInstance().get();
                const auto info = mgr->GetFriendById(uid);
                // 本地 _icon 可能是 file:// 路径，比较时只取文件名
                const QString localName = info ? QFileInfo(info->_icon).fileName() : QString();
                const QString icon = data.value("icon").toString();

                mgr->UpdateFriendProfile(uid, data.value("nick").toString(),
                                         data.value("desc").toString(),
                                         data.value("sex").toInt(-1));
                if (!icon.isEmpty() && icon != localName)
                    FileService::GetInstance()->RefreshContactIcon(uid, icon);
                emit profileLoaded(uid);
            });

    // 断开连接时在途请求不可能再回来，立即结束等待
    connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_connection_closed, this, [this] {
        if (_updating)
            finishProfile(false, QStringLiteral("连接已断开，资料没有保存"));
        if (_remarking)
            finishRemark(false, _pendingRemarkUid, _pendingRemark, _pendingTags,
                         QStringLiteral("连接已断开，资料没有保存"));
    });
}

ProfileService::~ProfileService()
{
}

void ProfileService::finishProfile(bool ok, const QString &message)
{
    _updating = false;
    _profileTimer.stop();
    emit profileUpdated(ok, message);
}

void ProfileService::finishRemark(bool ok, int uid, const QString &remark,
                                  const QStringList &tags, const QString &message)
{
    _remarking = false;
    _remarkTimer.stop();
    _pendingTags.clear();
    emit remarkUpdated(ok, uid, remark, tags, message);
}

// 修改本人资料：请求里不带 uid，服务端按会话身份写入，客户端无法借此改别人的资料
bool ProfileService::updateProfile(const QString &nick, int sex, const QString &desc)
{
    if (_updating)
        return false;

    QJsonObject body;
    body["nick"] = nick;
    body["sex"] = sex;
    body["desc"] = desc;

    _updating = true;
    _profileTimer.start();
    emit TcpMgr::GetInstance()->sig_send_data(
        ReqId::ID_UPDATE_PROFILE_REQ,
        QJsonDocument(body).toJson(QJsonDocument::Compact));
    return true;
}

// 修改好友备注与标签：只影响自己这一侧的显示名与分组，对方不会收到通知
bool ProfileService::updateFriendRelation(int uid, const QString &remark, const QStringList &tags)
{
    if (uid <= 0 || _remarking)
        return false;

    QJsonObject body;
    body["touid"] = uid;
    body["remark"] = remark;
    QJsonArray tag_array;
    for (const auto &tag : tags)
        tag_array.append(tag);
    body["tags"] = tag_array;

    _remarking = true;
    _pendingRemarkUid = uid;
    _pendingRemark = remark;
    _pendingTags = tags;
    _remarkTimer.start();
    emit TcpMgr::GetInstance()->sig_send_data(
        ReqId::ID_UPDATE_FRIEND_REMARK_REQ,
        QJsonDocument(body).toJson(QJsonDocument::Compact));
    return true;
}

// 拉取好友最新资料：不设超时也不做去重，短时间内重复调用最多多一次轻量查询
void ProfileService::requestProfile(int uid)
{
    if (uid <= 0)
        return;
    QJsonObject body;
    body["touid"] = uid;
    emit TcpMgr::GetInstance()->sig_send_data(
        ReqId::ID_GET_PROFILE_REQ,
        QJsonDocument(body).toJson(QJsonDocument::Compact));
}
