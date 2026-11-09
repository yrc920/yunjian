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

    void MoveToThread(QThread *thread); //对象与成员socket一起迁移
    void ConnectToServer(const QString &host, int port);
    void SendData(ReqId reqId, const QByteArray &data);
    void CloseConnectionAsync();
    void StartUpload(const QString &md5); //按md5取UploadFileInfo开始/续传
    void StartDownload(const QString &name); //按资源名取DownloadFileInfo开始下载

private:
    friend class Singleton<FileTcpMgr>;
    FileTcpMgr();

    void registerMetaType();
    void initHandlers();
    void handleMsg(ReqId id, int len, QByteArray data);
    void sendNextChunk(const QString &md5); //读服务器已确认偏移的下一片
    void sendDownloadRequest(const QString &name);

    QTcpSocket _socket; //必须与FileTcpMgr同线程，MoveToThread里单独迁移
    QString _host;
    quint16 _port = 0;
    QByteArray _buffer; //半包缓存
    bool _b_recv_pending = false; //头部已解析，等待完整包体
    quint16 _message_id = 0;
    quint32 _message_len = 0; //资源帧长度是4字节
    QMap<ReqId, std::function<void(ReqId id, int len, QByteArray data)>> _handlers;

    //发送队列：socket.write可能只写出一部分，由bytesWritten逐块推进
    QQueue<QByteArray> _send_queue;
    QByteArray _current_block;
    qint64 _bytes_sent = 0;
    bool _pending = false;

public slots:
    //以下槽只在网络线程执行，外部通过同名信号排队进入
    void slot_connect_resource(QString host, int port);
    void slot_send_data(ReqId reqId, QByteArray data);
    void slot_close_connection();
    void slot_start_upload(QString md5);
    void slot_start_download(QString name);

signals:
    //请求类信号：由主线程发出，网络线程的槽接收
    void sig_connect_resource(QString host, int port);
    void sig_send_data(ReqId reqId, QByteArray data);
    void sig_close_connection();
    void sig_start_upload(QString md5);
    void sig_start_download(QString name);

    //结果类信号：由网络线程发出，FileService 在主线程更新 UI
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
