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
constexpr int MaxFileLen = 2048; //与ResourceServer的MAX_FILE_LEN一致
constexpr int MaxFileFrame = 16 * 1024; //与ResourceServer的MAX_MESSAGE_LEN一致
constexpr int ResourceFileNotExists = 1012;

QJsonObject parseObject(const QByteArray &data)
{
    const QJsonDocument document = QJsonDocument::fromJson(data);
    return document.isObject() ? document.object() : QJsonObject{};
}
}

FileTcpMgr::FileTcpMgr()
{
    registerMetaType();

    QObject::connect(&_socket, &QTcpSocket::connected, this, [this]() {
        qDebug() << "[file-tcp] connected";
        emit sig_con_success(true);
    });

    QObject::connect(&_socket, &QAbstractSocket::errorOccurred, this,
        [this](QAbstractSocket::SocketError err) {
            if (err == QAbstractSocket::RemoteHostClosedError)
                return;
            qWarning() << "[file-tcp] connect error:" << err << _socket.errorString();
            emit sig_con_failed(_socket.errorString());
        });

    QObject::connect(&_socket, &QTcpSocket::readyRead, this, [this]() {
        _buffer.append(_socket.readAll());
        forever {
            if (!_b_recv_pending) {
                if (_buffer.size() < FileHeadLen)
                    return;

                QDataStream stream(_buffer.left(FileHeadLen));
                stream.setByteOrder(QDataStream::BigEndian);
                stream >> _message_id >> _message_len;
                _buffer = _buffer.mid(FileHeadLen);

                if (_message_len == 0 || _message_len > MaxFileFrame) {
                    qWarning() << "[file-tcp] invalid frame length" << _message_len;
                    slot_close_connection();
                    return;
                }
            }

            if (_buffer.size() < static_cast<int>(_message_len)) {
                _b_recv_pending = true;
                return;
            }

            _b_recv_pending = false;
            const QByteArray body = _buffer.mid(0, static_cast<int>(_message_len));
            _buffer = _buffer.mid(static_cast<int>(_message_len));
            handleMsg(ReqId(_message_id), static_cast<int>(_message_len), body);
        }
    });

    QObject::connect(&_socket, &QTcpSocket::disconnected, this, [this]() {
        _send_queue.clear();
        _current_block.clear();
        _bytes_sent = 0;
        _pending = false;
        emit sig_connection_closed();
    });

    QObject::connect(this, &FileTcpMgr::sig_connect_resource,
                     this, &FileTcpMgr::slot_connect_resource);
    QObject::connect(this, &FileTcpMgr::sig_send_data,
                     this, &FileTcpMgr::slot_send_data);
    QObject::connect(this, &FileTcpMgr::sig_close_connection,
                     this, &FileTcpMgr::slot_close_connection);
    QObject::connect(this, &FileTcpMgr::sig_start_upload,
                     this, &FileTcpMgr::slot_start_upload);
    QObject::connect(this, &FileTcpMgr::sig_start_download,
                     this, &FileTcpMgr::slot_start_download);

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
    //socket 是成员而不是子对象，moveToThread 必须单独调用
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
    _host = host;
    _port = static_cast<quint16>(port);
    slot_close_connection();
    _socket.connectToHost(_host, _port);
}

void FileTcpMgr::slot_close_connection()
{
    _socket.abort();
    _buffer.clear();
    _b_recv_pending = false;
    _message_id = 0;
    _message_len = 0;
    _send_queue.clear();
    _current_block.clear();
    _bytes_sent = 0;
    _pending = false;
}

void FileTcpMgr::slot_send_data(ReqId reqId, QByteArray dataBytes)
{
    if (_socket.state() != QAbstractSocket::ConnectedState || dataBytes.size() > MaxFileFrame) {
        qWarning() << "[file-tcp] cannot send frame" << reqId;
        return;
    }

    QByteArray block;
    QDataStream out(&block, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::BigEndian);
    out << static_cast<quint16>(reqId) << static_cast<quint32>(dataBytes.size());
    block.append(dataBytes);

    if (_pending) {
        _send_queue.enqueue(block);
        return;
    }

    _current_block = block;
    _bytes_sent = 0;
    _pending = true;
    _socket.write(_current_block);
}

//从已确认的偏移读取下一片；同一时刻只允许一片在途，等服务器回包后再继续
void FileTcpMgr::sendNextChunk(const QString &md5)
{
    auto info = UserMgr::GetInstance()->GetUploadFile(md5);
    if (!info)
        return;

    if (info->_trans_size >= info->_total_size) {
        UserMgr::GetInstance()->RemoveUploadFile(md5);
        emit sig_upload_finished(info->_name, md5);
        return;
    }

    QFile file(info->_path);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "[file-tcp] open upload file failed:" << info->_path;
        emit sig_upload_failed(info->_name, 1);
        return;
    }
    if (!file.seek(info->_trans_size)) {
        emit sig_upload_failed(info->_name, 1);
        return;
    }

    const QByteArray buffer = file.read(MaxFileLen);
    if (buffer.isEmpty()) {
        emit sig_upload_failed(info->_name, 1);
        return;
    }
    file.close();

    const int seq = info->_seq + 1;
    const qint64 trans_size = info->_trans_size + buffer.size();
    const bool last = trans_size >= info->_total_size;

    QJsonObject frame;
    frame["uid"] = info->_uid;
    frame["token"] = UserMgr::GetInstance()->GetToken();
    frame["md5"] = info->_md5;
    frame["name"] = info->_name;
    frame["seq"] = seq;
    frame["trans_size"] = static_cast<double>(trans_size);
    frame["total_size"] = static_cast<double>(info->_total_size);
    frame["last"] = last ? 1 : 0;
    frame["data"] = QString::fromLatin1(buffer.toBase64());

    info->_seq = seq;
    info->_trans_size = trans_size;

    SendData(info->_req_id, QJsonDocument(frame).toJson(QJsonDocument::Compact));
}

void FileTcpMgr::slot_start_upload(QString md5)
{
    auto info = UserMgr::GetInstance()->GetUploadFile(md5);
    if (!info)
        return;

    //重新开始上传时从0开始，服务器收到seq=1会截断旧文件
    info->_seq = 0;
    info->_trans_size = 0;
    sendNextChunk(md5);
}

void FileTcpMgr::sendDownloadRequest(const QString &name)
{
    auto info = UserMgr::GetInstance()->GetDownloadFile(name);
    if (!info)
        return;

    QJsonObject frame;
    frame["uid"] = UserMgr::GetInstance()->GetUid();
    frame["token"] = UserMgr::GetInstance()->GetToken();
    frame["owner_uid"] = info->_owner_uid;
    frame["name"] = info->_name;
    frame["seq"] = info->_seq;
    SendData(ID_DOWN_LOAD_FILE_REQ, QJsonDocument(frame).toJson(QJsonDocument::Compact));
}

void FileTcpMgr::slot_start_download(QString name)
{
    sendDownloadRequest(name);
}

void FileTcpMgr::initHandlers()
{
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

        if (error != 0) {
            emit sig_upload_failed(name, error);
            return;
        }
        if (!info)
            return;

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

        if (UserMgr::GetInstance()->UploadPaused())
            return;
        sendNextChunk(md5);
    };

    _handlers[ID_UPLOAD_HEAD_ICON_RSP] = uploadResponse;
    _handlers[ID_UPLOAD_FILE_RSP] = uploadResponse;

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
            //服务器已经丢了这次传输，重新从第一片开始
            info->_seq = 0;
            info->_trans_size = 0;
        }
        else if (error != 0) {
            emit sig_upload_failed(info->_name, error);
            return;
        }
        else {
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

        info->_seq = seq + 1;
        sendDownloadRequest(name);
    };
}

void FileTcpMgr::handleMsg(ReqId id, int len, QByteArray data)
{
    auto iter = _handlers.find(id);
    if (iter == _handlers.end()) {
        qDebug() << "[file-tcp] no handler for id" << id;
        return;
    }
    iter.value()(id, len, data);
}
