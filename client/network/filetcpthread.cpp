#include "filetcpthread.h"

#include <QMetaObject>

#include "filetcpmgr.h"

FileTcpThread::FileTcpThread()
{
    //头像/文件连接单独占一个线程：大文件分片不能和聊天消息抢同一个事件循环
    _thread = new QThread(); //资源网络线程
    auto mgr = FileTcpMgr::GetInstance(); //主线程创建单例
    mgr->MoveToThread(_thread); //对象与socket一起迁移
    _thread->start(); //启动事件循环
}

FileTcpThread::~FileTcpThread()
{
    if (_thread == nullptr)
        return; //已经清理

    if (_thread->isRunning()) {
        auto mgr = FileTcpMgr::GetInstance();
        //先关socket再迁回主线程，最后才退出事件循环并销毁QThread
        QMetaObject::invokeMethod(mgr.get(), [mgr]() {
            mgr->slot_close_connection(); //在socket所属线程关闭
            mgr->MoveToThread(QThread::currentThread()); //迁回主线程
        }, Qt::BlockingQueuedConnection);
        _thread->quit(); //退出事件循环
        _thread->wait(); //等线程结束
    }
    delete _thread; //释放QThread
    _thread = nullptr;
}
