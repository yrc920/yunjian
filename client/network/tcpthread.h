#ifndef TCPTHREAD_H
#define TCPTHREAD_H

#include <QThread>

//RAII线程封装：构造时创建线程并把TcpMgr移入，析构时停连接、退线程
class TcpThread
{
public:
    TcpThread();
    ~TcpThread();
    TcpThread(const TcpThread &) = delete;
    TcpThread &operator=(const TcpThread &) = delete;

private:
    QThread *_thread = nullptr;
};

#endif // TCPTHREAD_H
