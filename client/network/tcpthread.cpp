#include "tcpthread.h"

#include <QMetaObject>

#include "tcpmgr.h"

TcpThread::TcpThread()
{
    _thread = new QThread();
    auto mgr = TcpMgr::GetInstance();
    mgr->MoveToThread(_thread);
    _thread->start();
}

TcpThread::~TcpThread()
{
    if (_thread == nullptr)
        return;

    if (_thread->isRunning()) {
        auto mgr = TcpMgr::GetInstance();
        //在主线程之外关闭socket，再把对象迁回主线程，避免QThread销毁后对象仍指向旧线程
        QMetaObject::invokeMethod(mgr.get(), [mgr]() {
            mgr->CloseConnection();
            mgr->MoveToThread(QThread::currentThread());
        }, Qt::BlockingQueuedConnection);
        _thread->quit();
        _thread->wait();
    }
    delete _thread;
    _thread = nullptr;
}
