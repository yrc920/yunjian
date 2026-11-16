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
#include "localstore.h"

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
        for (const auto &value : threads) {
            const qint64 id = value.toObject().value("thread_id").toVariant().toLongLong();
            if (id > 0)
                _seen_threads.insert(id);
        }
        mgr->AppendChatThreads(threads);
        mgr->RefreshFriendList();
        //每个会话先拿第一页，剩下的由 load_more 继续；群聊再补一次成员，
        //会话行的"谁说了什么"与气泡名字都要用成员表
        for (const auto &value : threads) {
            const auto item = value.toObject();
            const qint64 threadId = item.value("thread_id").toVariant().toLongLong();
            requestMessages(threadId, LocalStore::GetInstance()->SyncCursor(threadId));
            if (item.value("thread_type").toInt(1) == static_cast<int>(ChatThreadKind::Group))
                GroupService::GetInstance()->RequestGroupInfo(threadId);
        }
        if (data.value("load_more").toBool() && !threads.isEmpty())
            loadMoreThreads(threads.last().toObject().value("thread_id").toVariant().toLongLong());
        else
            finishThreadList();
    });    connect(tcp, &TcpMgr::sig_chat_messages, this, [this](const QJsonObject &data) {
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
        const auto items = data.value("text_array").toArray();
        for (const auto &value : items) {
            const auto item = value.toObject();
            //历史回包当成一行库记录来解析：本地翻页与历史同步共用同一条还原路径，
            //图片/视频的本地文件、缩略图与传输状态都在那一处补齐
            StoredMessage row;
            row.client_msg_id = item.value("msgid").toString();
            row.server_id = item.value("message_id").toVariant().toLongLong();
            row.thread_id = threadId;
            row.sender_id = item.value("fromuid").toInt();
            row.recv_id = item.value("touid").toInt();
            row.msg_type = item.value("msg_type").toInt(0);
            row.content = item.value("content").toString();
            row.mentions = item.value("mentions").toString();
            row.created_at = item.value("time").toVariant().toLongLong();
            row.status = item.value("status").toInt(row.sender_id == mgr->GetUid()
                ? static_cast<int>(ChatMessageStatus::Sent)
                : static_cast<int>(ChatMessageStatus::Unread));
            row.is_read = item.value("read").toBool();
            row.recalled = item.value("recalled").toBool();
            messages.push_back(ChatService::GetInstance()->MessageFromStored(row, mgr->GetUid()));
        }
        mgr->AppendChatMsgs(conv, std::move(messages));
        //打开会话后历史一落地就上报已读；不在前台时不报，等用户真正进入会话再报
        if (threadId == mgr->ActiveThread())
            ChatService::GetInstance()->MarkSessionRead(conv);
        //历史按消息号升序分页，还有更新的就继续追；页数设上限，
        //避免异常数据让登录阶段无限翻页（正常登录游标已经在头部，一页就结束）
        if (data.value("load_more").toBool() && !items.isEmpty() && _history_pages < 2000) {
            ++_history_pages;
            requestMessages(threadId,
                items.last().toObject().value("message_id").toVariant().toLongLong());
        }
    });
    //中途被拉进新群：群资料到位后拉第一页历史，邀请前的消息也能看到
    connect(GroupService::GetInstance().get(), &GroupService::groupActivated, this,
            [this](qint64 threadId) {
                requestMessages(threadId, LocalStore::GetInstance()->SyncCursor(threadId));
            });
    connect(tcp, &TcpMgr::sig_private_chat_created, this, [](const QJsonObject &data) {
        if (data.value("error").toInt() != 0) {
            UserMgr::GetInstance()->MarkPrivateChatPending(data.value("uid").toInt(), false);
            return;
        }
        UserMgr::GetInstance()->SetThreadId(data.value("uid").toInt(),
            data.value("thread_id").toVariant().toLongLong());
    });

    // 收到新的好友申请：交给 UserMgr 去重并挪到列表最前面。
    //不能再按"以前收到过这个人的申请"直接丢弃：旧记录可能早已通过（status=1），
    //删好友后对方重新申请时会被这条判断吞掉，接收方既看不到条目也没有红点
    connect(tcp, &TcpMgr::sig_friend_apply, this,
            [](std::shared_ptr<AddFriendApply> apply) {
                if (!apply)
                    return;
                UserMgr::GetInstance()->AddApplyList(std::make_shared<ApplyInfo>(apply));
            });

    //同意方与被同意方都要有的本地装配：好友、会话、欢迎消息。
    //incoming 表示"对方通过我的申请"，只有这一侧才把默认招呼算成未读；
    //我自己同意时正进入会话，算未读反而会立刻被清掉
    auto applyFriendAdded = [](const std::shared_ptr<AuthRsp> &rsp, bool incoming) {
        auto *mgr = UserMgr::GetInstance().get();
        //两个方向都置为已通过：申请方那侧是"我发出的"，同意方那侧是"别人申请我"
        mgr->SetApplyStatus(rsp->_uid, 1, 0);
        mgr->SetApplyStatus(rsp->_uid, 1, 1);
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

    // 登出回包可能被网络丢掉，超时后照常断开；服务端已经处理或令牌由 TTL 兜底
    _logout_timer.setSingleShot(true);
    _logout_timer.setInterval(1000);
    connect(&_logout_timer, &QTimer::timeout, this, &SessionService::finishLogout);
    connect(tcp, &TcpMgr::sig_logout_rsp, this, &SessionService::finishLogout);
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
    _history_pages = 0;
    _seen_threads.clear();
    _threads_complete = false;
    UserMgr::GetInstance()->ClearSession(); //内部会关闭上一个账号的本地库

    //先开库并恢复缓存：网络回来之前会话列表与历史就已经可读，
    //1006 到达后只做一次合并，不必等每个会话的第一页消息
    auto store = LocalStore::GetInstance();
    if (store->Open(info.Uid))
        UserMgr::GetInstance()->RestoreFromCache();

    _pending = info;
    _connecting = true;
    _timeout.start();

    emit connecting();

    //聊天连接与资源连接并行建立；资源服务器不可用不应阻塞聊天登录
    TcpMgr::GetInstance()->ConnectToServer(info);
    FileService::GetInstance()->ConnectToResource(info); //与聊天并行建连，不阻塞登录
}

// 主动登出：先把登出帧交给服务端，收到回包或 1 秒超时后关闭连接。
// 只有用户明确退出才走这里；被踢、断网、登录超时都不吊销令牌
void SessionService::logout()
{
    if (_logging_out)
        return;
    if (!_ready) {
        closeSession();
        return;
    }

    _logging_out = true;
    const QByteArray payload =
        QJsonDocument(QJsonObject{}).toJson(QJsonDocument::Indented);
    emit TcpMgr::GetInstance()->sig_send_data(ReqId::ID_LOGOUT_REQ, payload);
    _logout_timer.start();
}

void SessionService::finishLogout()
{
    if (!_logging_out)
        return;
    _logging_out = false;
    _logout_timer.stop();
    closeSession();
}

// 主动结束会话：停心跳、停超时、断开连接
void SessionService::closeSession()
{
    _logout_timer.stop();
    _logging_out = false;
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

    // 把服务端错误码翻成可操作的提示：能重试的与需要重新登录的要区分开
    QString text;
    switch (code) {
    case 1010: text = QStringLiteral("登录凭证已失效，请重新登录"); break;
    case 1011: text = QStringLiteral("账号不存在或已被注销"); break;
    case 1101: text = QStringLiteral("登录服务暂时不可用，请稍后重试"); break;
    case 1104: text = QStringLiteral("聊天服务正在启动，请稍后重试"); break;
    case 1105: text = QStringLiteral("登录过于频繁，请稍后再试"); break;
    default: text = QStringLiteral("登录失败，错误码 %1").arg(code); break;
    }
    failAndReset(text);
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
    _history_pages = 0;
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
        if (threadId > 0)
            _seen_threads.insert(threadId);
        requestMessages(threadId, LocalStore::GetInstance()->SyncCursor(threadId));
        if (item.value("thread_type").toInt(1) == static_cast<int>(ChatThreadKind::Group))
            GroupService::GetInstance()->RequestGroupInfo(threadId);
    }
    if (data.value("thread_more").toBool() && !threads.isEmpty())
        loadMoreThreads(threads.last().toObject().value("thread_id").toVariant().toLongLong());
    else
        finishThreadList();
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

//会话列表到齐：本地有、这次没下发的群说明已经被移出或解散。
//只标状态不删历史，用户还能翻记录，清不清由他自己决定
void SessionService::finishThreadList()
{
    if (_threads_complete)
        return;
    _threads_complete = true;
    UserMgr::GetInstance()->MarkMissingGroupsLeft(_seen_threads);
}

// 统一失败出口：停表、复位状态，把原因抛给界面
void SessionService::failAndReset(const QString &msg)
{
    closeSession();
    emit connectFailed(msg);
}
