#include "log.h"
#include "tcpmgr.h"
#include <QAbstractSocket>
#include <QDebug>
#include <QSslError>
#include <QJsonDocument>
#include <limits>
#include "usermgr.h"

namespace {
//服务端 CSession::Send 用 2 字节长度字段，包体上限 65531；
//两端都按 uint16 处理，超过这个值的响应会被服务端拒绝并回错误包
constexpr quint16 MaxChatFrame = std::numeric_limits<quint16>::max() - 4;
//服务端接收上限（ChatServer MAX_LENGTH）：4 字节头之外最多 8KB 包体
constexpr int MaxChatRequest = 8192;

//帧体不是合法 JSON：协议层异常，带上消息号便于与服务端日志对照
void LogBadBody(ReqId id)
{
    qCWarning(lcNet, "msg %d: body is not a json object", static_cast<int>(id));
}
}

TcpMgr::TcpMgr() : _host(""), _port(0), _b_recv_pending(false), _message_id(0), _message_len(0)
{
    registerMetaType();

    //传输层就绪：明文是 connected，TLS 要等握手完成（encrypted）才算真的连上
    auto on_transport_ready = [this]() {
        qCInfo(lcNet, "chat channel connected");
        _manual_close = false;
        emit sig_con_success(true); //连接成功后发出信号通知界面更新状态
    };
    QObject::connect(&_socket, &QTcpSocket::connected, this, [this, on_transport_ready]() {
        if (tcp_tls_enabled)
            return; //握手在 connected 之后，交给 encrypted 通知
        on_transport_ready();
    });
    QObject::connect(&_socket, &QSslSocket::encrypted, this, on_transport_ready);
    //证书不受信、域名不匹配都会走到这里；QSslSocket 默认在信号之后断开连接
    QObject::connect(&_socket, &QSslSocket::sslErrors, this,
        [this](const QList<QSslError> &errors) {
            const QString reason = errors.isEmpty() ? QStringLiteral("TLS handshake failed")
                                                    : errors.first().errorString();
            qCWarning(lcNet) << "[tcp] tls error:" << reason;
            emit sig_con_failed(reason);
        });

    //连接层面的失败（地址不可达、端口拒绝、超时）此前没有任何出口，
    //界面只能干等；这里把它接出来，让调用方能立即给出提示
    QObject::connect(&_socket, &QAbstractSocket::errorOccurred, this,
        [this](QAbstractSocket::SocketError err) {
            if (err == QAbstractSocket::RemoteHostClosedError)
                return; //对端正常断开由 sig_connection_closed 处理
            qCWarning(lcNet) << "[tcp] connect error:" << err << _socket.errorString();
            emit sig_con_failed(_socket.errorString());
        });

    QObject::connect(&_socket, &QTcpSocket::readyRead, this, [this]() {
        //当有数据可读时，读取所有数据并追加到缓冲区
        _buffer.append(_socket.readAll());

        forever {
            //先解析头部
            if(!_b_recv_pending){
                //检查缓冲区中的数据是否足够解析出一个消息头(消息ID + 消息长度)
                if (_buffer.size() < static_cast<int>(sizeof(quint16) * 2))
                    return; //数据不够，等待更多数据

                //每次解析头部都新建 stream：QDataStream 的读取位置是它自己的内部状态，
                //不会因为 _buffer 被消费而回到 0；复用同一个对象会让第二帧从错误偏移开始读
                QDataStream stream(_buffer.left(sizeof(quint16) * 2));
                stream.setByteOrder(QDataStream::BigEndian);
                stream >> _message_id >> _message_len;
                _buffer.remove(0, sizeof(quint16) * 2); //原地摘掉头部，比 mid 重新分配更省

                //空包或超长说明字节流已经错位，继续等包体只会把后续帧一起吞掉；
                //清掉半包状态并断开，让登录流程重新建连
                if (_message_len == 0 || _message_len > MaxChatFrame) {
                    qCWarning(lcNet) << "[tcp] invalid frame length" << _message_len;
                    resetRecvState();
                    _socket.abort(); //disconnected 会走统一清理并通知会话层
                    return;
                }
            }

            //buffer剩余长读是否满足消息体长度，不满足则退出继续等待接受
            if(_buffer.size() < _message_len){
                _b_recv_pending = true; //设置标志，表示正在等待接收完整消息
                return;
            }

            _b_recv_pending = false; //重置标志，准备接收下一条消息
            //读取消息体
            const QByteArray messageBody = _buffer.left(_message_len);
            _buffer.remove(0, _message_len); //原地移除已处理的包体，剩余数据继续解析
            //调用消息处理函数，根据消息ID分发到对应的处理函数
            handleMsg(ReqId(_message_id), _message_len, messageBody);
        }

    });

    //处理连接断开
    QObject::connect(&_socket, &QTcpSocket::disconnected, this, [this]() {
        qCInfo(lcNet, "chat channel disconnected");
        resetRecvState(); //旧连接可能停在半包，重连后不能把残留字节拼到新帧头
        _send_queue.clear();
        _current_block.clear();
        _bytes_sent = 0;
        _pending = false;
        if (_manual_close) {
            _manual_close = false;
            return;
        }
        emit sig_connection_closed(); //并且发送通知到界面
        });

    //连接发送信号用来发送数据
    QObject::connect(this, &TcpMgr::sig_send_data, this, &TcpMgr::slot_send_data);
    //连接请求与关闭请求也交给本对象所在线程执行
    QObject::connect(this, &TcpMgr::sig_connect_tcp, this, &TcpMgr::slot_tcp_connect);
    QObject::connect(this, &TcpMgr::sig_close_connection, this, &TcpMgr::CloseConnection);

    //Qt的socket非阻塞，缓冲区满时write只写出一部分；由bytesWritten续写剩余部分
    QObject::connect(&_socket, &QTcpSocket::bytesWritten, this, [this](qint64 bytes) {
        _bytes_sent += bytes;
        if (_bytes_sent < _current_block.size()) {
            _socket.write(_current_block.mid(_bytes_sent));
            return;
        }

        if (_send_queue.isEmpty()) {
            _current_block.clear();
            _bytes_sent = 0;
            _pending = false;
            return;
        }

        _current_block = _send_queue.dequeue();
        _bytes_sent = 0;
        _pending = true;
        _socket.write(_current_block);
    });

    initHandlers(); //注册消息
}

void TcpMgr::registerMetaType()
{
    //跨线程队列连接需要运行时打包/解包参数，自定义类型必须注册
    qRegisterMetaType<ServerInfo>("ServerInfo");
    qRegisterMetaType<ReqId>("ReqId");
    qRegisterMetaType<std::shared_ptr<SearchInfo>>("std::shared_ptr<SearchInfo>");
    qRegisterMetaType<std::shared_ptr<AddFriendApply>>("std::shared_ptr<AddFriendApply>");
    qRegisterMetaType<std::shared_ptr<AuthInfo>>("std::shared_ptr<AuthInfo>");
    qRegisterMetaType<std::shared_ptr<AuthRsp>>("std::shared_ptr<AuthRsp>");
    qRegisterMetaType<std::shared_ptr<TextChatMsg>>("std::shared_ptr<TextChatMsg>");
}

void TcpMgr::MoveToThread(QThread *thread)
{
    if (!thread)
        return;
    //moveToThread不会带走没有父子关系的成员socket，必须单独迁移，否则socket仍在主线程
    moveToThread(thread);
    _socket.moveToThread(thread);
}

void TcpMgr::ConnectToServer(const ServerInfo &info)
{
    emit sig_connect_tcp(info);
}

void TcpMgr::CloseConnectionAsync()
{
    emit sig_close_connection();
}

void TcpMgr::resetRecvState()
{
    _buffer.clear(); //半包数据属于上一条流，不能带到下一次解析
    _b_recv_pending = false; //回到等待4字节头部的状态
    _message_id = 0;
    _message_len = 0;
}

void TcpMgr::CloseConnection(){
    _manual_close = true;
    _socket.abort();
    resetRecvState();
    _send_queue.clear();
    _current_block.clear();
    _bytes_sent = 0;
    _pending = false;
}

TcpMgr::~TcpMgr(){

}

void TcpMgr::initHandlers()
{
    //shared_from_this()只能在对象已经完全构建之后调用
    //auto self = shared_from_this();

    //注册处理登录响应的消息处理函数
    _handlers.insert(ID_CHAT_LOGIN_RSP, [this](ReqId id, int len, QByteArray data){
        Q_UNUSED(len); //表示这个参数在函数体内未使用，避免编译器警告

        QJsonDocument jsonDoc = QJsonDocument::fromJson(data); //将QByteArray转换为QJsonDocument

        //检查转换是否成功
        if(jsonDoc.isNull()){
            LogBadBody(id);
            return;
        }

        QJsonObject jsonObj = jsonDoc.object(); //从QJsonDocument中提取QJsonObject

        //如果JSON对象中不包含"error"字段，说明服务器返回的数据格式不正确
        if(!jsonObj.contains("error")){
            qCWarning(lcNet, "login rsp without error field");
            emit sig_login_failed(ErrorCodes::ERR_JSON); //回包不完整按协议错误处理
            return;
        }

        int err = jsonObj["error"].toInt();
        //如果服务器返回的错误码不为0，说明登录失败
        if(err != ErrorCodes::SUCCESS){
            qCWarning(lcNet, "login failed, error=%d", err);
            emit sig_login_failed(err); //发出登录失败的信号，参数为错误码
            return;
        }

        emit sig_login_success(jsonObj);
    });

    auto emitJson = [this](ReqId expected, auto signal) {
        _handlers.insert(expected, [this, signal](ReqId id, int len, QByteArray data) {
            Q_UNUSED(id);
            Q_UNUSED(len);
            const auto document = QJsonDocument::fromJson(data);
            if (!document.isObject())
                return;
            const auto object = document.object();
            if (object.value("error").toInt() == ErrorCodes::SUCCESS)
                emit (this->*signal)(object);
        });
    };
    emitJson(ID_LOAD_CHAT_THREAD_RSP, &TcpMgr::sig_chat_threads);
    emitJson(ID_LOAD_CHAT_MSG_RSP, &TcpMgr::sig_chat_messages);
    //图片回执成功失败都要回传：成功后启动资源上传，失败要把消息标成发送失败
    _handlers.insert(ID_IMG_CHAT_MSG_RSP, [this](ReqId, int, QByteArray data) {
        const auto document = QJsonDocument::fromJson(data);
        if (document.isObject())
            emit sig_chat_img_rsp(document.object());
    });
    //图片已上传完成的推送：插入接收方消息，本地没有压缩图时再发起下载
    _handlers.insert(ID_NOTIFY_IMG_CHAT_MSG_REQ, [this](ReqId, int, QByteArray data) {
        const auto document = QJsonDocument::fromJson(data);
        if (document.isObject())
            emit sig_image_notify(document.object());
    });
    //视频/文件回执与上传完成通知：成功后才上传资源，通知只建卡片不自动下载
    emitJson(ID_RESOURCE_CHAT_MSG_RSP, &TcpMgr::sig_resource_chat_rsp);
    emitJson(ID_NOTIFY_RESOURCE_CHAT_MSG_REQ, &TcpMgr::sig_resource_notify);
    //创建会话失败也要回传，客户端据此清除 pending 并允许重试
    _handlers.insert(ID_CREATE_PRIVATE_CHAT_RSP, [this](ReqId, int, QByteArray data) {
        const auto document = QJsonDocument::fromJson(data);
        if (document.isObject())
            emit sig_private_chat_created(document.object());
    });

    //资料与备注修改：成功失败都要交给业务层，界面要按 error 给出提示
    _handlers.insert(ID_UPDATE_PROFILE_RSP, [this](ReqId, int, QByteArray data) {
        const auto document = QJsonDocument::fromJson(data);
        if (document.isObject())
            emit sig_profile_updated(document.object());
    });
    _handlers.insert(ID_UPDATE_FRIEND_REMARK_RSP, [this](ReqId, int, QByteArray data) {
        const auto document = QJsonDocument::fromJson(data);
        if (document.isObject())
            emit sig_friend_remark_updated(document.object());
    });
    //按 uid 拉最新资料：昵称与签名随时会变，打开会话或资料页时刷新一次
    _handlers.insert(ID_GET_PROFILE_RSP, [this](ReqId, int, QByteArray data) {
        const auto document = QJsonDocument::fromJson(data);
        if (document.isObject())
            emit sig_profile_loaded(document.object());
    });
    //好友资料变更推送：昵称、签名或头像改了，在线好友立即覆盖本地缓存
    _handlers.insert(ID_NOTIFY_PROFILE_CHANGED, [this](ReqId, int, QByteArray data) {
        const auto document = QJsonDocument::fromJson(data);
        if (document.isObject())
            emit sig_friend_profile_changed(document.object());
    });
    //已读上报回包：只是确认，客户端不需要额外动作
    _handlers.insert(ID_MARK_READ_RSP, [this](ReqId, int, QByteArray data) {
        const auto document = QJsonDocument::fromJson(data);
        if (document.isObject())
            emit sig_mark_read_rsp(document.object());
    });
    //对端已读：把这条会话里自己发的、消息号不超过它的消息标成已读
    _handlers.insert(ID_NOTIFY_MSG_READ, [this](ReqId, int, QByteArray data) {
        const auto document = QJsonDocument::fromJson(data);
        if (document.isObject())
            emit sig_message_read(document.object());
    });
    //撤回回包：成功后本端也要把气泡换成提示
    _handlers.insert(ID_RECALL_MSG_RSP, [this](ReqId, int, QByteArray data) {
        const auto document = QJsonDocument::fromJson(data);
        if (document.isObject())
            emit sig_recall_rsp(document.object());
    });
    _handlers.insert(ID_NOTIFY_MSG_RECALLED, [this](ReqId, int, QByteArray data) {
        const auto document = QJsonDocument::fromJson(data);
        if (document.isObject())
            emit sig_message_recalled(document.object());
    });
    //删好友回包：失败也交给业务层，界面要给出原因
    _handlers.insert(ID_DELETE_FRIEND_RSP, [this](ReqId, int, QByteArray data) {
        const auto document = QJsonDocument::fromJson(data);
        if (document.isObject())
            emit sig_delete_friend_rsp(document.object());
    });

    //群聊回包：失败要带原因，不能用只转发成功结果的辅助函数
    _handlers.insert(ID_CREATE_GROUP_RSP, [this](ReqId, int, QByteArray data) {
        const auto document = QJsonDocument::fromJson(data);
        if (document.isObject())
            emit sig_group_created(document.object());
    });
    _handlers.insert(ID_GET_GROUP_INFO_RSP, [this](ReqId, int, QByteArray data) {
        const auto document = QJsonDocument::fromJson(data);
        if (document.isObject())
            emit sig_group_info(document.object());
    });
    _handlers.insert(ID_UPDATE_GROUP_RSP, [this](ReqId, int, QByteArray data) {
        const auto document = QJsonDocument::fromJson(data);
        if (document.isObject())
            emit sig_group_updated(document.object());
    });
    _handlers.insert(ID_GROUP_MEMBER_OP_RSP, [this](ReqId, int, QByteArray data) {
        const auto document = QJsonDocument::fromJson(data);
        if (document.isObject())
            emit sig_group_member_op(document.object());
    });
    //群资料/成员/被移出/解散的推送：所有在线成员都会收到同一份 JSON
    _handlers.insert(ID_NOTIFY_GROUP_CHANGED, [this](ReqId, int, QByteArray data) {
        const auto document = QJsonDocument::fromJson(data);
        if (document.isObject())
            emit sig_group_changed(document.object());
    });

    //注册处理用户搜索响应的消息处理函数
    _handlers.insert(ID_SEARCH_USER_RSP, [this](ReqId id, int len, QByteArray data){
        Q_UNUSED(len); //表示这个参数在函数体内未使用，避免编译器警告

        QJsonDocument jsonDoc = QJsonDocument::fromJson(data); //将QByteArray转换为QJsonDocument

        //检查转换是否成功
        if(jsonDoc.isNull()){
            LogBadBody(id);
            return;
        }

        QJsonObject jsonObj = jsonDoc.object(); //从QJsonDocument中提取QJsonObject

        //如果JSON对象中不包含"error"字段，说明服务器返回的数据格式不正确
        if(!jsonObj.contains("error")){
            qCWarning(lcNet, "search rsp without error field");
            emit sig_user_search(nullptr); //发出用户搜索结果的信号，参数为nullptr表示搜索失败
            return;
        }

        int err = jsonObj["error"].toInt();
        //如果服务器返回的错误码不为0，说明登录失败
        if(err != ErrorCodes::SUCCESS){
            qCWarning(lcNet, "search failed, error=%d", err);
            emit sig_user_search(nullptr); //发出用户搜索结果的信号，参数为nullptr表示搜索失败
            return;
        }

        //从JSON对象中提取用户信息并创建一个SearchInfo对象的智能指针
        //relation 由服务端计算（两行好友关系 + 待处理申请），客户端不再用本地缓存判断
        auto search_info = std::make_shared<SearchInfo>(jsonObj["uid"].toInt(),
            jsonObj["name"].toString(), jsonObj["nick"].toString(),
            jsonObj["desc"].toString(), jsonObj["sex"].toInt(), jsonObj["icon"].toString(),
            jsonObj.contains("relation") ? jsonObj["relation"].toInt() : -1);

        emit sig_user_search(search_info); //发出用户搜索结果的信号
    });

    //注册处理添加好友响应的消息处理函数
    //失败回包同样要交给上层，界面才能提示“申请没有发出去”
    _handlers.insert(ID_ADD_FRIEND_RSP, [this](ReqId id, int len, QByteArray data) {
        Q_UNUSED(len);

        const QJsonDocument jsonDoc = QJsonDocument::fromJson(data);
        if (!jsonDoc.isObject()) {
            qCWarning(lcNet, "add-friend rsp without error field");
            emit sig_add_friend_rsp(QJsonObject{{QStringLiteral("error"), ErrorCodes::ERR_JSON}});
            return;
        }
        emit sig_add_friend_rsp(jsonDoc.object());
    });

    //注册处理添加好友申请通知的消息处理函数
    _handlers.insert(ID_NOTIFY_ADD_FRIEND_REQ, [this](ReqId id, int len, QByteArray data) {
        Q_UNUSED(len); //表示这个参数在函数体内未使用，避免编译器警告

        QJsonDocument jsonDoc = QJsonDocument::fromJson(data); //将QByteArray转换为QJsonDocument

        //检查转换是否成功
        if (jsonDoc.isNull()) {
            LogBadBody(id);
            return;
        }

        QJsonObject jsonObj = jsonDoc.object(); //从QJsonDocument中提取QJsonObject

         //如果JSON对象中不包含"error"字段，说明服务器返回的数据格式不正确
        if (!jsonObj.contains("error")) {
            qCWarning(lcNet, "add-friend notify without error field");
            return;
        }

        int err = jsonObj["error"].toInt();
        //如果服务器返回的错误码不为0，说明添加好友申请通知失败
        if (err != ErrorCodes::SUCCESS) {
            qCWarning(lcNet, "add-friend notify error=%d", err);
            return;
        }

        //从JSON对象中提取好友申请信息并创建一个AddFriendApply对象的智能指针
        int from_uid = jsonObj["applyuid"].toInt();
        QString name = jsonObj["name"].toString();
        QString desc = jsonObj["desc"].toString();
        QString icon = jsonObj["icon"].toString();
        QString nick = jsonObj["nick"].toString();
        int sex = jsonObj["sex"].toInt();
        auto apply_info = std::make_shared<AddFriendApply>(
            from_uid, name, desc, icon, nick, sex);

        emit sig_friend_apply(apply_info); //发出好友申请通知的信号
    });

    //注册处理好友申请认证通知的消息处理函数(申请人同意了我的好友申请))
    _handlers.insert(ID_NOTIFY_AUTH_FRIEND_REQ, [this](ReqId id, int len, QByteArray data) {
        Q_UNUSED(len);
        // 将QByteArray转换为QJsonDocument
        QJsonDocument jsonDoc = QJsonDocument::fromJson(data);

        // 检查转换是否成功
        if (jsonDoc.isNull()) {
            LogBadBody(id);
            return;
        }

        QJsonObject jsonObj = jsonDoc.object();
        if (!jsonObj.contains("error")) {
            qCWarning(lcNet, "auth-friend notify without error field");
            return;
        }

        int err = jsonObj["error"].toInt();
        if (err != ErrorCodes::SUCCESS) {
            qCWarning(lcNet, "auth-friend notify error=%d", err);
            return;
        }

        int from_uid = jsonObj["fromuid"].toInt();
        QString name = jsonObj["name"].toString();
        QString nick = jsonObj["nick"].toString();
        QString icon = jsonObj["icon"].toString();
        int sex = jsonObj["sex"].toInt();

        //3008 是"对方通过我的申请"：用 AuthRsp 交给会话层，走和 3007 同一套装配，
        //否则通过语不会计未读、顶部关系提示也不会清
        auto auth_info = std::make_shared<AuthRsp>(from_uid, name, nick, icon, sex,
            jsonObj["thread_id"].toVariant().toLongLong());
        //申请时填过备注和标签的话，通过后立刻能按备注显示、按分组筛选，不必重新登录
        auth_info->_back = jsonObj["back"].toString();
        for (const auto &value : jsonObj["tags"].toArray()) {
            const QString tag = value.toString();
            if (!tag.isEmpty() && !auth_info->_tags.contains(tag))
                auth_info->_tags.append(tag);
        }
        for (const auto &value : jsonObj["welcome_msgs"].toArray()) {
            const auto item = value.toObject();
            auto welcome = std::make_shared<TextChatData>(
                item["msgid"].toString(), item["content"].toString(),
                item["fromuid"].toInt(from_uid), item["touid"].toInt(UserMgr::GetInstance()->GetUid()),
                item["message_id"].toVariant().toLongLong(),
                jsonObj["thread_id"].toVariant().toLongLong(),
                static_cast<ChatMessageStatus>(item["status"].toInt(3)));
            //欢迎消息也带落库时间：加好友那一刻就是它们的时间，聊天里正常显示时间条
            welcome->_time = item["time"].toVariant().toLongLong();
            auth_info->_welcome_msgs.push_back(welcome);
        }

        emit sig_auth_rsp(auth_info);
    });

    //注册处理好友申请认证通知的消息处理函数(我同意了对方的好友申请，对方收到通知)
    _handlers.insert(ID_AUTH_FRIEND_RSP, [this](ReqId id, int len, QByteArray data) {
        Q_UNUSED(len);
        // 将QByteArray转换为QJsonDocument
        QJsonDocument jsonDoc = QJsonDocument::fromJson(data);

        // 检查转换是否成功
        if (jsonDoc.isNull()) {
            LogBadBody(id);
            return;
        }

        QJsonObject jsonObj = jsonDoc.object();

        if (!jsonObj.contains("error")) {
            qCWarning(lcNet, "auth-friend without error field");
            return;
        }

        int err = jsonObj["error"].toInt();
        if (err != ErrorCodes::SUCCESS) {
            qCWarning(lcNet, "auth-friend failed, error=%d", err);
            return;
        }

        auto name = jsonObj["name"].toString();
        auto nick = jsonObj["nick"].toString();
        auto icon = jsonObj["icon"].toString();
        auto sex = jsonObj["sex"].toInt();
        auto uid = jsonObj["uid"].toInt();
        auto rsp = std::make_shared<AuthRsp>(uid, name, nick, icon, sex,
            jsonObj["thread_id"].toVariant().toLongLong());
        //同意时填写的备注随回包返回，本地好友列表立即使用
        rsp->_back = jsonObj["back"].toString();
        for (const auto &value : jsonObj["welcome_msgs"].toArray()) {
            const auto item = value.toObject();
            auto welcome = std::make_shared<TextChatData>(
                item["msgid"].toString(), item["content"].toString(),
                item["fromuid"].toInt(uid), item["touid"].toInt(UserMgr::GetInstance()->GetUid()),
                item["message_id"].toVariant().toLongLong(),
                jsonObj["thread_id"].toVariant().toLongLong(),
                static_cast<ChatMessageStatus>(item["status"].toInt(1)));
            //欢迎消息也带落库时间：加好友那一刻就是它们的时间，聊天里正常显示时间条
            welcome->_time = item["time"].toVariant().toLongLong();
            rsp->_welcome_msgs.push_back(welcome);
        }
        emit sig_auth_friend_rsp(rsp);

    });

    _handlers.insert(ID_TEXT_CHAT_MSG_RSP, [this](ReqId id, int len, QByteArray data) {
        Q_UNUSED(len);
        // 将QByteArray转换为QJsonDocument
        QJsonDocument jsonDoc = QJsonDocument::fromJson(data);

        // 检查转换是否成功
        if (jsonDoc.isNull()) {
            LogBadBody(id);
            return;
        }

        QJsonObject jsonObj = jsonDoc.object();

        if (!jsonObj.contains("error")) {
            //缺 error 字段说明服务端回包不完整：按协议错误处理并通知上层，
            //否则这条消息的气泡会一直停在"发送中"
            qCWarning(lcNet, "send-msg rsp without error field");
            QJsonObject failed = jsonObj;
            failed.insert(QStringLiteral("error"), ErrorCodes::ERR_JSON);
            emit sig_chat_msg_failed(failed);
            return;
        }

        int err = jsonObj["error"].toInt();
        if (err != ErrorCodes::SUCCESS) {
            qCWarning(lcNet, "send-msg failed, error=%d", err);
            emit sig_chat_msg_failed(jsonObj);
            return;
        }

        const auto message = std::make_shared<TextChatMsg>(jsonObj["fromuid"].toInt(),
            jsonObj["touid"].toInt(), jsonObj["text_array"].toArray(),
            jsonObj["thread_id"].toVariant().toLongLong());
        emit sig_text_chat_msg(message);
    });

    _handlers.insert(ID_NOTIFY_TEXT_CHAT_MSG_REQ, [this](ReqId id, int len, QByteArray data) {
        Q_UNUSED(len);
        // 将QByteArray转换为QJsonDocument
        QJsonDocument jsonDoc = QJsonDocument::fromJson(data);

        // 检查转换是否成功
        if (jsonDoc.isNull()) {
            LogBadBody(id);
            return;
        }

        QJsonObject jsonObj = jsonDoc.object();

        if (!jsonObj.contains("error")) {
            qCWarning(lcNet, "msg notify without error field");
            return;
        }

        int err = jsonObj["error"].toInt();
        if (err != ErrorCodes::SUCCESS) {
            qCWarning(lcNet, "msg notify error=%d", err);
            return;
        }

        auto msg_ptr = std::make_shared<TextChatMsg>(jsonObj["fromuid"].toInt(),
            jsonObj["touid"].toInt(),jsonObj["text_array"].toArray(),
            jsonObj["thread_id"].toVariant().toLongLong(),
            jsonObj.value("thread_type").toInt(1));
        emit sig_text_chat_msg(msg_ptr);
    });

    _handlers.insert(ID_NOTIFY_OFF_LINE_REQ,[this](ReqId id, int len, QByteArray data){
        Q_UNUSED(len);
        // 将QByteArray转换为QJsonDocument
        QJsonDocument jsonDoc = QJsonDocument::fromJson(data);

        // 检查转换是否成功
        if (jsonDoc.isNull()) {
            LogBadBody(id);
            return;
        }

        QJsonObject jsonObj = jsonDoc.object();

        if (!jsonObj.contains("error")) {
            qCWarning(lcNet, "offline notify without error field");
            return;
        }

        int err = jsonObj["error"].toInt();
        if (err != ErrorCodes::SUCCESS) {
            qCWarning(lcNet, "offline notify error=%d", err);
            return;
        }

        auto uid = jsonObj["uid"].toInt();
        const int reason = jsonObj.value("reason").toInt(1);
        qCInfo(lcNet, "kicked by new login, uid=%d reason=%d", uid, reason);
        //断开连接并通知界面；异地登录必须由界面提示用户，不能静默重连把新会话顶掉
        emit sig_notify_offline(reason);
    });

    _handlers.insert(ID_LOGOUT_RSP,[this](ReqId id, int len, QByteArray data){
        Q_UNUSED(id);
        Q_UNUSED(len);
        Q_UNUSED(data);
        // 收到回包只通知会话层收尾，界面已经回到登录页
        emit sig_logout_rsp();
    });

    _handlers.insert(ID_HEARTBEAT_RSP,[this](ReqId id, int len, QByteArray data){
        Q_UNUSED(len);
        // 将QByteArray转换为QJsonDocument
        QJsonDocument jsonDoc = QJsonDocument::fromJson(data);

        // 检查转换是否成功
        if (jsonDoc.isNull()) {
            LogBadBody(id);
            return;
        }

        QJsonObject jsonObj = jsonDoc.object();

        if (!jsonObj.contains("error")) {
            qCWarning(lcNet, "heartbeat rsp without error field");
            return;
        }

        int err = jsonObj["error"].toInt();
        if (err != ErrorCodes::SUCCESS) {
            qCWarning(lcNet, "heartbeat error=%d", err);
            return;
        }

        emit sig_heartbeat_ok();
    });
}

void TcpMgr::handleMsg(ReqId id, int len, QByteArray data)
{
    auto find_iter =  _handlers.find(id); //在_handlers中查找对应消息ID的处理函数
    //如果没有找到对应的处理函数，输出调试信息并返回
    if(find_iter == _handlers.end()){
        qCWarning(lcNet, "no handler for msg %d", static_cast<int>(id));
        return;
    }

    find_iter.value()(id, len, data); //调用找到的处理函数，传入消息ID、消息长度和消息数据
}

void TcpMgr::slot_tcp_connect(ServerInfo info)
{
    qCDebug(lcNet, "tcp connect requested");
    _host = info.Host; //服务器地址
    _port = static_cast<uint16_t>(info.Port.toUInt()); //服务器端口
    CloseConnection();
    if (tcp_tls_enabled)
        _socket.connectToHostEncrypted(_host, _port); //加密连接：握手完成后才发 encrypted
    else
        _socket.connectToHost(_host, _port); //连接到服务器(异步)
}

void TcpMgr::slot_send_data(ReqId reqId, QByteArray dataBytes)
{
    if (_socket.state() != QAbstractSocket::ConnectedState || dataBytes.size() > MaxChatRequest) {
        qCWarning(lcNet) << "Cannot send frame: disconnected or payload exceeds" << MaxChatRequest << "bytes";
        return;
    }

    uint16_t id = reqId; //将ReqId转换为uint16_t类型的消息ID
    quint16 len = static_cast<quint16>(dataBytes.size()); //计算长度(使用网络字节序转换)

    //创建一个QByteArray用于存储要发送的所有数据
    QByteArray block;
    QDataStream out(&block, QIODevice::WriteOnly);

    //设置数据流使用网络字节序
    out.setByteOrder(QDataStream::BigEndian);
    out << id << len; //写入ID和长度
    block.append(dataBytes); //添加字符串数据

    //已有数据在发送时先入队，等bytesWritten把当前块发完再取
    if (_pending) {
        _send_queue.enqueue(block);
        return;
    }

    _current_block = block;
    _bytes_sent = 0;
    _pending = true;
    _socket.write(_current_block);
}
