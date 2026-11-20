#include "log.h"
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
constexpr int kMaxReconnectAttempts = 6; //约一分钟仍连不上就交给用户重新登录
constexpr int kReconnectBaseMs = 1000;
constexpr int kReconnectMaxMs = 15000;
}

SessionService::SessionService() : _ready(false)
{
    _timeout.setSingleShot(true);
    _timeout.setInterval(kSessionTimeoutMs);
    connect(&_timeout, &QTimer::timeout, this, &SessionService::onSessionTimeout);

    //断线后不立刻猛连：按 1/2/4/8/15 秒退避，给服务端重启与网络恢复留时间
    _reconnect_timer.setSingleShot(true);
    connect(&_reconnect_timer, &QTimer::timeout, this, &SessionService::startReconnectAttempt);

    //TCP 在 Wi-Fi 掉线/休眠后可能长时间不报 disconnected；心跳回包 35 秒没来就主动重连
    _heartbeat_watchdog.setSingleShot(true);
    _heartbeat_watchdog.setInterval(35000);
    connect(&_heartbeat_watchdog, &QTimer::timeout, this, [this] {
        if (!_ready)
            return;
        qCWarning(lcApp) << "[session] heartbeat timeout, reconnect";
        _ready = false;
        _heartbeat.stop();
        UserMgr::GetInstance()->FailPendingMessages();
        FileService::GetInstance()->CloseResource();
        _reconnect_attempt = 0;
        TcpMgr::GetInstance()->CloseConnectionAsync(); //主动关，避免再走一次断线回调
        scheduleReconnect(QStringLiteral("与服务器的连接已超时"));
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
            QJsonDocument(frame).toJson(QJsonDocument::Compact));
    });

    auto *tcp = TcpMgr::GetInstance().get();
    connect(tcp, &TcpMgr::sig_heartbeat_ok, this, [this] {
        //只有在线会话需要喂狗；重连/登录阶段的回包不参与
        if (_ready)
            _heartbeat_watchdog.start();
    });

    // 收到新消息：单聊与群聊都按 thread 落到 UserMgr。
    // 只有别人发来的才计未读；正在看的会话立刻上报已读。
    // 群聊先把 conv 归到群，未读按 thread 计，@我 单独标出来
    connect(tcp, &TcpMgr::sig_text_chat_msg, this,
            [this](std::shared_ptr<TextChatMsg> msg) {
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
                //只在没看这条会话时提示；看完最后一条再弹通知会显得多余
                if (incoming > 0 && conv._thread_id > 0
                    && conv._thread_id != mgr->ActiveThread()) {
                    std::shared_ptr<TextChatData> latest;
                    for (const auto &chat : msg->_chat_msgs) {
                        if (chat && chat->_from_uid != self
                            && chat->_msg_type == ChatMsgType::Text && !chat->_recalled)
                            latest = chat;
                    }
                    if (latest) {
                        QString body = latest->_msg_content;
                        if (conv._kind == ChatThreadKind::Group)
                            body = mgr->GroupSenderName(conv._thread_id, latest->_from_uid)
                                + QStringLiteral("：") + body;
                        if (latest->_mentions.contains(QString::number(self)))
                            body.prepend(QStringLiteral("[有人@我] "));
                        QString title = mgr->ThreadTitle(conv._thread_id);
                        if (title.isEmpty())
                            title = tr("新消息");
                        emit messageArrived(title, body.left(120), conv._thread_id,
                                            static_cast<int>(conv._kind), conv._peer_uid);
                    }
                }
                if (conv._thread_id == mgr->ActiveThread())
                    ChatService::GetInstance()->MarkSessionRead(conv);
            });
    //4008 回执带错误：按 msgid 把对应会话里的消息标失败
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
    connect(tcp, &TcpMgr::sig_image_notify, this, [this](const QJsonObject &data) {
        auto *mgr = UserMgr::GetInstance().get();
        const qint64 threadId = data.value("thread_id").toVariant().toLongLong();
        const int from = data.value("fromuid").toInt();
        const int self = mgr->GetUid();
        if (from > 0 && from != self && threadId > 0 && threadId != mgr->ActiveThread())
            mgr->IncThreadUnread(threadId);
        ChatService::GetInstance()->HandleImageNotify(data);
        const int kind = data.value("thread_type").toInt(1);
        if (from > 0 && from != self && threadId > 0 && threadId != mgr->ActiveThread()) {
            QString title = mgr->ThreadTitle(threadId);
            if (title.isEmpty())
                title = tr("新消息");
            emit messageArrived(title, tr("[图片]"), threadId, kind,
                                kind == static_cast<int>(ChatThreadKind::Private) ? from : 0);
        }
        if (threadId > 0 && threadId == mgr->ActiveThread()) {
            ConversationRef conv;
            conv._thread_id = threadId;
            conv._kind = static_cast<ChatThreadKind>(kind);
            conv._peer_uid = conv._kind == ChatThreadKind::Private ? from : 0;
            ChatService::GetInstance()->MarkSessionRead(conv);
        }
    });
    //视频/文件回执与上传完成通知：卡片先出现，资源等用户点击后再下载
    connect(tcp, &TcpMgr::sig_resource_chat_rsp, this, [](const QJsonObject &data) {
        ChatService::GetInstance()->HandleResourceRsp(data);
    });
    connect(tcp, &TcpMgr::sig_resource_notify, this, [this](const QJsonObject &data) {
        auto *mgr = UserMgr::GetInstance().get();
        const qint64 threadId = data.value("thread_id").toVariant().toLongLong();
        const int from = data.value("fromuid").toInt();
        const int self = mgr->GetUid();
        if (from > 0 && from != self && threadId > 0 && threadId != mgr->ActiveThread())
            mgr->IncThreadUnread(threadId);
        ChatService::GetInstance()->HandleResourceNotify(data);
        const int kind = data.value("thread_type").toInt(1);
        if (from > 0 && from != self && threadId > 0 && threadId != mgr->ActiveThread()) {
            const int msgType = data.value("msg_type").toInt(
                static_cast<int>(ChatMsgType::File));
            QString body = msgType == static_cast<int>(ChatMsgType::Video)
                ? tr("[视频]") : tr("[文件]");
            const QString name = data.value("file_name").toString();
            if (!name.isEmpty())
                body += QLatin1Char(' ') + name;
            QString title = mgr->ThreadTitle(threadId);
            if (title.isEmpty())
                title = tr("新消息");
            emit messageArrived(title, body.left(120), threadId, kind,
                                kind == static_cast<int>(ChatThreadKind::Private) ? from : 0);
        }
        if (threadId > 0 && threadId == mgr->ActiveThread()) {
            ConversationRef conv;
            conv._thread_id = threadId;
            conv._kind = static_cast<ChatThreadKind>(kind);
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
        if (data.value("load_more").toBool() && !threads.isEmpty()) {
            const auto last = threads.last().toObject();
            loadMoreThreads(last.value("thread_id").toVariant().toLongLong(),
                last.value("time").toVariant().toLongLong());
        }
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
        //3008 别人通过我的申请：列表与红点更新，不强行切页
        applyFriendAdded(rsp, true);
        emit friendApproved(rsp->_uid);
    });
    connect(tcp, &TcpMgr::sig_auth_friend_rsp, this, [this, applyFriendAdded](std::shared_ptr<AuthRsp> rsp) {
        if (!rsp)
            return;
        //3007 我自己同意别人：不计未读，界面直接进会话
        applyFriendAdded(rsp, false);
        emit friendAdded(rsp->_uid);
    });
    connect(tcp, &TcpMgr::sig_con_success, this, &SessionService::onTcpConnected);
    connect(tcp, &TcpMgr::sig_login_failed, this, &SessionService::onChatLoginFailed);
    connect(tcp, &TcpMgr::sig_login_success, this, &SessionService::onChatLoggedIn);
    connect(tcp, &TcpMgr::sig_con_failed, this, [this](const QString &message) {
        const QString text = QStringLiteral("无法连接聊天服务器：%1").arg(message);
        if (_reconnecting) {
            _connecting = false;
            scheduleReconnect(text);
            return;
        }
        if (!_connecting && !_ready)
            return;
        //已经连上时，迟到的连接错误可能来自旧 socket；真正断线由 disconnected 处理，
        //不能因为一条过期错误把健康会话拆掉
        if (_ready)
            return;
        failAndReset(text);
    });
    connect(tcp, &TcpMgr::sig_connection_closed, this, [this] {
        if (!_want_connection) {
            //主动退出/被顶号：连接关闭是预期结果，不能触发重连
            _connecting = false;
            return;
        }
        if (_connecting) {
            //登录帧还没回来就断了：首次登录交给登录页，重连则进入下一次退避
            _connecting = false;
            if (_reconnecting)
                scheduleReconnect(QStringLiteral("聊天服务器在登录完成前断开了连接"));
            else
                failAndReset(QStringLiteral("聊天服务器在登录完成前断开了连接"));
            return;
        }
        if (!_ready)
            return;
        _ready = false;
        _heartbeat.stop();
        _heartbeat_watchdog.stop();
        UserMgr::GetInstance()->FailPendingMessages();
        FileService::GetInstance()->CloseResource(); //聊天断开后资源连接也不保留
        //一次新的断线从第 1 次重试重新计数
        _reconnect_attempt = 0;
        scheduleReconnect(QStringLiteral("与服务器的连接已断开"));
    });
    connect(tcp, &TcpMgr::sig_notify_offline, this, [this] {
        //被顶号/登录过期：旧 token 再登录会把新会话顶回去，绝不能自动重连
        _want_connection = false;
        _reconnecting = false;
        _reconnect_attempt = 0;
        _reconnect_timer.stop();
        _timeout.stop();
        if (_connecting) {
            _connecting = false;
            failAndReset(QStringLiteral("登录期间会话被其他终端替换，请重试"));
            return;
        }
        _ready = false;
        _heartbeat.stop();
        _heartbeat_watchdog.stop();
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
    _want_connection = true;
    _reconnecting = false;
    _reconnect_attempt = 0;
    _reconnect_timer.stop();
    _timeout.stop();
    _heartbeat.stop();
    _heartbeat_watchdog.stop();
    _ready = false;
    _current = {};
    _history_pages = 0;
    _seen_threads.clear();
    _threads_complete = false;
    UserMgr::GetInstance()->ClearSession(); //内部会关闭上一个账号的本地库

    //先开库并恢复缓存：网络回来之前会话列表与历史就已经可读，
    //2002 到达后只做一次合并，不必等每个会话的第一页消息
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
    _want_connection = false;
    _reconnecting = false;
    _reconnect_attempt = 0;
    _reconnect_timer.stop();
    _heartbeat_watchdog.stop();
    if (_logging_out)
        return;
    if (!_ready) {
        closeSession();
        return;
    }

    _logging_out = true;
    const QByteArray payload =
        QJsonDocument(QJsonObject{}).toJson(QJsonDocument::Compact);
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
    _want_connection = false;
    _reconnecting = false;
    _reconnect_attempt = 0;
    _reconnect_timer.stop();
    _logout_timer.stop();
    _logging_out = false;
    _timeout.stop();
    _heartbeat.stop();
    _heartbeat_watchdog.stop();
    _ready = false;
    _connecting = false;
    _current = {};
    _pending = {};
    Log::SetUser(0);
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
        QJsonDocument(frame).toJson(QJsonDocument::Compact);

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
    case server_error::TokenInvalid: text = QStringLiteral("登录凭证已失效，请重新登录"); break;
    case server_error::UidInvalid: text = QStringLiteral("账号不存在或已被注销"); break;
    case server_error::ServiceUnavailable: text = QStringLiteral("登录服务暂时不可用，请稍后重试"); break;
    case server_error::InstanceUnknown: text = QStringLiteral("聊天服务正在启动，请稍后重试"); break;
    case server_error::TooManyRequests: text = QStringLiteral("登录过于频繁，请稍后再试"); break;
    default: text = QStringLiteral("登录失败，错误码 %1").arg(code); break;
    }
    if (_reconnecting) {
        _connecting = false;
        //凭证失效/账号不存在是永久错误；其余（服务启动中、过于频繁）继续退避
        const bool permanent = code == server_error::TokenInvalid
            || code == server_error::UidInvalid;
        scheduleReconnect(text, permanent);
        return;
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
    const bool was_reconnecting = _reconnecting;
    auto *mgr = UserMgr::GetInstance().get();
    //之后每行日志都带 uid，支持同学不必再按登录时间对账
    Log::SetUser(_pending.Uid);
    _history_pages = 0;
    //重连时 2002 会重新下发完整快照：分页状态与"本次出现过的群"必须重新统计，
    //否则上一次登录的 _seen_threads 会把已解散的群误判成还在
    _seen_threads.clear();
    _threads_complete = false;
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
    if (data.value("thread_more").toBool() && !threads.isEmpty()) {
        const auto last = threads.last().toObject();
        loadMoreThreads(last.value("thread_id").toVariant().toLongLong(),
            last.value("time").toVariant().toLongLong());
    }
    else
        finishThreadList();
    _connecting = false;
    _timeout.stop();
    _ready = true;
    _current = _pending;
    _reconnect_attempt = 0;
    _reconnecting = false;
    _reconnect_timer.stop();
    _heartbeat.start();
    _heartbeat_watchdog.start();
    FileService::GetInstance()->OnSessionReady(); //登录完成后再补拉缺失头像
    if (was_reconnecting)
        emit reconnected();
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

// 会话列表翻页：服务端按 (活跃时间, thread_id) 倒序，旧服务的 thread_id 游标仍兼容
void SessionService::loadMoreThreads(qint64 lastThreadId, qint64 lastTime)
{
    if (lastThreadId <= 0)
        return;
    QJsonObject request{{"fromuid", UserMgr::GetInstance()->GetUid()},
                        {"thread_id", lastThreadId}, {"last_time", lastTime}};
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

//连接 + 登录帧的整体超时：首次登录交给登录页，重连时进入下一次退避
void SessionService::onSessionTimeout()
{
    if (_reconnecting) {
        _connecting = false;
        scheduleReconnect(QStringLiteral("连接聊天服务超时"));
        return;
    }
    failAndReset(QStringLiteral("连接聊天服务超时"));
}

//普通断线的统一入口：保留 UserMgr 与本地库，只重建 TCP 并重新登录。
//被顶号/凭证失效/主动退出走 permanent 或 _want_connection=false，不会再重连
void SessionService::scheduleReconnect(const QString &reason, bool permanent)
{
    if (permanent) {
        _reconnecting = false;
        _reconnect_attempt = 0;
        _reconnect_timer.stop();
        _timeout.stop();
        _connecting = false;
        _want_connection = false;
        _heartbeat_watchdog.stop();
        TcpMgr::GetInstance()->CloseConnectionAsync();
        FileService::GetInstance()->CloseResource();
        emit reconnectFailed(reason);
        return;
    }
    if (!_want_connection || _current.Uid <= 0 || _current.Token.isEmpty()) {
        _reconnecting = false;
        _reconnect_attempt = 0;
        _reconnect_timer.stop();
        _timeout.stop();
        _connecting = false;
        _heartbeat_watchdog.stop();
        TcpMgr::GetInstance()->CloseConnectionAsync();
        FileService::GetInstance()->CloseResource();
        emit reconnectFailed(reason.isEmpty() ? QStringLiteral("登录状态已失效") : reason);
        return;
    }
    //同一次失败可能同时收到 con_failed 与 disconnected：只排一次，避免跳号
    if (_reconnect_timer.isActive())
        return;
    if (_reconnect_attempt >= kMaxReconnectAttempts) {
        _reconnecting = false;
        _want_connection = false;
        _connecting = false;
        _timeout.stop();
        _heartbeat.stop();
        _heartbeat_watchdog.stop();
        TcpMgr::GetInstance()->CloseConnectionAsync();
        FileService::GetInstance()->CloseResource();
        emit reconnectFailed(QStringLiteral("自动重连失败，请检查网络后重新登录"));
        return;
    }

    _reconnect_attempt++;
    _reconnecting = true;
    _connecting = false;
    _ready = false;
    _timeout.stop();
    _heartbeat.stop();
    _heartbeat_watchdog.stop();
    const int shift = qMin(_reconnect_attempt - 1, 4);
    const int delay = qMin(kReconnectBaseMs << shift, kReconnectMaxMs);
    emit reconnecting(_reconnect_attempt);
    _reconnect_timer.start(delay);
}

//真正发起一次重连：复用当前 token，不清 UserMgr/本地库
void SessionService::startReconnectAttempt()
{
    if (!_reconnecting || !_want_connection)
        return;
    if (_current.Uid <= 0 || _current.Token.isEmpty()) {
        scheduleReconnect(QStringLiteral("登录状态已失效"), true);
        return;
    }

    _pending = _current;
    _connecting = true;
    _timeout.start();
    TcpMgr::GetInstance()->ConnectToServer(_pending);
    FileService::GetInstance()->ConnectToResource(_pending); //资源连接不阻塞聊天重连
}

// 统一失败出口：停表、复位状态，把原因抛给界面
void SessionService::failAndReset(const QString &msg)
{
    closeSession();
    emit connectFailed(msg);
}
