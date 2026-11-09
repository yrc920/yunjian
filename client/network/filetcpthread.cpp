#include "filetcpthread.h"

#include <QMetaObject>

#include "filetcpmgr.h"

FileTcpThread::FileTcpThread()
{
    _thread = new QThread();
    auto mgr = FileTcpMgr::GetInstance();
    mgr->MoveToThread(_thread);
    _thread->start();
}

FileTcpThread::~FileTcpThread()
{
    if (_thread == nullptr)
        return;

    if (_thread->isRunning()) {
        auto mgr = FileTcpMgr::GetInstance();
        QMetaObject::invokeMethod(mgr.get(), [mgr]() {
            mgr->slot_close_connection();
            mgr->MoveToThread(QThread::currentThread());
        }, Qt::BlockingQueuedConnection);
        _thread->quit();
        _thread->wait();
    }
    delete _thread;
    _thread = nullptr;
}
