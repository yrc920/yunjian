#include "groupservice.h"

#include <QDateTime>
#include <QTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

#include "chatservice.h"
#include "tcpmgr.h"
#include "usermgr.h"

namespace {
//服务端下发的一份群消息：系统消息与普通文本共用字段名，这里只挑系统消息需要的
std::shared_ptr<TextChatData> buildSystemMessage(qint64 thread_id, const QJsonObject &item)
{
    const qint64 messageId = item.value("message_id").toVariant().toLongLong();
    if (messageId <= 0)
        return {};
    auto message = std::make_shared<TextChatData>(item.value("msgid").toString(),
        item.value("content").toString(), item.value("fromuid").toInt(), 0, messageId,
        thread_id, ChatMessageStatus::Sent);
    message->_msg_type = ChatMsgType::System;
    message->_time = item.value("time").toVariant().toLongLong();
    return message;
}
}

GroupService::GroupService()
{
    auto *tcp = TcpMgr::GetInstance().get();

    connect(tcp, &TcpMgr::sig_group_created, this, [this](const QJsonObject &data) {
        _creating = false;
        const int error = data.value("error").toInt(1);
        const qint64 threadId = data.value("thread_id").toVariant().toLongLong();
        if (error != 0) {
            emit createFinished(false, 0, data.value("message").toString(
                QStringLiteral("建群失败")));
            return;
        }

        //建群回包已经带完整资料与成员，直接落库，不再多拉一次 1062
        const QJsonObject group = data.value("group").toObject();
        QJsonObject thread;
        thread["thread_id"] = QJsonValue::fromVariant(threadId);
        thread["thread_type"] = static_cast<int>(ChatThreadKind::Group);
        thread["group_name"] = group.value("name").toString();
        thread["owner"] = group.value("owner").toInt();
        thread["member_count"] = group.value("member_count").toInt();
        thread["my_role"] = group.value("my_role").toInt();
        thread["muted"] = group.value("muted").toBool();
        thread["time"] = static_cast<double>(QDateTime::currentSecsSinceEpoch());
        auto *mgr = UserMgr::GetInstance().get();
        mgr->UpsertGroupThread(thread);
        mgr->ApplyGroupProfile(group);
        mgr->SetGroupMembers(threadId, data.value("members").toArray());
        emit createFinished(true, threadId, QString());
        emit groupActivated(threadId);
    });

    connect(tcp, &TcpMgr::sig_group_info, this, [this](const QJsonObject &data) {
        const qint64 threadId = data.value("thread_id").toVariant().toLongLong();
        _pending_info.remove(threadId);
        if (data.value("error").toInt(1) != 0) {
            emit infoLoaded(false, threadId, QStringLiteral("群资料加载失败"));
            return;
        }
        auto *mgr = UserMgr::GetInstance().get();
        const bool known = mgr->GetGroup(threadId) != nullptr;
        mgr->ApplyGroupProfile(data.value("group").toObject());
        mgr->SetGroupMembers(threadId, data.value("members").toArray());
        emit infoLoaded(true, threadId, QString());
        //第一次拿到这个群（被邀请后补拉）：让会话层把历史与首条系统消息拉回来
        if (!known)
            emit groupActivated(threadId);
    });

    connect(tcp, &TcpMgr::sig_group_updated, this, [this](const QJsonObject &data) {
        const qint64 threadId = data.value("thread_id").toVariant().toLongLong();
        const QString action = data.value("action").toString();
        if (data.value("error").toInt(1) != 0) {
            emit updateFinished(false, threadId, action,
                data.value("message").toString(QStringLiteral("修改失败")));
            return;
        }
        auto *mgr = UserMgr::GetInstance().get();
        auto group = mgr->GetGroup(threadId);
        if (group) {
            QJsonObject patch;
            patch["thread_id"] = QJsonValue::fromVariant(threadId);
            const QString value = data.value("value").toString();
            if (action == "name")
                patch["name"] = value;
            else if (action == "notice")
                patch["notice"] = value;
            else if (action == "nickname")
                patch["my_nick"] = value;
            mgr->ApplyGroupProfile(patch);
        }
        if (action == "mute")
            mgr->SetGroupMuted(threadId, data.value("value").toString() == "1");
        emit updateFinished(true, threadId, action, QString());
    });

    connect(tcp, &TcpMgr::sig_group_member_op, this, [this](const QJsonObject &data) {
        const qint64 threadId = data.value("thread_id").toVariant().toLongLong();
        const QString action = data.value("action").toString();
        if (data.value("error").toInt(1) != 0) {
            emit memberOpFinished(false, threadId, action,
                data.value("message").toString(QStringLiteral("操作失败")));
            return;
        }
        auto *mgr = UserMgr::GetInstance().get();
        if (data.contains("member_count"))
            mgr->SetGroupMemberCount(threadId, data.value("member_count").toInt());
        //成员名单以服务端为准：邀请、移除、设管理员后统一重拉一次
        if (action == "invite" || action == "remove" || action == "set_admin"
            || action == "unset_admin")
            RequestGroupInfo(threadId);
        emit memberOpFinished(true, threadId, action, data.value("message").toString());
    });

    connect(tcp, &TcpMgr::sig_group_changed, this, [this](const QJsonObject &data) {
        applyGroupChanged(data);
    });
}

GroupService::~GroupService() = default;

void GroupService::CreateGroup(const QString &name, const QList<int> &members)
{
    if (_creating)
        return;
    _creating = true;
    const int token = ++_createToken;
    //服务端不回包时不能让按钮一直转：超时后允许重试，回包到达时 token 已过期
    QTimer::singleShot(12000, this, [this, token] {
        if (!_creating || token != _createToken)
            return;
        _creating = false;
        emit createFinished(false, 0, QStringLiteral("建群超时，请重试"));
    });
    QJsonArray list;
    for (int uid : members)
        list.append(uid);
    QJsonObject frame{{"fromuid", UserMgr::GetInstance()->GetUid()},
                      {"name", name}, {"members", list}};
    emit TcpMgr::GetInstance()->sig_send_data(ReqId::ID_CREATE_GROUP_REQ,
        QJsonDocument(frame).toJson(QJsonDocument::Compact));
}

void GroupService::RequestGroupInfo(qint64 thread_id)
{
    if (thread_id <= 0 || _pending_info.contains(thread_id))
        return;
    _pending_info.insert(thread_id);
    QTimer::singleShot(10000, this, [this, thread_id] {
        if (!_pending_info.remove(thread_id))
            return;
        emit infoLoaded(false, thread_id, QStringLiteral("群资料加载超时"));
    });
    QJsonObject frame{{"fromuid", UserMgr::GetInstance()->GetUid()},
                      {"thread_id", thread_id}};
    emit TcpMgr::GetInstance()->sig_send_data(ReqId::ID_GET_GROUP_INFO_REQ,
        QJsonDocument(frame).toJson(QJsonDocument::Compact));
}

void GroupService::UpdateGroup(qint64 thread_id, const QString &action, const QString &value)
{
    if (thread_id <= 0)
        return;
    QJsonObject frame{{"fromuid", UserMgr::GetInstance()->GetUid()},
                      {"thread_id", thread_id}, {"action", action}, {"value", value}};
    emit TcpMgr::GetInstance()->sig_send_data(ReqId::ID_UPDATE_GROUP_REQ,
        QJsonDocument(frame).toJson(QJsonDocument::Compact));
}

void GroupService::InviteMembers(qint64 thread_id, const QList<int> &members)
{
    if (thread_id <= 0 || members.isEmpty())
        return;
    QJsonArray list;
    for (int uid : members)
        list.append(uid);
    QJsonObject frame{{"fromuid", UserMgr::GetInstance()->GetUid()},
                      {"thread_id", thread_id}, {"action", "invite"}, {"members", list}};
    emit TcpMgr::GetInstance()->sig_send_data(ReqId::ID_GROUP_MEMBER_OP_REQ,
        QJsonDocument(frame).toJson(QJsonDocument::Compact));
}

void GroupService::RemoveMember(qint64 thread_id, int target)
{
    QJsonObject frame{{"fromuid", UserMgr::GetInstance()->GetUid()},
                      {"thread_id", thread_id}, {"action", "remove"}, {"target", target}};
    emit TcpMgr::GetInstance()->sig_send_data(ReqId::ID_GROUP_MEMBER_OP_REQ,
        QJsonDocument(frame).toJson(QJsonDocument::Compact));
}

void GroupService::SetAdmin(qint64 thread_id, int target, bool admin)
{
    QJsonObject frame{{"fromuid", UserMgr::GetInstance()->GetUid()},
                      {"thread_id", thread_id},
                      {"action", admin ? "set_admin" : "unset_admin"}, {"target", target}};
    emit TcpMgr::GetInstance()->sig_send_data(ReqId::ID_GROUP_MEMBER_OP_REQ,
        QJsonDocument(frame).toJson(QJsonDocument::Compact));
}

void GroupService::LeaveGroup(qint64 thread_id)
{
    QJsonObject frame{{"fromuid", UserMgr::GetInstance()->GetUid()},
                      {"thread_id", thread_id}, {"action", "leave"}};
    emit TcpMgr::GetInstance()->sig_send_data(ReqId::ID_GROUP_MEMBER_OP_REQ,
        QJsonDocument(frame).toJson(QJsonDocument::Compact));
}

void GroupService::DismissGroup(qint64 thread_id)
{
    QJsonObject frame{{"fromuid", UserMgr::GetInstance()->GetUid()},
                      {"thread_id", thread_id}, {"action", "dismiss"}};
    emit TcpMgr::GetInstance()->sig_send_data(ReqId::ID_GROUP_MEMBER_OP_REQ,
        QJsonDocument(frame).toJson(QJsonDocument::Compact));
}

//1068 的 event 决定处理方式：
//created 新群 / info 改名称公告 / members 人数与角色 / removed 被移出 / dismissed 解散 / message 系统消息
void GroupService::applyGroupChanged(const QJsonObject &data)
{
    const qint64 threadId = data.value("thread_id").toVariant().toLongLong();
    const QString event = data.value("event").toString();
    if (threadId <= 0 || event.isEmpty())
        return;
    auto *mgr = UserMgr::GetInstance().get();

    if (event == "created") {
        const QJsonObject group = data.value("group").toObject();
        QJsonObject thread;
        thread["thread_id"] = QJsonValue::fromVariant(threadId);
        thread["thread_type"] = static_cast<int>(ChatThreadKind::Group);
        thread["group_name"] = group.value("name").toString();
        thread["owner"] = group.value("owner").toInt();
        thread["member_count"] = group.value("member_count").toInt();
        thread["my_role"] = group.value("my_role").toInt();
        thread["time"] = static_cast<double>(QDateTime::currentSecsSinceEpoch());
        mgr->UpsertGroupThread(thread);
        mgr->ApplyGroupProfile(group);
        mgr->SetGroupMembers(threadId, data.value("members").toArray());
        emit groupActivated(threadId);
        return;
    }

    if (event == "info") {
        QJsonObject patch;
        patch["thread_id"] = QJsonValue::fromVariant(threadId);
        if (data.contains("name"))
            patch["name"] = data.value("name").toString();
        if (data.contains("notice"))
            patch["notice"] = data.value("notice").toString();
        mgr->ApplyGroupProfile(patch);
        return;
    }

    if (event == "members") {
        //被邀请的人本地还没有这个群：先建占位再拉资料，避免系统消息没有容器可放
        const bool known = mgr->GetGroup(threadId) != nullptr;
        if (!known) {
            QJsonObject thread;
            thread["thread_id"] = QJsonValue::fromVariant(threadId);
            thread["thread_type"] = static_cast<int>(ChatThreadKind::Group);
            thread["group_name"] = QStringLiteral("群聊");
            mgr->UpsertGroupThread(thread);
        }
        if (data.contains("member_count"))
            mgr->SetGroupMemberCount(threadId, data.value("member_count").toInt());
        //新成员、角色变化、退群都在这条分支；未知群必须补拉，否则群名与成员表一直是空的
        if (!known || data.contains("target"))
            RequestGroupInfo(threadId);
        return;
    }

    //离开与解散都不删本地对象：历史要留着，由 ChatBridge 统一标记状态
    if (event == "removed") {
        emit groupRemoved(threadId, false);
        return;
    }

    if (event == "dismissed") {
        emit groupRemoved(threadId, true);
        return;
    }

    if (event == "message")
        appendSystemMessage(threadId, data.value("message").toObject());
}

//群系统消息：建群、邀请、移除、退群、改名都靠它留痕，居中显示不占气泡
void GroupService::appendSystemMessage(qint64 thread_id, const QJsonObject &message)
{
    auto data = buildSystemMessage(thread_id, message);
    if (!data)
        return;
    ConversationRef conv;
    conv._thread_id = thread_id;
    conv._kind = ChatThreadKind::Group;
    UserMgr::GetInstance()->AppendChatMsgs(conv, {data});
}
