#ifndef FILETCPMGR_H
#define FILETCPMGR_H

#include <QTcpSocket>
#include <QQueue>
#include <QThread>
#include <QMap>
#include <functional>
#include "singleton.h"
#include "global.h"

/******************************************************************************
 * @file       filetcpmgr.h
 * @brief      资源服务器 TCP 连接：2 字节 ID + 4 字节长度，负责分片上传与下载
 *
 * 与聊天 TcpMgr 分开的原因：
 *   · 帧头长度不同，聊天帧的包体上限只有 2048 字节
 *   · 文件读写与等待回包不应占用聊天消息线程
 *   · 大文件传输需要独立的发送队列和暂停/续传状态
 *****************************************************************************/

class FileTcpMgr : public QObject, public Singleton<FileTcpMgr>,
    public std::enable_shared_from_this<FileTcpMgr>
{
    Q_OBJECT

public:
    ~FileTcpMgr();

    void MoveToThread(QThread *thread);
    void ConnectToServer(const QString &host, int port);
    void SendData(ReqId reqId, const QByteArray &data);
    void CloseConnectionAsync();
    void StartUpload(const QString &md5);
    void StartDownload(const QString &name);

private:
    friend class Singleton<FileTcpMgr>;
    FileTcpMgr();

    void registerMetaType();
    void initHandlers();
    void handleMsg(ReqId id, int len, QByteArray data);
    void sendNextChunk(const QString &md5);
    void sendDownloadRequest(const QString &name);

    QTcpSocket _socket;
    QString _host;
    quint16 _port = 0;
    QByteArray _buffer;
    bool _b_recv_pending = false;
    quint16 _message_id = 0;
    quint32 _message_len = 0;
    QMap<ReqId, std::function<void(ReqId id, int len, QByteArray data)>> _handlers;

    QQueue<QByteArray> _send_queue;
    QByteArray _current_block;
    qint64 _bytes_sent = 0;
    bool _pending = false;

public slots:
    void slot_connect_resource(QString host, int port);
    void slot_send_data(ReqId reqId, QByteArray data);
    void slot_close_connection();
    void slot_start_upload(QString md5);
    void slot_start_download(QString name);

signals:
    void sig_connect_resource(QString host, int port);
    void sig_send_data(ReqId reqId, QByteArray data);
    void sig_close_connection();
    void sig_start_upload(QString md5);
    void sig_start_download(QString name);

    void sig_con_success(bool success);
    void sig_con_failed(const QString &message);
    void sig_connection_closed();
    void sig_upload_progress(const QString &name, qint64 trans_size, qint64 total_size);
    void sig_upload_finished(const QString &name, const QString &md5);
    void sig_upload_failed(const QString &name, int error);
    void sig_download_finished(const QString &name, const QString &path);
    void sig_download_failed(const QString &name, int error);
};

#endif // FILETCPMGR_H
