#include "sessionservice.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>
#include <QFileInfo>
#include <QImageReader>
#include <QSize>
#include <QUrl>

#include "chatservice.h"
#include "tcpmgr.h"
#include "usermgr.h"
#include "fileservice.h"
#include "groupservice.h"

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

    // 收到新消息：单聊与群聊都按 thread 落到 UserMgr。
    // 只有别人发来的才计未读；正在看的会话立刻上报已读。
    // 群聊先把 conv 归到群，未读按 thread 计，@我 单独标出来
    connect(tcp, &TcpMgr::sig_text_chat_msg, this,
            [](std::shared_ptr<TextChatMsg> msg) {
                if (!msg)
                    return;
                auto *mgr = UserMgr::GetInstance().get();
                const int self = mgr->GetUid();
                ConversationRef conv;
                conv._thread_id = msg->_thread_id;
                conv._kind = static_cast<ChatThreadKind>(msg->_thread_type);
                if (conv._kind == ChatThreadKind::Group) {
                    //被邀请的人可能还没拿到群资料：先占位并补拉，消息不能丢
                    if (!mgr->GetGroup(conv._thread_id)) {
                        mgr->EnsureGroup(conv._thread_id);
                        GroupService::GetInstance()->RequestGroupInfo(conv._thread_id);
                    }
                }
                else {
                    conv._peer_uid = msg->_from_uid == self ? msg->_to_uid : msg->_from_uid;
                    if (msg->_thread_id > 0)
                        mgr->SetThreadId(conv._peer_uid, msg->_thread_id);
                    else
                        conv._thread_id = mgr->ThreadId(conv._peer_uid);
                }

                int incoming = 0;
                bool at_me = false;
                for (const auto &chat : msg->_chat_msgs) {
                    if (!chat || chat->_from_uid == self)
                        continue;
                    ++incoming;
                    if (chat->_mentions.contains(QString::number(self)))
                        at_me = true;
                }
                for (int i = 0; i < incoming; ++i)
                    mgr->IncThreadUnread(conv._thread_id);
                if (at_me)
                    mgr->MarkThreadAtMe(conv._thread_id);

                mgr->AppendChatMsgs(conv, msg->_chat_msgs);
                //服务端可能刚刚补建了会话，这里同步 thread_id
                if (conv._kind == ChatThreadKind::Private && msg->_thread_id > 0)
                    conv._thread_id = msg->_thread_id;
                if (conv._thread_id == mgr->ActiveThread())
                    ChatService::GetInstance()->MarkSessionRead(conv);
            });
    //1018 回执带错误：按 msgid 把对应会话里的消息标失败
    connect(tcp, &TcpMgr::sig_chat_msg_failed, this, [](const QJsonObject &data) {
        ConversationRef conv;
        conv._thread_id = data.value("thread_id").toVariant().toLongLong();
        conv._kind = static_cast<ChatThreadKind>(data.value("thread_type").toInt(1));
        conv._peer_uid = conv._kind == ChatThreadKind::Private ? data.value("touid").toInt() : 0;
        for (const auto &value : data.value("text_array").toArray())
            UserMgr::GetInstance()->UpdateChatMessage(conv,
                value.toObject().value("msgid").toString(), 0, ChatMessageStatus::Failed);
    });
    //对端已读：把自己发的、消息号不超过它的消息换成双勾（仅单聊）
    connect(tcp, &TcpMgr::sig_message_read, this, [](const QJsonObject &data) {
        const int uid = data.value("uid").toInt();
        if (uid <= 0)
            return;
        UserMgr::GetInstance()->MarkMessagesReadTo(uid,
            data.value("message_id").toVariant().toLongLong());
    });
    //自己撤回成功 / 别人撤回：消息号全局唯一，直接按号定位，不再依赖会话类型
    connect(tcp, &TcpMgr::sig_recall_rsp, this, [](const QJsonObject &data) {
        if (data.value("error").toInt(1) != 0)
            return;
        UserMgr::GetInstance()->MarkMessageRecalled(
            data.value("message_id").toVariant().toLongLong());
    });
    connect(tcp, &TcpMgr::sig_message_recalled, this, [](const QJsonObject &data) {
        UserMgr::GetInstance()->MarkMessageRecalled(
            data.value("message_id").toVariant().toLongLong());
    });
    //图片消息回执：补上 message_id/thread_id 后启动资源上传；失败则结束本地消息
    connect(tcp, &TcpMgr::sig_chat_img_rsp, this, [](const QJsonObject &data) {
        auto *mgr = UserMgr::GetInstance().get();
        const QString uniqueId = data.value("unique_id").toString();
        if (uniqueId.isEmpty())
            return;

        ConversationRef conv;
        conv._thread_id = data.value("thread_id").toVariant().toLongLong();
        conv._kind = static_cast<ChatThreadKind>(data.value("thread_type").toInt(1));
        conv._peer_uid = conv._kind == ChatThreadKind::Private ? data.value("touid").toInt() : 0;

        if (data.value("error").toInt(1) != 0) {
            ChatService::GetInstance()->discardPendingImage(uniqueId);
            mgr->UpdateChatMessage(conv, uniqueId, 0, ChatMessageStatus::Failed);
            return;
        }

        if (conv._kind == ChatThreadKind::Private && conv._thread_id > 0)
            mgr->SetThreadId(conv._peer_uid, conv._thread_id);
        //消息已落库但资源还没上传；状态保持发送中，由资源上传进度推进到已发送
        const qint64 messageId = data.value("message_id").toVariant().toLongLong();
        mgr->UpdateChatMessage(conv, uniqueId, messageId, ChatMessageStatus::Sending);
        ChatService::GetInstance()->uploadPendingImage(uniqueId, messageId, conv);
    });
    //接收方图片上传完成：补消息并下载资源，界面先在会话里显示占位和进度
    connect(tcp, &TcpMgr::sig_image_notify, this, [](const QJsonObject &data) {
        const qint64 threadId = data.value("thread_id").toVariant().toLongLong();
        const int from = data.value("fromuid").toInt();
        const int self = UserMgr::GetInstance()->GetUid();
        if (from > 0 && from != self && threadId > 0 && threadId != UserMgr::GetInstance()->ActiveThread())
            UserMgr::GetInstance()->IncThreadUnread(threadId);
        ChatService::GetInstance()->HandleImageNotify(data);
        if (threadId > 0 && threadId == UserMgr::GetInstance()->ActiveThread()) {
            ConversationRef conv;
            conv._thread_id = threadId;
            conv._kind = static_cast<ChatThreadKind>(data.value("thread_type").toInt(1));
            conv._peer_uid = conv._kind == ChatThreadKind::Private ? from : 0;
            ChatService::GetInstance()->MarkSessionRead(conv);
        }
    });
    //视频/文件回执与上传完成通知：卡片先出现，资源等用户点击后再下载
    connect(tcp, &TcpMgr::sig_resource_chat_rsp, this, [](const QJsonObject &data) {
        ChatService::GetInstance()->HandleResourceRsp(data);
    });
    connect(tcp, &TcpMgr::sig_resource_notify, this, [](const QJsonObject &data) {
        const qint64 threadId = data.value("thread_id").toVariant().toLongLong();
        const int from = data.value("fromuid").toInt();
        const int self = UserMgr::GetInstance()->GetUid();
        if (from > 0 && from != self && threadId > 0 && threadId != UserMgr::GetInstance()->ActiveThread())
            UserMgr::GetInstance()->IncThreadUnread(threadId);
        ChatService::GetInstance()->HandleResourceNotify(data);
        if (threadId > 0 && threadId == UserMgr::GetInstance()->ActiveThread()) {
            ConversationRef conv;
            conv._thread_id = threadId;
            conv._kind = static_cast<ChatThreadKind>(data.value("thread_type").toInt(1));
            conv._peer_uid = conv._kind == ChatThreadKind::Private ? from : 0;
            ChatService::GetInstance()->MarkSessionRead(conv);
        }
    });
    connect(tcp, &TcpMgr::sig_chat_threads, this, [this](const QJsonObject &data) {
        auto *mgr = UserMgr::GetInstance().get();
        const auto threads = data.value("thread_list").toArray();
        mgr->AppendChatThreads(threads);
        mgr->RefreshFriendList();
        //每个会话先拿第一页，剩下的由 load_more 继续；群聊再补一次成员，
        //会话行的"谁说了什么"与气泡名字都要用成员表
        for (const auto &value : threads) {
            const auto item = value.toObject();
            const qint64 threadId = item.value("thread_id").toVariant().toLongLong();
            requestMessages(threadId);
            if (item.value("thread_type").toInt(1) == static_cast<int>(ChatThreadKind::Group))
                GroupService::GetInstance()->RequestGroupInfo(threadId);
        }
        if (data.value("load_more").toBool() && !threads.isEmpty())
            loadMoreThreads(threads.last().toObject().value("thread_id").toVariant().toLongLong());
    });
    connect(tcp, &TcpMgr::sig_chat_messages, this, [this](const QJsonObject &data) {
        auto *mgr = UserMgr::GetInstance().get();
        const qint64 threadId = data.value("thread_id").toVariant().toLongLong();
        if (threadId <= 0)
            return;
        ConversationRef conv;
        conv._thread_id = threadId;
        conv._kind = static_cast<ChatThreadKind>(data.value("thread_type").toInt(1));
        if (conv._kind == ChatThreadKind::Group) {
            if (!mgr->GetGroup(threadId)) {
                mgr->EnsureGroup(threadId);
                GroupService::GetInstance()->RequestGroupInfo(threadId);
            }
        }
        else {
            conv._peer_uid = mgr->FriendIdByThread(threadId);
            if (conv._peer_uid <= 0)
                return;
        }

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
            //落库时间：历史补拉与实时消息用同一套字段，界面据此画时间条与排序
            message->_time = item.value("time").toVariant().toLongLong();
            message->_recalled = item.value("recalled").toBool();
            const QString mentions = item.value("mentions").toString();
            for (const QString &uid : mentions.split(QLatin1Char(','), Qt::SkipEmptyParts))
                message->_mentions.append(uid.trimmed());
            //自己发的消息如果库里已标已读，直接按已读显示，不必等下一次通知
            if (message->_status == ChatMessageStatus::Sent && item.value("read").toBool())
                message->_status = ChatMessageStatus::Read;
            //图片消息的 content 是资源文件名；视频/文件的 content 是元数据 JSON。
            //图片自动补全，视频只补首帧，视频/文件本体等用户点击下载
            const int msgType = item.value("msg_type").toInt(0);
            if (msgType == static_cast<int>(ChatMsgType::System)) {
                message->_msg_type = ChatMsgType::System;
            }
            else if (msgType == static_cast<int>(ChatMsgType::Image)) {
                message->_msg_type = ChatMsgType::Image;
                message->_resource_name = item.value("content").toString();
                QSize pixel_size;
                const QString thumb = ChatService::EnsureImageThumbnail(message->_resource_name,
                    &pixel_size);
                if (!thumb.isEmpty()) {
                    message->_imageSource = QUrl::fromLocalFile(thumb).toString();
                    message->_imageWidth = pixel_size.width();
                    message->_imageHeight = pixel_size.height();
                    message->_local_path = ChatService::ImageCachePath(message->_resource_name);
                    message->_transferProgress = 100;
                    message->_transfer_state = ResourceTransferState::Done;
                }
                else if (!message->_resource_name.isEmpty()) {
                    message->_transferProgress = 0; //先显示进度条，由资源下载推进
                    message->_transfer_state = ResourceTransferState::Downloading;
                    ChatService::GetInstance()->DownloadChatImage(message->_resource_name,
                        message->_from_uid, message->_message_id, threadId);
                }
            }
            else if (msgType == static_cast<int>(ChatMsgType::Video)
                || msgType == static_cast<int>(ChatMsgType::File)) {
                auto resource = ChatService::BuildResourceMessage(message->_from_uid,
                    message->_to_uid, message->_message_id, threadId, msgType,
                    item.value("content").toString(), QString(), status);
                if (resource && !resource->_resource_name.isEmpty()) {
                    resource->_msg_id = message->_msg_id;
                    const QString local = ChatService::ResourcePath(resource->_msg_type,
                        resource->_resource_name);
                    if (QFileInfo::exists(local)) {
                        resource->_local_path = local;
                        resource->_transferProgress = 100;
                        resource->_transfer_state = ResourceTransferState::Done;
                    }
                    if (resource->_msg_type == ChatMsgType::Video
                        && !resource->_thumb_name.isEmpty()) {
                        const QString thumbPath = ChatService::VideoThumbPath(
                            resource->_thumb_name);
                        if (QFileInfo::exists(thumbPath)) {
                            resource->_imageSource = QUrl::fromLocalFile(thumbPath).toString();
                            resource->_imageWidth = QImageReader(thumbPath).size().width();
                            resource->_imageHeight = QImageReader(thumbPath).size().height();
                        }
                        else {
                            ChatService::GetInstance()->DownloadVideoThumb(
                                resource->_thumb_name, resource->_from_uid,
                                resource->_message_id, threadId);
                        }
                    }
                    message = std::move(resource);
                }
            }
            messages.push_back(std::move(message));
        }
        mgr->AppendChatMsgs(conv, std::move(messages));
        //打开会话后历史一落地就上报已读；不在前台时不报，等用户真正进入会话再报
        if (threadId == mgr->ActiveThread())
            ChatService::GetInstance()->MarkSessionRead(conv);
        if (data.value("load_more").toBool()) {
            const auto items = data.value("text_array").toArray();
            if (!items.isEmpty())
                requestMessages(threadId,
                    items.last().toObject().value("message_id").toVariant().toLongLong());
        }
    });
    //中途被拉进新群：群资料到位后拉第一页历史，邀请前的消息也能看到
    connect(GroupService::GetInstance().get(), &GroupService::groupActivated, this,
            [this](qint64 threadId) { requestMessages(threadId); });
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
                if (!info->_welcome_msgs.empty()) {
                    ConversationRef conv;
                    conv._thread_id = info->_thread_id;
                    conv._peer_uid = info->_uid;
                    mgr->AppendChatMsgs(conv, info->_welcome_msgs);
                }
            });
    //同意方与被同意方都要有的本地装配：好友、会话、欢迎消息。
    //incoming 表示"对方通过我的申请"，只有这一侧才把默认招呼算成未读；
    //我自己同意时正进入会话，算未读反而会立刻被清掉
    auto applyFriendAdded = [](const std::shared_ptr<AuthRsp> &rsp, bool incoming) {
        auto *mgr = UserMgr::GetInstance().get();
        mgr->SetApplyStatus(rsp->_uid, 1);
        if (!mgr->CheckFriendById(rsp->_uid))
            mgr->AddFriend(rsp);
        mgr->SetThreadId(rsp->_uid, rsp->_thread_id);
        ConversationRef conv;
        conv._thread_id = rsp->_thread_id;
        conv._peer_uid = rsp->_uid;
        if (!rsp->_welcome_msgs.empty())
            mgr->AppendChatMsgs(conv, rsp->_welcome_msgs);
        if (!incoming)
            return;
        //默认招呼是为了让申请方知道"对方通过了"，对申请方就是一条普通未读消息
        for (const auto &message : rsp->_welcome_msgs) {
            if (message && message->_from_uid == rsp->_uid)
                mgr->IncThreadUnread(rsp->_thread_id);
        }
    };
    connect(tcp, &TcpMgr::sig_auth_rsp, this, [this, applyFriendAdded](std::shared_ptr<AuthRsp> rsp) {
        if (!rsp)
            return;
        //1015 别人通过我的申请：列表与红点更新，不强行切页
        applyFriendAdded(rsp, true);
        emit friendApproved(rsp->_uid);
    });
    connect(tcp, &TcpMgr::sig_auth_friend_rsp, this, [this, applyFriendAdded](std::shared_ptr<AuthRsp> rsp) {
        if (!rsp)
            return;
        //1014 我自己同意别人：不计未读，界面直接进会话
        applyFriendAdded(rsp, false);
        emit friendAdded(rsp->_uid);
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
    auto self_info = std::make_shared<UserInfo>(data["uid"].toInt(), data["name"].toString(),
        data["nick"].toString(), data["icon"].toString(), data["sex"].toInt());
    self_info->_desc = data["desc"].toString(); //个性签名只在资料页展示，单独补上
    mgr->SetUserInfo(self_info);
    mgr->AppendApplyList(data["apply_list"].toArray());
    mgr->AppendFriendList(data["friend_list"].toArray());
    mgr->SetChatThreads(data["thread_list"].toArray());
    //登录响应只带一页会话；先给每个会话拉第一页消息，会话更多时按游标继续
    const auto threads = data["thread_list"].toArray();
    for (const auto &value : threads) {
        const auto item = value.toObject();
        const qint64 threadId = item.value("thread_id").toVariant().toLongLong();
        requestMessages(threadId);
        if (item.value("thread_type").toInt(1) == static_cast<int>(ChatThreadKind::Group))
            GroupService::GetInstance()->RequestGroupInfo(threadId);
    }
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
