#include "tcpthread.h"

#include <QMetaObject>

#include "tcpmgr.h"

TcpThread::TcpThread()
{
    //TcpMgr 是单例，先在主线程创建，再整体交给网络线程；
    //MoveToThread 内部会额外迁移 socket，不能只调用 QObject::moveToThread。
    _thread = new QThread(); //网络线程
    auto mgr = TcpMgr::GetInstance(); //主线程创建单例
    mgr->MoveToThread(_thread); //对象与socket一起迁移
    _thread->start(); //启动事件循环
}

TcpThread::~TcpThread()
{
    if (_thread == nullptr)
        return; //已经清理

    if (_thread->isRunning()) {
        auto mgr = TcpMgr::GetInstance();
        //在主线程之外关闭socket，再把对象迁回主线程，避免QThread销毁后对象仍指向旧线程。
        //必须用 BlockingQueuedConnection：要等网络线程真正执行完关闭和迁移才能退出事件循环。
        QMetaObject::invokeMethod(mgr.get(), [mgr]() {
            mgr->CloseConnection(); //在socket所属线程关闭
            mgr->MoveToThread(QThread::currentThread()); //迁回主线程
        }, Qt::BlockingQueuedConnection);
        _thread->quit(); //退出事件循环
        _thread->wait(); //等线程结束
    }
    delete _thread; //释放QThread对象
    _thread = nullptr;
}
