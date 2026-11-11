#include "fileservice.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

#include "filetcpmgr.h"
#include "usermgr.h"

namespace {
//头像只接受 4MB 以内的 PNG：服务器单文件上限是 4GB，但头像没有必要放开这么大；
//先校验 PNG 签名，避免把任意文件当成图片交给 Image 和 ResourceServer。
constexpr qint64 MaxAvatarBytes = 4 * 1024 * 1024;

bool isPng(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QByteArray head = file.read(8);
    static const QByteArray signature("\x89PNG\r\n\x1a\n", 8);
    return head == signature;
}

//只有服务端下发的资源文件名才需要下载；默认头像、本地/内嵌/网络地址都能直接加载
bool isRemoteResource(const QString &iconName)
{
    return !iconName.isEmpty()
        && !iconName.startsWith(QLatin1Char(':'))
        && !iconName.startsWith(QStringLiteral("file:"))
        && !iconName.startsWith(QStringLiteral("data:"))
        && !iconName.startsWith(QStringLiteral("http"));
}
}

FileService::FileService()
{
    auto *file = FileTcpMgr::GetInstance().get(); //资源网络的信号源，统一在这里接线

    //连接成功后才允许发起上传/下载；登录可能先于资源连接完成，所以这里再补一次同步
    connect(file, &FileTcpMgr::sig_con_success, this, [this](bool success) {
        _connected = success; //先记连接态，下面的同步会读它
        if (success)
            TryStartResourceSync(); //登录已就绪时立即补拉缺失头像
    });
    //资源服务异常不能影响聊天，只把错误写进个人资料页的提示并结束在途上传
    connect(file, &FileTcpMgr::sig_con_failed, this, [this](const QString &message) {
        _connected = false; //后续请求一律等重连，不直接退出
        if (_uploading)
            HandleUploadFailed(_uploadName, 2); //在途上传按失败收尾，界面不留假进度
        _message = QStringLiteral("资源服务器连接失败：%1").arg(message); //只在资料页提示
        emit uploadStateChanged(); //刷新进度条与按钮文案
    });
    //对端断开与主动关闭共用同一个标志，避免业务层继续往socket投递分片
    connect(file, &FileTcpMgr::sig_connection_closed, this, [this] {
        _connected = false; //断线后OnSessionReady不能再触发同步
        AbortDownloads(); //在途下载已经无法继续，记录和半截文件一起清掉
    });

    //收发帧由FileTcpMgr负责，业务层的落盘、缓存与界面刷新集中在本类
    connect(file, &FileTcpMgr::sig_upload_progress, this, &FileService::HandleUploadProgress); //更新进度
    connect(file, &FileTcpMgr::sig_upload_finished, this,
            [this](const QString &name, const QString &) { HandleUploadFinished(name); }); //md5本地已有，回包只取名字
    connect(file, &FileTcpMgr::sig_upload_failed, this, &FileService::HandleUploadFailed); //失败提示
    connect(file, &FileTcpMgr::sig_download_finished, this, &FileService::HandleDownloadFinished); //拼完整个文件
    connect(file, &FileTcpMgr::sig_download_failed, this, &FileService::HandleDownloadFailed); //下载失败
}

FileService::~FileService() = default;

void FileService::ConnectToResource(const ServerInfo &info)
{
    _resourceConfigured = !info.ResHost.isEmpty() && info.ResPort.toInt() > 0; //服务端是否下发了资源地址
    _connected = false; //新连接尚未建立
    _sessionReady = false; //聊天登录也还没完成
    _uploading = false; //上一个账号的上传一律作废
    _paused = false; //暂停标志随账号一起清
    _uploadMd5.clear(); //资源名只在当前账号内有效
    _uploadName.clear();
    _trans_size = 0; //进度归零，等新账号登录后重新开始
    _total_size = 0;
    AbortDownloads(); //上一个账号的在途下载不能带到新连接
    _message.clear(); //个人资料页的旧提示不保留
    if (!_resourceConfigured) { //没有配置资源服务时不发连接请求
        _message = QStringLiteral("当前服务器未配置资源服务，头像仅保存在本机"); //只提示，不影响聊天
        emit uploadStateChanged();
        return;
    }
    FileTcpMgr::GetInstance()->ConnectToServer(info.ResHost, info.ResPort.toInt()); //交给资源网络线程建连
}

void FileService::CloseResource()
{
    _connected = false; //资源连接即将断开
    _sessionReady = false; //登录态也一起失效
    AbortDownloads(); //主动关闭同样要清掉在途任务
    FileTcpMgr::GetInstance()->CloseConnectionAsync(); //交给网络线程关闭socket
}

void FileService::OnSessionReady()
{
    _sessionReady = true; //聊天已经拿到用户资料和token
    TryStartResourceSync(); //若资源连接已就绪，立即补拉头像
}

void FileService::TryStartResourceSync()
{
    if (!_connected || !_sessionReady) //两个条件缺一不可，谁后完成谁触发
        return;

    auto *mgr = UserMgr::GetInstance().get(); //UserMgr 是唯一数据源
    auto user = mgr->GetUserInfo();
    if (!user) //登录资料还没写入，等下一次触发
        return;

    //本机已有完整头像时不再下载；缺失或上次下载中断留下半截文件才重新请求
    const QString local_avatar = UserMgr::LocalAvatarPath();
    if (!local_avatar.isEmpty() && !UserMgr::IsCompleteAvatarFile(local_avatar))
        QFile::remove(local_avatar); //残图留着会被下次登录当成有效头像
    if (!local_avatar.isEmpty() && !QFileInfo::exists(local_avatar)
        && isRemoteResource(user->_icon))
        RequestResource(user->_uid, user->_icon, local_avatar); //本机头像缓存

    const int count = mgr->FriendCount();
    for (int i = 0; i < count; ++i) {
        auto friend_info = mgr->FriendAt(i);
        if (friend_info)
            CacheContactIcon(friend_info->_uid, friend_info->_icon); //好友列表头像
    }

    //好友申请单独遍历：申请人未必已经是好友，好友列表不会覆盖到他们。
    //已经是好友的 uid 跳过，避免申请里保存的旧资源名把刚下载的新头像又盖回去。
    const int apply_count = mgr->ApplyCount();
    for (int i = 0; i < apply_count; ++i) {
        auto apply_info = mgr->ApplyAt(i);
        if (!apply_info || mgr->CheckFriendById(apply_info->_uid))
            continue;
        CacheContactIcon(apply_info->_uid, apply_info->_icon);
    }
}

void FileService::CacheContactIcon(int uid, const QString &iconName)
{
    if (uid <= 0 || !isRemoteResource(iconName)) //没有归属，或图标不是待下载的资源名
        return;

    const QString target = UserMgr::ResourceCachePath(uid, iconName); //按网关/uid/资源名算缓存路径
    if (target.isEmpty()) //AppData 不可用或文件名非法
        return;

    if (UserMgr::IsCompleteAvatarFile(target)) { //本地已有完整缓存
        UserMgr::GetInstance()->SetContactIcon(uid, QUrl::fromLocalFile(target).toString()); //直接换成 file URL
        return;
    }
    if (QFileInfo::exists(target))
        QFile::remove(target); //下载中断留下的残图不能继续用
    RequestResource(uid, iconName, target); //没有缓存才真正走网络
}

void FileService::RequestResource(int ownerUid, const QString &iconName, const QString &targetPath)
{
    if (ownerUid <= 0 || iconName.isEmpty() || targetPath.isEmpty()) //参数不完整不发请求
        return;

    //调用方可能在下载开始后又调用了一次，这里再检查一次本地文件，命中就直接发布
    if (UserMgr::IsCompleteAvatarFile(targetPath)) {
        if (ownerUid == UserMgr::GetInstance()->GetUid()) { //自己的头像走 data URL
            QFile file(targetPath);
            if (file.open(QIODevice::ReadOnly))
                UserMgr::GetInstance()->SetLocalAvatar(file.read(MaxAvatarBytes + 1));
        }
        else {
            UserMgr::GetInstance()->SetContactIcon(ownerUid,
                QUrl::fromLocalFile(targetPath).toString()); //联系人走本地文件 URL
        }
        return;
    }
    if (QFileInfo::exists(targetPath))
        QFile::remove(targetPath); //半截文件不能接着写，从seq=1重新传时会覆盖

    //同名资源只允许一个下载任务，避免多个列表同时请求同一头像时重复占连接
    if (UserMgr::GetInstance()->GetDownloadFile(iconName))
        return;

    auto info = std::make_shared<DownloadFileInfo>(); //一次下载的本地状态
    info->_name = iconName; //资源名，回包按它找回这条记录
    info->_client_path = targetPath; //写到哪里
    info->_owner_uid = ownerUid; //下载完成后更新谁
    info->_seq = 1; //下载总是从第一片开始
    UserMgr::GetInstance()->AddDownloadFile(info); //登记，FileTcpMgr 会按名字取
    _download_owners[iconName] = ownerUid; //回包只带资源名，归属要在这里记
    FileTcpMgr::GetInstance()->StartDownload(iconName); //投递到资源网络线程
}

void FileService::AbortDownloads()
{
    const int self_uid = UserMgr::GetInstance()->GetUid();
    for (auto iter = _download_owners.begin(); iter != _download_owners.end(); ++iter) {
        //自己的头像写在固定路径，联系人的写在按 uid 分层的缓存目录
        const QString path = (self_uid > 0 && iter.value() == self_uid)
            ? UserMgr::LocalAvatarPath()
            : UserMgr::ResourceCachePath(iter.value(), iter.key());

        //没写完的 PNG 不能留在缓存里，否则下次登录会被当成已下载，头像永久损坏
        if (!path.isEmpty() && !UserMgr::IsCompleteAvatarFile(path))
            QFile::remove(path);

        UserMgr::GetInstance()->RemoveDownloadFile(iter.key()); //记录也要清，重连后才能重新请求
    }
    _download_owners.clear();
}

void FileService::CancelOwnAvatarDownload()
{
    const int uid = UserMgr::GetInstance()->GetUid(); //只取消本机账号的下载
    //上传新头像前旧头像可能还在拉，迟到回包会把刚保存的head.png写回旧内容
    for (auto iter = _download_owners.begin(); iter != _download_owners.end();) {
        if (iter.value() == uid) { //按归属过滤，不动好友的下载任务
            UserMgr::GetInstance()->RemoveDownloadFile(iter.key()); //移出在途表，回包会被直接丢弃
            iter = _download_owners.erase(iter); //erase返回下一个有效迭代器
        }
        else {
            ++iter; //不满足条件才手动前进
        }
    }
}

void FileService::UploadAvatar(const QString &path)
{
    const QFileInfo file_info(path);
    if (!file_info.exists() || file_info.size() <= 0 || file_info.size() > MaxAvatarBytes
        || !isPng(path)) {
        _message = QStringLiteral("头像文件不可用"); //裁剪结果异常，直接拒绝
        emit uploadStateChanged();
        return;
    }

    //旧头像可能还在下载，先停掉，避免它和服务端 seq=1 的截断互相覆盖
    CancelOwnAvatarDownload();

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        _message = QStringLiteral("头像文件无法读取"); //权限/文件被占用
        emit uploadStateChanged();
        return;
    }
    QCryptographicHash hash(QCryptographicHash::Md5); //资源名用 MD5，内容相同即同名
    hash.addData(&file);
    file.close();

    const QString md5 = QString::fromLatin1(hash.result().toHex()); //32位十六进制
    const QString name = md5 + QStringLiteral(".png"); //服务器落盘文件名

    //同一次上传正在等待回包时不重复投递
    if (_uploading && _uploadMd5 == md5)
        return;

    //换了另一张头像：上一次的上传状态已经失效，先清掉再建新的
    if (!_uploadMd5.isEmpty())
        UserMgr::GetInstance()->RemoveUploadFile(_uploadMd5);

    auto info = std::make_shared<UploadFileInfo>(); //本次上传的本地状态
    info->_md5 = md5;
    info->_name = name;
    info->_path = path; //FileTcpMgr 按偏移重新打开这个文件
    info->_total_size = file_info.size();
    info->_uid = UserMgr::GetInstance()->GetUid();
    info->_req_id = ID_UPLOAD_HEAD_ICON_REQ; //头像走 2007
    UserMgr::GetInstance()->AddUploadFile(info);

    _uploadMd5 = md5; //业务层自己的副本，用于暂停/失败清理
    _uploadName = name;
    _trans_size = 0; //第一片还没发
    _total_size = info->_total_size;
    _uploading = true;
    _paused = false;
    UserMgr::GetInstance()->SetUploadPaused(false);

    //资源服务没有配置或还没连上：本机头像已经保存，不需要回滚
    if (!_resourceConfigured || !_connected) {
        _uploading = false;
        _message = QStringLiteral("资源服务器未连接，头像仅保存在本机");
        UserMgr::GetInstance()->RemoveUploadFile(md5);
        _uploadMd5.clear();
        emit uploadStateChanged();
        return;
    }

    _message = QStringLiteral("正在同步头像");
    emit uploadStateChanged(); //先让界面出现进度条
    FileTcpMgr::GetInstance()->StartUpload(md5); //网络线程读第一片
}

//聊天图片走通用上传通道(2001)，与头像的上传状态互不影响
void FileService::UploadChatImage(const QString &name, const QString &path, const QString &md5,
    qint64 messageId, int touid, qint64 threadId, const QString &uniqueId)
{
    const QFileInfo file_info(path);
    if (name.isEmpty() || md5.isEmpty() || !file_info.exists() || file_info.size() <= 0)
        return;

    //同一内容已经在传（例如连续发送同一张图），复用这次上传，避免两路分片互相覆盖进度
    if (UserMgr::GetInstance()->GetUploadFile(md5))
        return;

    auto info = std::make_shared<UploadFileInfo>();
    info->_md5 = md5;
    info->_name = name;
    info->_path = path;
    info->_total_size = file_info.size();
    info->_uid = UserMgr::GetInstance()->GetUid();
    info->_req_id = ID_UPLOAD_FILE_REQ; //通用资源，不是头像
    info->_chat_msg_id = messageId;
    info->_touid = touid;
    info->_thread_id = threadId;
    info->_unique_id = uniqueId;
    UserMgr::GetInstance()->AddUploadFile(info);

    //资源服务器没连上时不阻塞聊天：本地文件已保存，等下次登录重新发送
    if (!_resourceConfigured || !_connected) {
        UserMgr::GetInstance()->RemoveUploadFile(md5);
        return;
    }
    FileTcpMgr::GetInstance()->StartUpload(md5);
}

void FileService::SetUploadPaused(bool paused)
{
    if (!_uploading || _paused == paused) //没有上传或状态没变，直接返回
        return;

    _paused = paused; //业务层记录暂停状态
    UserMgr::GetInstance()->SetUploadPaused(paused); //网络线程回包时检查这个标志

    if (paused) {
        _message = QStringLiteral("头像同步已暂停"); //当前分片仍可能在途
    }
    else {
        _message = QStringLiteral("正在同步头像");
        //续传前先问服务器已经写到哪一片，避免按本地旧进度重复追加
        QJsonObject frame;
        frame["uid"] = UserMgr::GetInstance()->GetUid();
        frame["token"] = UserMgr::GetInstance()->GetToken();
        frame["md5"] = _uploadMd5;
        FileTcpMgr::GetInstance()->SendData(ID_SYNC_FILE_REQ,
            QJsonDocument(frame).toJson(QJsonDocument::Compact)); //让服务器回seq/trans_size
    }
    emit uploadStateChanged();
}

void FileService::ToggleUploadPaused()
{
    SetUploadPaused(!_paused);
}

int FileService::Progress() const
{
    if (_total_size <= 0) //还没有总大小，无法算百分比
        return 0;
    const qint64 percent = _trans_size * 100 / _total_size; //整数百分比
    return static_cast<int>(qBound<qint64>(0LL, percent, 100LL)); //QML只认0..100
}

void FileService::HandleUploadProgress(const QString &name, qint64 trans, qint64 total)
{
    //聊天图片也会走同一个信号，按资源名过滤，不能把它的进度显示到头像上
    if (!_uploading || name != _uploadName)
        return;
    _trans_size = trans; //已经连续确认的字节数，直接作为进度
    _total_size = total;
    if (!_paused) //暂停时保留“已暂停”提示，不被进度覆盖
        _message = QStringLiteral("正在同步头像");
    emit uploadStateChanged();
}

void FileService::HandleUploadFinished(const QString &name)
{
    if (!_uploading || name != _uploadName) //聊天图片完成不结束头像上传
        return;
    _uploading = false; //进度条收起
    _paused = false;
    _trans_size = _total_size; //直接显示100%
    _message = QStringLiteral("头像已同步");
    _uploadMd5.clear(); //后续失败回调不再匹配
    emit uploadStateChanged();
}

void FileService::HandleUploadFailed(const QString &name, int error)
{
    if (!_uploading && _uploadMd5.isEmpty()) //没有在途上传，忽略
        return;
    if (_uploading && !_uploadName.isEmpty() && name != _uploadName)
        return; //聊天图片失败不影响头像的进度与提示

    _uploading = false; //停止进度展示
    _paused = false;
    UserMgr::GetInstance()->SetUploadPaused(false); //网络线程不再继续发片
    if (!_uploadMd5.isEmpty()) {
        UserMgr::GetInstance()->RemoveUploadFile(_uploadMd5); //清掉断点状态
        _uploadMd5.clear();
    }
    _message = QStringLiteral("头像同步失败（错误码 %1），本机头像已保留").arg(error); //不删本地文件
    emit uploadStateChanged();
}

void FileService::HandleDownloadFinished(const QString &name, const QString &path)
{
    const int owner_uid = _download_owners.take(name); //先取归属，防止后续早退丢掉映射
    if (owner_uid == UserMgr::GetInstance()->GetUid()) { //自己的头像
        //上传已经在进行时，服务端文件正在被新头像覆盖，不能拿旧下载结果回滚本机头像
        if (_uploading)
            return;
        QFile file(path); //下载已经落盘，读出来发布到界面
        if (file.open(QIODevice::ReadOnly))
            UserMgr::GetInstance()->SetLocalAvatar(file.read(MaxAvatarBytes + 1));
        return;
    }

    if (owner_uid > 0) {
        auto *mgr = UserMgr::GetInstance().get();
        const QString icon_path = QUrl::fromLocalFile(path).toString(); //QML Image 可直接加载 file URL

        //文件属于谁就先更新谁：即使这个人只在申请列表里，也能显示新头像
        mgr->SetContactIcon(owner_uid, icon_path);

        //同一个资源文件可能被多个联系人使用，按原始资源名一起替换
        const int friend_count = mgr->FriendCount();
        for (int i = 0; i < friend_count; ++i) {
            auto friend_info = mgr->FriendAt(i);
            if (friend_info && friend_info->_icon == name)
                mgr->SetContactIcon(friend_info->_uid, icon_path); //共用同一张图的好友
        }
        const int apply_count = mgr->ApplyCount();
        for (int i = 0; i < apply_count; ++i) {
            auto apply_info = mgr->ApplyAt(i);
            if (apply_info && apply_info->_icon == name)
                mgr->SetContactIcon(apply_info->_uid, icon_path); //共用同一张图的申请人
        }
    }
}

void FileService::HandleDownloadFailed(const QString &name, int error)
{
    const int owner_uid = _download_owners.take(name); //取归属，失败后不再保留
    if (owner_uid <= 0)
        return;

    //失败时清掉写了一半的缓存，避免下次登录把残缺图片当成已下载
    if (owner_uid == UserMgr::GetInstance()->GetUid()) {
        //上传已经开始时本机文件是新头像，不能因为旧下载失败把它删掉
        if (!_uploading)
            QFile::remove(UserMgr::LocalAvatarPath());
        if (!_uploading) {
            _message = QStringLiteral("头像加载失败（错误码 %1）").arg(error);
            emit uploadStateChanged();
        }
    }
    else {
        QFile::remove(UserMgr::ResourceCachePath(owner_uid, name));
    }
}
