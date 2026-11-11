#include "filetcpmgr.h"

#include <QAbstractSocket>
#include <QDataStream>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>

#include "usermgr.h"

namespace {
constexpr int FileHeadLen = 6; //2字节消息ID + 4字节包体长度
constexpr int MaxFileLen = 2048; //与ResourceServer的MAX_FILE_LEN一致，两端切片必须同尺寸
constexpr int MaxFileFrame = 16 * 1024; //与ResourceServer的MAX_MESSAGE_LEN一致
constexpr int ResourceFileNotExists = 1012; //服务器没有这次上传的进度，需要从第一片重来

QJsonObject parseObject(const QByteArray &data)
{
    const QJsonDocument document = QJsonDocument::fromJson(data);
    return document.isObject() ? document.object() : QJsonObject{};
}
}

FileTcpMgr::FileTcpMgr()
{
    registerMetaType(); //ReqId要跨线程排队

    //连接成功只负责通知业务层，具体何时登录、何时拉头像由 FileService 决定
    QObject::connect(&_socket, &QTcpSocket::connected, this, [this]() {
        qDebug() << "[file-tcp] connected";
        emit sig_con_success(true); //交给FileService决定是否开始同步
    });

    //RemoteHostClosedError 是对端正常断开，交给 disconnected 处理，避免重复报错
    QObject::connect(&_socket, &QAbstractSocket::errorOccurred, this,
        [this](QAbstractSocket::SocketError err) {
            if (err == QAbstractSocket::RemoteHostClosedError)
                return; //正常断开不算连接失败
            qWarning() << "[file-tcp] connect error:" << err << _socket.errorString();
            emit sig_con_failed(_socket.errorString()); //界面显示具体原因
        });

    //接收循环：先读6字节头，再按4字节长度等包体；_b_recv_pending 表示头已解析、包体未齐
    QObject::connect(&_socket, &QTcpSocket::readyRead, this, [this]() {
        _buffer.append(_socket.readAll()); //TCP可能一次给半包，先全收进缓冲
        forever {
            if (!_b_recv_pending) {
                if (_buffer.size() < FileHeadLen)
                    return; //头都没收齐，等下一次readyRead

                QDataStream stream(_buffer.left(FileHeadLen)); //只读头部，不消费缓冲
                stream.setByteOrder(QDataStream::BigEndian); //与服务器网络序一致
                stream >> _message_id >> _message_len; //2字节ID + 4字节长度
                _buffer = _buffer.mid(FileHeadLen); //把头部从缓冲摘掉

                //资源帧允许2KB分片，长度字段是4字节；超过上限说明流已经错位，直接断开
                if (_message_len == 0 || _message_len > MaxFileFrame) {
                    qWarning() << "[file-tcp] invalid frame length" << _message_len;
                    slot_close_connection();
                    return;
                }
            }

            if (_buffer.size() < static_cast<int>(_message_len)) {
                _b_recv_pending = true; //包体未齐，保留状态等下一批数据
                return;
            }

            //包体收齐：从缓冲区摘掉并交给分发表，然后继续尝试解析下一帧
            _b_recv_pending = false;
            const QByteArray body = _buffer.mid(0, static_cast<int>(_message_len));
            _buffer = _buffer.mid(static_cast<int>(_message_len));
            handleMsg(ReqId(_message_id), static_cast<int>(_message_len), body);
        }
    });

    //断开时丢弃未发完的队列，避免重连后把上一连接的分片发到新连接
    QObject::connect(&_socket, &QTcpSocket::disconnected, this, [this]() {
        _send_queue.clear(); //未发送的帧全部作废
        _current_block.clear(); //正在写的块也不能再续
        _bytes_sent = 0;
        _pending = false;
        emit sig_connection_closed(); //FileService据此把_connected置false
    });

    //外部调用都在主线程，统一通过信号排队到本对象所在线程执行
    QObject::connect(this, &FileTcpMgr::sig_connect_resource,
                     this, &FileTcpMgr::slot_connect_resource); //建连
    QObject::connect(this, &FileTcpMgr::sig_send_data,
                     this, &FileTcpMgr::slot_send_data); //发送
    QObject::connect(this, &FileTcpMgr::sig_close_connection,
                     this, &FileTcpMgr::slot_close_connection); //关闭
    QObject::connect(this, &FileTcpMgr::sig_start_upload,
                     this, &FileTcpMgr::slot_start_upload); //开始上传
    QObject::connect(this, &FileTcpMgr::sig_start_download,
                     this, &FileTcpMgr::slot_start_download); //开始下载

    //发送队列：一次write可能只写出一部分，必须等bytesWritten确认后再写剩余字节
    QObject::connect(&_socket, &QTcpSocket::bytesWritten, this, [this](qint64 bytes) {
        _bytes_sent += bytes; //累计已写出字节
        if (_bytes_sent < _current_block.size()) {
            _socket.write(_current_block.mid(_bytes_sent)); //只补没写完的尾部
            return;
        }

        if (_send_queue.isEmpty()) {
            _current_block.clear(); //当前块完成，队列也空了
            _bytes_sent = 0;
            _pending = false;
            return;
        }

        _current_block = _send_queue.dequeue(); //取下一帧
        _bytes_sent = 0;
        _pending = true;
        _socket.write(_current_block);
    });

    initHandlers();
}

FileTcpMgr::~FileTcpMgr() = default;

void FileTcpMgr::registerMetaType()
{
    qRegisterMetaType<ReqId>("ReqId");
}

void FileTcpMgr::MoveToThread(QThread *thread)
{
    if (!thread)
        return;
    moveToThread(thread);
    _socket.moveToThread(thread);
}

void FileTcpMgr::ConnectToServer(const QString &host, int port)
{
    emit sig_connect_resource(host, port);
}

void FileTcpMgr::SendData(ReqId reqId, const QByteArray &data)
{
    emit sig_send_data(reqId, data);
}

void FileTcpMgr::CloseConnectionAsync()
{
    emit sig_close_connection();
}

void FileTcpMgr::StartUpload(const QString &md5)
{
    emit sig_start_upload(md5);
}

void FileTcpMgr::StartDownload(const QString &name)
{
    emit sig_start_download(name);
}

void FileTcpMgr::slot_connect_resource(QString host, int port)
{
    _host = host; //地址来自登录回包的res_host
    _port = static_cast<quint16>(port); //端口来自res_port，转成16位
    slot_close_connection(); //先清掉上一个账号的缓冲与在途帧
    _socket.connectToHost(_host, _port); //异步建连，结果由connected信号通知
}

void FileTcpMgr::slot_close_connection()
{
    _socket.abort(); //立即丢弃内核缓冲，不等待未发完的分片
    _buffer.clear(); //旧连接的残留数据会让新连接的帧头解析错位
    _b_recv_pending = false; //回到等待6字节头的状态
    _message_id = 0; //清掉半包解析出的ID
    _message_len = 0; //长度也要归零，否则会按旧长度等包体
    _send_queue.clear(); //未发出的帧一律作废
    _current_block.clear(); //正在写的帧也作废
    _bytes_sent = 0; //部分写入计数归零
    _pending = false; //释放发送通道，新连接可重新起一帧
}

void FileTcpMgr::slot_send_data(ReqId reqId, QByteArray dataBytes)
{
    //只在本线程碰socket；未连接或包体超限时直接丢弃，不排队等重连
    if (_socket.state() != QAbstractSocket::ConnectedState || dataBytes.size() > MaxFileFrame) {
        qWarning() << "[file-tcp] cannot send frame" << reqId;
        return;
    }

    QByteArray block; //完整帧 = 2字节ID + 4字节长度 + JSON包体
    QDataStream out(&block, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::BigEndian); //与ResourceServer的网络序一致
    out << static_cast<quint16>(reqId) << static_cast<quint32>(dataBytes.size()); //长度只算包体
    block.append(dataBytes); //包体紧跟头部

    //同一时刻只允许一片在途，服务器按seq推进，乱序会被判为跳号
    if (_pending) {
        _send_queue.enqueue(block); //排到bytesWritten之后再写
        return;
    }

    _current_block = block; //保存整帧，write可能只发出一部分
    _bytes_sent = 0; //已写字节从零开始计
    _pending = true; //占用发送通道
    _socket.write(_current_block); //异步写，剩余字节在bytesWritten里补
}

void FileTcpMgr::sendNextChunk(const QString &md5)
{
    auto info = UserMgr::GetInstance()->GetUploadFile(md5); //按md5取本次上传的本地状态
    if (!info)
        return; //记录已移除说明任务取消，迟到回包不再发片

    //已经到文件尾：可能是暂停前最后一片的回包刚刚到达，直接收尾
    if (info->_trans_size >= info->_total_size) {
        UserMgr::GetInstance()->RemoveUploadFile(md5); //清掉在途记录
        emit sig_upload_finished(info->_name, md5); //通知资料页刷新
        return;
    }

    QFile file(info->_path); //每片重新打开，避免长上传一直占着文件句柄
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "[file-tcp] open upload file failed:" << info->_path;
        emit sig_upload_failed(info->_name, 1); //文件被删或权限变化，按失败收尾
        return;
    }
    if (!file.seek(info->_trans_size)) { //从服务器确认过的偏移继续读
        emit sig_upload_failed(info->_name, 1); //偏移超出文件，说明本地文件被改动
        return;
    }

    const QByteArray buffer = file.read(MaxFileLen); //切片大小必须与服务端一致
    if (buffer.isEmpty()) {
        emit sig_upload_failed(info->_name, 1); //还没到声明的大小却读不到数据
        return;
    }
    file.close(); //读完立刻关闭，不跨片持有句柄

    //服务器要求 seq 与 (trans_size - 本片长度) 对应，序号从1开始
    const int seq = info->_seq + 1; //在服务器已落盘的序号上递增
    const qint64 trans_size = info->_trans_size + buffer.size(); //本片发完后的累计字节
    const bool last = trans_size >= info->_total_size; //最后一片要通知服务器收尾

    //分片内容用 Base64 放进 JSON；token 每片都带，服务器在首片校验后绑定连接
    QJsonObject frame;
    frame["uid"] = info->_uid; //上传者，服务器据此建<uid>目录
    frame["token"] = UserMgr::GetInstance()->GetToken(); //首片校验utoken前缀，后续只比对连接身份
    frame["md5"] = info->_md5; //整文件校验值，也是Redis进度键的一部分
    frame["name"] = info->_name; //资源名，回包按它找回这条记录
    frame["seq"] = seq; //分片序号，跳号会被拒绝
    frame["trans_size"] = static_cast<double>(trans_size); //累计字节，JSON里用double传大整数
    frame["total_size"] = static_cast<double>(info->_total_size); //总大小，首片建FileInfo时要一致
    frame["last"] = last ? 1 : 0; //最后一片才写库并清进度
    frame["data"] = QString::fromLatin1(buffer.toBase64()); //二进制分片转文本，避免JSON转义问题

    //先记账再发送：回包处理函数会以这两个字段为基准继续读下一片
    info->_seq = seq;
    info->_trans_size = trans_size;

    SendData(info->_req_id, QJsonDocument(frame).toJson(QJsonDocument::Compact)); //头像与通用文件只是ID不同
}

void FileTcpMgr::slot_start_upload(QString md5)
{
    auto info = UserMgr::GetInstance()->GetUploadFile(md5); //上传记录由FileService先登记
    if (!info)
        return; //记录被清掉说明任务已取消，不再发片

    info->_seq = 0; //从头开始，服务器seq=1会重新建进度
    info->_trans_size = 0; //本地偏移归零，避免用旧进度读错位置
    sendNextChunk(md5); //发第一片
}

void FileTcpMgr::sendDownloadRequest(const QString &name)
{
    auto info = UserMgr::GetInstance()->GetDownloadFile(name); //按资源名取本地下载状态
    if (!info)
        return; //任务已被取消，迟到回包不再继续要片

    QJsonObject frame; //下载请求不带数据，只声明要哪个文件的第几片
    frame["uid"] = UserMgr::GetInstance()->GetUid(); //当前登录者，用于token校验
    frame["token"] = UserMgr::GetInstance()->GetToken(); //首片绑定连接身份
    frame["owner_uid"] = info->_owner_uid; //文件归属者，服务器据此拼<uid>/<name>路径
    frame["name"] = info->_name; //资源名，回包按它找回这条记录
    frame["seq"] = info->_seq; //期望的分片序号，服务器按偏移读取
    SendData(ID_DOWN_LOAD_FILE_REQ, QJsonDocument(frame).toJson(QJsonDocument::Compact)); //投递到网络线程
}

void FileTcpMgr::slot_start_download(QString name)
{
    sendDownloadRequest(name); //_seq登记时已置1，从第一片开始拉
}

void FileTcpMgr::initHandlers()
{
    //上传回包：服务器已经写盘，trans_size 可以直接作为下一片的起点。
    //头像和通用文件共用同一处理，响应ID不同但字段一致。
    auto uploadResponse = [this](ReqId, int, QByteArray data) {
        const QJsonObject object = parseObject(data);
        if (object.isEmpty()) {
            emit sig_upload_failed(QString(), 1);
            return;
        }

        const int error = object.value("error").toInt(1);
        const QString md5 = object.value("md5").toString();
        auto info = UserMgr::GetInstance()->GetUploadFile(md5);
        const QString name = object.value("name").toString(info ? info->_name : QString());

        //服务器拒绝时不要继续发下一片，交给 FileService 结束本次上传并提示
        if (error != 0) {
            UserMgr::GetInstance()->RemoveUploadFile(md5); //失败后清记录，重发同一内容能从头开始
            emit sig_upload_failed(name, error);
            return;
        }
        if (!info)
            return;

        //以服务器返回值覆盖本地进度：断开重连后本地可能比服务器“多走”了几片
        const qint64 trans_size = object.value("trans_size").toVariant().toLongLong();
        const qint64 total_size = object.value("total_size").toVariant().toLongLong();
        info->_seq = object.value("seq").toInt(info->_seq);
        info->_trans_size = trans_size;
        if (total_size > 0)
            info->_total_size = total_size;

        emit sig_upload_progress(name, info->_trans_size, info->_total_size);

        if (info->_trans_size >= info->_total_size) {
            UserMgr::GetInstance()->RemoveUploadFile(md5);
            emit sig_upload_finished(name, md5);
            return;
        }

        //暂停状态只保留状态，不发下一片；继续时走同步分支
        if (UserMgr::GetInstance()->UploadPaused())
            return;
        sendNextChunk(md5);
    };

    _handlers[ID_UPLOAD_HEAD_ICON_RSP] = uploadResponse;
    _handlers[ID_UPLOAD_FILE_RSP] = uploadResponse;

    //续传同步回包：暂停/重连后先确认服务器实际写到哪一片，再决定从哪继续
    _handlers[ID_SYNC_FILE_RSP] = [this](ReqId, int, QByteArray data) {
        const QJsonObject object = parseObject(data);
        if (object.isEmpty()) {
            emit sig_upload_failed(QString(), 1);
            return;
        }

        const QString md5 = object.value("md5").toString();
        auto info = UserMgr::GetInstance()->GetUploadFile(md5);
        if (!info)
            return;

        const int error = object.value("error").toInt(1);
        if (error == ResourceFileNotExists) {
            //服务器已经丢了这次传输（例如Redis过期），只能从第一片重新开始
            info->_seq = 0;
            info->_trans_size = 0;
        }
        else if (error != 0) {
            emit sig_upload_failed(info->_name, error);
            return;
        }
        else {
            //服务器的seq是已落盘的最后一片，本地下一片正好是seq+1
            info->_seq = object.value("seq").toInt(info->_seq);
            info->_trans_size = object.value("trans_size").toVariant().toLongLong();
            const qint64 total = object.value("total_size").toVariant().toLongLong();
            if (total > 0)
                info->_total_size = total;
        }

        emit sig_upload_progress(info->_name, info->_trans_size, info->_total_size);

        if (info->_trans_size >= info->_total_size) {
            UserMgr::GetInstance()->RemoveUploadFile(md5);
            emit sig_upload_finished(info->_name, md5);
            return;
        }
        if (UserMgr::GetInstance()->UploadPaused())
            return;
        sendNextChunk(md5);
    };

    //下载回包：seq=1 覆盖写，其余追加；写到 is_last 才通知业务层，
    //这样调用方读到的本地文件一定是完整的。
    _handlers[ID_DOWN_LOAD_FILE_RSP] = [this](ReqId, int, QByteArray data) {
        const QJsonObject object = parseObject(data);
        if (object.isEmpty()) {
            emit sig_download_failed(QString(), 1);
            return;
        }

        const QString name = object.value("name").toString();
        auto info = UserMgr::GetInstance()->GetDownloadFile(name);
        if (!info)
            return;

        const int error = object.value("error").toInt(1);
        if (error != 0) {
            UserMgr::GetInstance()->RemoveDownloadFile(name);
            emit sig_download_failed(name, error);
            return;
        }

        const int seq = object.value("seq").toInt(info->_seq);
        const QByteArray chunk = QByteArray::fromBase64(
            object.value("data").toString().toLatin1());

        //本地目录可能还不存在；首片覆盖旧文件，后续分片严格按seq追加
        QDir().mkpath(QFileInfo(info->_client_path).absolutePath());
        QFile file(info->_client_path);
        QIODevice::OpenMode mode = QIODevice::WriteOnly;
        if (seq != 1)
            mode |= QIODevice::Append;
        if (!file.open(mode) || file.write(chunk) != chunk.size()) {
            UserMgr::GetInstance()->RemoveDownloadFile(name);
            emit sig_download_failed(name, 1);
            return;
        }
        file.close();

        info->_seq = seq;
        info->_current_size = object.value("current_size").toVariant().toLongLong();
        info->_total_size = object.value("total_size").toVariant().toLongLong();

        if (object.value("is_last").toBool()) {
            const QString path = info->_client_path;
            UserMgr::GetInstance()->RemoveDownloadFile(name);
            emit sig_download_finished(name, path);
            return;
        }

        //继续下一片；下载不设暂停，暂停只作用于用户主动操作的上传
        info->_seq = seq + 1;
        sendDownloadRequest(name);
    };
}

void FileTcpMgr::handleMsg(ReqId id, int len, QByteArray data)
{
    auto iter = _handlers.find(id); //按消息ID找回调表
    if (iter == _handlers.end()) { //未知ID只记日志，不影响后续帧解析
        qDebug() << "[file-tcp] no handler for id" << id;
        return;
    }
    iter.value()(id, len, data); //len仅作校验提示，包体已由接收循环完整切出
}
