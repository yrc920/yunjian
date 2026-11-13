#include "sessionservice.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>
#include <QSize>
#include <QUrl>

#include "chatservice.h"
#include "tcpmgr.h"
#include "usermgr.h"
#include "fileservice.h"

namespace {
constexpr int kSessionTimeoutMs = 12000; // 连接 + 登录帧回包的整体上限
}

SessionService::SessionService() : _ready(false)
{
    _timeout.setSingleShot(true);
    _timeout.setInterval(kSessionTimeoutMs);
    connect(&_timeout, &QTimer::timeout, this, [this] {
        failAndReset(QStringLiteral("连接聊天服务超时"));
    });

    //服务端按心跳判活，断掉就踢下线；它属于会话而不是某个页面，
    //所以放在这里，切页面不影响保活
    _heartbeat.setInterval(10000);
    connect(&_heartbeat, &QTimer::timeout, this, [this] {
        if (!_ready)
            return;
        // 服务端按 fromuid 记录会话活跃时间，空包会被记成 uid 0
        QJsonObject frame;
        frame["fromuid"] = _current.Uid;
        emit TcpMgr::GetInstance()->sig_send_data(
            ReqId::ID_HEART_BEAT_REQ,
            QJsonDocument(frame).toJson(QJsonDocument::Indented));
    });

    auto *tcp = TcpMgr::GetInstance().get();

    // 收到新消息：写进 UserMgr。好友模型只刷新那一行的摘要，
    // 消息模型在同一个好友下会增量插入，界面不需要重建
    connect(tcp, &TcpMgr::sig_text_chat_msg, this,
            [](std::shared_ptr<TextChatMsg> msg) {
                if (!msg)
                    return;
                const int peerUid = msg->_from_uid == UserMgr::GetInstance()->GetUid()
                    ? msg->_to_uid : msg->_from_uid;
                if (msg->_thread_id > 0)
                    UserMgr::GetInstance()->SetThreadId(peerUid, msg->_thread_id);
                UserMgr::GetInstance()->AppendFriendChatMsg(peerUid, msg->_chat_msgs);
            });
    connect(tcp, &TcpMgr::sig_chat_msg_failed, this, [](const QJsonObject &data) {
        auto *mgr = UserMgr::GetInstance().get();
        const int peerUid = data.value("touid").toInt();
        for (const auto &value : data.value("text_array").toArray())
            mgr->UpdateChatMessage(peerUid, value.toObject().value("msgid").toString(), 0,
                ChatMessageStatus::Failed);
    });
    //图片消息回执：补上 message_id/thread_id 后启动资源上传；失败则结束本地消息
    connect(tcp, &TcpMgr::sig_chat_img_rsp, this, [](const QJsonObject &data) {
        auto *mgr = UserMgr::GetInstance().get();
        const QString uniqueId = data.value("unique_id").toString();
        const int peerUid = data.value("touid").toInt();
        if (uniqueId.isEmpty() || peerUid <= 0)
            return;

        if (data.value("error").toInt(1) != 0) {
            ChatService::GetInstance()->discardPendingImage(uniqueId);
            mgr->UpdateChatMessage(peerUid, uniqueId, 0, ChatMessageStatus::Failed);
            return;
        }

        const qint64 threadId = data.value("thread_id").toVariant().toLongLong();
        if (threadId > 0)
            mgr->SetThreadId(peerUid, threadId);
        //消息已落库但资源还没上传；状态保持发送中，由资源上传进度推进到已发送
        const qint64 messageId = data.value("message_id").toVariant().toLongLong();
        mgr->UpdateChatMessage(peerUid, uniqueId, messageId, ChatMessageStatus::Sending);
        ChatService::GetInstance()->uploadPendingImage(uniqueId, messageId, peerUid, threadId);
    });
    //对方图片上传完成：补消息并下载资源，界面先在会话里显示占位和进度
    connect(tcp, &TcpMgr::sig_image_notify, this, [](const QJsonObject &data) {
        ChatService::GetInstance()->HandleImageNotify(data);
    });
    connect(tcp, &TcpMgr::sig_chat_threads, this, [this](const QJsonObject &data) {
        auto *mgr = UserMgr::GetInstance().get();
        const auto threads = data.value("thread_list").toArray();
        mgr->AppendChatThreads(threads);
        mgr->RefreshFriendList();
        //每个会话先拿第一页，剩下的由 load_more 继续
        for (const auto &value : threads) {
            const auto item = value.toObject();
            requestMessages(item.value("thread_id").toVariant().toLongLong());
        }
        if (data.value("load_more").toBool() && !threads.isEmpty())
            loadMoreThreads(threads.last().toObject().value("thread_id").toVariant().toLongLong());
    });
    connect(tcp, &TcpMgr::sig_chat_messages, this, [this](const QJsonObject &data) {
        auto *mgr = UserMgr::GetInstance().get();
        const qint64 threadId = data.value("thread_id").toVariant().toLongLong();
        const int peerUid = mgr->FriendIdByThread(threadId);
        if (peerUid <= 0)
            return;
        std::vector<std::shared_ptr<TextChatData>> messages;
        for (const auto &value : data.value("text_array").toArray()) {
            const auto item = value.toObject();
            const int defaultStatus = item.value("fromuid").toInt() == mgr->GetUid()
                ? static_cast<int>(ChatMessageStatus::Sent)
                : static_cast<int>(ChatMessageStatus::Unread);
            const auto status = static_cast<ChatMessageStatus>(
                item.value("status").toInt(defaultStatus));
            auto message = std::make_shared<TextChatData>(item.value("msgid").toString(),
                item.value("content").toString(), item.value("fromuid").toInt(),
                item.value("touid").toInt(), item.value("message_id").toVariant().toLongLong(),
                threadId, status);
            //图片消息的 content 是资源文件名；重新登录后用本机缩略图显示，
            //本机没有压缩图时排入下载队列，界面先显示占位和进度
            if (item.value("msg_type").toInt(0) == static_cast<int>(ChatMsgType::Image)) {
                message->_msg_type = ChatMsgType::Image;
                message->_resource_name = item.value("content").toString();
                QSize pixel_size;
                const QString thumb = ChatService::EnsureImageThumbnail(message->_resource_name,
                    &pixel_size);
                if (!thumb.isEmpty()) {
                    message->_imageSource = QUrl::fromLocalFile(thumb).toString();
                    message->_imageWidth = pixel_size.width();
                    message->_imageHeight = pixel_size.height();
                    message->_transferProgress = 100;
                }
                else if (!message->_resource_name.isEmpty()) {
                    message->_transferProgress = 0; //先显示进度条，由资源下载推进
                    ChatService::GetInstance()->DownloadChatImage(message->_resource_name,
                        message->_from_uid, message->_message_id, threadId);
                }
            }
            messages.push_back(std::move(message));
        }
        mgr->AppendFriendChatMsg(peerUid, std::move(messages));
        if (data.value("load_more").toBool()) {
            const auto items = data.value("text_array").toArray();
            if (!items.isEmpty())
                requestMessages(threadId, items.last().toObject().value("message_id").toVariant().toLongLong());
        }
    });
    connect(tcp, &TcpMgr::sig_private_chat_created, this, [](const QJsonObject &data) {
        if (data.value("error").toInt() != 0) {
            UserMgr::GetInstance()->MarkPrivateChatPending(data.value("uid").toInt(), false);
            return;
        }
        UserMgr::GetInstance()->SetThreadId(data.value("uid").toInt(),
            data.value("thread_id").toVariant().toLongLong());
    });

    // 收到新的好友申请：同一个人只留一条，重复通知直接丢掉
    connect(tcp, &TcpMgr::sig_friend_apply, this,
            [](std::shared_ptr<AddFriendApply> apply) {
                if (!apply)
                    return;
                if (UserMgr::GetInstance()->AlreadyApply(apply->_from_uid))
                    return;
                UserMgr::GetInstance()->AddApplyList(std::make_shared<ApplyInfo>(apply));
            });

    // 互相通过验证：把对方加进好友列表。两个信号分别对应
    // 「别人同意了我」和「我同意了别人」，都要落到同一份好友数据里
    connect(tcp, &TcpMgr::sig_add_auth_friend, this,
            [](std::shared_ptr<AuthInfo> info) {
                if (!info)
                    return;
                auto *mgr = UserMgr::GetInstance().get();
                if (!mgr->CheckFriendById(info->_uid))
                    mgr->AddFriend(info);
                // 对方通过了我的申请，我发出去的那条记录也跟着变成已通过
                mgr->SetApplyStatus(info->_uid, 1, 1);
                mgr->SetThreadId(info->_uid, info->_thread_id);
                if (!info->_welcome_msgs.empty())
                    mgr->AppendFriendChatMsg(info->_uid, info->_welcome_msgs);
            });
    connect(tcp, &TcpMgr::sig_auth_rsp, this, [this](std::shared_ptr<AuthRsp> rsp) {
        if (!rsp)
            return;
        // 服务端确认通过后，申请条目从「添加」变成「已添加」
        UserMgr::GetInstance()->SetApplyStatus(rsp->_uid, 1);
        auto *mgr = UserMgr::GetInstance().get();
        if (!mgr->CheckFriendById(rsp->_uid))
            mgr->AddFriend(rsp);
        mgr->SetThreadId(rsp->_uid, rsp->_thread_id);
        if (!rsp->_welcome_msgs.empty())
            mgr->AppendFriendChatMsg(rsp->_uid, rsp->_welcome_msgs);
        // 好友和欢迎消息都已就位，这里再通知界面跳到会话
        emit friendApproved(rsp->_uid);
    });
    connect(tcp, &TcpMgr::sig_con_success, this, &SessionService::onTcpConnected);
    connect(tcp, &TcpMgr::sig_login_failed, this, &SessionService::onChatLoginFailed);
    connect(tcp, &TcpMgr::sig_login_success, this, &SessionService::onChatLoggedIn);
    connect(tcp, &TcpMgr::sig_con_failed, this, [this](const QString &message) {
        if (!_connecting && !_ready)
            return;
        failAndReset(QStringLiteral("无法连接聊天服务器：%1").arg(message));
    });
    connect(tcp, &TcpMgr::sig_connection_closed, this, [this] {
        if (_connecting)
            failAndReset(QStringLiteral("聊天服务器在登录完成前断开了连接"));
        else {
            _ready = false;
            _heartbeat.stop();
            UserMgr::GetInstance()->FailPendingMessages();
            FileService::GetInstance()->CloseResource(); //聊天断开后资源连接也不保留
        }
    });
    connect(tcp, &TcpMgr::sig_notify_offline, this, [this] {
        if (_connecting) {
            failAndReset(QStringLiteral("登录期间会话被其他终端替换，请重试"));
            return;
        }
        _ready = false;
        _heartbeat.stop();
        FileService::GetInstance()->CloseResource(); //被顶下线后token已失效，资源服务也不能用
    });
}

SessionService::~SessionService()
{
}

// 建立会话：清掉上一个账号的数据，连 TCP 并发登录帧，整体超时兜底
void SessionService::connectToChat(const ServerInfo &info)
{
    //不复用closeSession：它会把旧连接的关闭信号当成新登录过程中的断线
    _timeout.stop();
    _heartbeat.stop();
    _ready = false;
    _current = {};
    UserMgr::GetInstance()->ClearSession();

    _pending = info;
    _connecting = true;
    _timeout.start();

    emit connecting();

    //聊天连接与资源连接并行建立；资源服务器不可用不应阻塞聊天登录
    TcpMgr::GetInstance()->ConnectToServer(info);
    FileService::GetInstance()->ConnectToResource(info); //与聊天并行建连，不阻塞登录
}

// 主动结束会话：停心跳、停超时、断开连接
void SessionService::closeSession()
{
    _timeout.stop();
    _heartbeat.stop();
    _ready = false;
    _connecting = false;
    _current = {};
    _pending = {};
    TcpMgr::GetInstance()->CloseConnectionAsync();
    FileService::GetInstance()->CloseResource();
}

// TCP 连上之后发 ID_CHAT_LOGIN 帧，把 uid 与 token 交给聊天服务器
void SessionService::onTcpConnected(bool ok)
{
    if (!_connecting)
        return;
    if (!ok) {
        failAndReset(QStringLiteral("网络异常"));
        return;
    }

    // 长连接已建立，发送聊天服务器登录帧
    QJsonObject frame;
    frame["uid"] = _pending.Uid;
    frame["token"] = _pending.Token;

    const QByteArray payload =
        QJsonDocument(frame).toJson(QJsonDocument::Indented);

    emit TcpMgr::GetInstance()->sig_send_data(ReqId::ID_CHAT_LOGIN, payload);
}

// 聊天服务器拒绝登录（token 过期 / 重复登录等）
void SessionService::onChatLoginFailed(int code)
{
    if (!_connecting)
        return;
    failAndReset(QStringLiteral("登录失败，错误码 %1").arg(code));
}

// 聊天服务器登录成功：会话就绪，开始定时心跳
void SessionService::onChatLoggedIn(const QJsonObject &data)
{
    if (!_connecting)
        return;
    if (data.value("uid").toInt() != _pending.Uid) {
        failAndReset(QStringLiteral("聊天服务返回的账号不匹配"));
        return;
    }
    auto *mgr = UserMgr::GetInstance().get();
    mgr->SetToken(_pending.Token); //资源服务器校验同一个utoken
    mgr->SetUserInfo(std::make_shared<UserInfo>(data["uid"].toInt(), data["name"].toString(),
        data["nick"].toString(), data["icon"].toString(), data["sex"].toInt()));
    mgr->AppendApplyList(data["apply_list"].toArray());
    mgr->AppendFriendList(data["friend_list"].toArray());
    mgr->SetChatThreads(data["thread_list"].toArray());
    //登录响应只带一页会话；先给每个会话拉第一页消息，会话更多时按游标继续
    const auto threads = data["thread_list"].toArray();
    for (const auto &value : threads)
        requestMessages(value.toObject().value("thread_id").toVariant().toLongLong());
    if (data.value("thread_more").toBool() && !threads.isEmpty())
        loadMoreThreads(threads.last().toObject().value("thread_id").toVariant().toLongLong());
    _connecting = false;
    _timeout.stop();
    _ready = true;
    _current = _pending;
    _heartbeat.start();
    FileService::GetInstance()->OnSessionReady(); //登录完成后再补拉缺失头像
    emit sessionReady();
}

// 统一构造消息拉取帧：thread_id 无效时直接丢弃，避免服务端按 0 查库
void SessionService::requestMessages(qint64 threadId, qint64 lastMessageId)
{
    if (threadId <= 0)
        return;
    QJsonObject request{{"fromuid", UserMgr::GetInstance()->GetUid()},
                        {"thread_id", threadId}, {"message_id", lastMessageId}};
    emit TcpMgr::GetInstance()->sig_send_data(ReqId::ID_LOAD_CHAT_MSG_REQ,
        QJsonDocument(request).toJson(QJsonDocument::Compact));
}

// 会话列表翻页：服务端只认大于该值的 thread_id
void SessionService::loadMoreThreads(qint64 lastThreadId)
{
    if (lastThreadId <= 0)
        return;
    QJsonObject request{{"fromuid", UserMgr::GetInstance()->GetUid()},
                        {"thread_id", lastThreadId}};
    emit TcpMgr::GetInstance()->sig_send_data(ReqId::ID_LOAD_CHAT_THREAD_REQ,
        QJsonDocument(request).toJson(QJsonDocument::Compact));
}

// 统一失败出口：停表、复位状态，把原因抛给界面
void SessionService::failAndReset(const QString &msg)
{
    closeSession();
    emit connectFailed(msg);
}
