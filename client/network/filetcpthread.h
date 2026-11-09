#ifndef FILETCPTHREAD_H
#define FILETCPTHREAD_H

#include <QThread>

//资源网络线程：与聊天TcpMgr分开，避免大文件分片阻塞聊天收发
class FileTcpThread
{
public:
    FileTcpThread();
    ~FileTcpThread();
    FileTcpThread(const FileTcpThread &) = delete;
    FileTcpThread &operator=(const FileTcpThread &) = delete;

private:
    QThread *_thread = nullptr;
};

#endif // FILETCPTHREAD_H
