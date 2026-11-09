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
}

FileService::FileService()
{
    auto *file = FileTcpMgr::GetInstance().get();

    connect(file, &FileTcpMgr::sig_con_success, this, [this](bool success) {
        _connected = success;
        if (success)
            TryStartResourceSync();
    });
    connect(file, &FileTcpMgr::sig_con_failed, this, [this](const QString &message) {
        _connected = false;
        if (_uploading)
            HandleUploadFailed(_uploadName, 2);
        _message = QStringLiteral("资源服务器连接失败：%1").arg(message);
        emit uploadStateChanged();
    });
    connect(file, &FileTcpMgr::sig_connection_closed, this, [this] {
        _connected = false;
    });

    connect(file, &FileTcpMgr::sig_upload_progress, this, &FileService::HandleUploadProgress);
    connect(file, &FileTcpMgr::sig_upload_finished, this,
            [this](const QString &name, const QString &) { HandleUploadFinished(name); });
    connect(file, &FileTcpMgr::sig_upload_failed, this, &FileService::HandleUploadFailed);
    connect(file, &FileTcpMgr::sig_download_finished, this, &FileService::HandleDownloadFinished);
    connect(file, &FileTcpMgr::sig_download_failed, this, &FileService::HandleDownloadFailed);
}

FileService::~FileService() = default;

void FileService::ConnectToResource(const ServerInfo &info)
{
    _resourceConfigured = !info.ResHost.isEmpty() && info.ResPort.toInt() > 0;
    _connected = false;
    _sessionReady = false;
    _uploading = false;
    _paused = false;
    _uploadMd5.clear();
    _uploadName.clear();
    _trans_size = 0;
    _total_size = 0;
    _download_owners.clear();
    _message.clear();
    if (!_resourceConfigured) {
        _message = QStringLiteral("当前服务器未配置资源服务，头像仅保存在本机");
        emit uploadStateChanged();
        return;
    }
    FileTcpMgr::GetInstance()->ConnectToServer(info.ResHost, info.ResPort.toInt());
}

void FileService::CloseResource()
{
    _connected = false;
    _sessionReady = false;
    _download_owners.clear();
    FileTcpMgr::GetInstance()->CloseConnectionAsync();
}

void FileService::OnSessionReady()
{
    _sessionReady = true;
    TryStartResourceSync();
}

void FileService::TryStartResourceSync()
{
    if (!_connected || !_sessionReady)
        return;

    auto *mgr = UserMgr::GetInstance().get();
    auto user = mgr->GetUserInfo();
    if (!user)
        return;

    //本机已有头像时不再下载；缺失且服务端存的是资源文件名才请求
    const QString local_avatar = UserMgr::LocalAvatarPath();
    if (!QFileInfo::exists(local_avatar))
        RequestResource(user->_uid, user->_icon, local_avatar);

    const int count = mgr->FriendCount();
    for (int i = 0; i < count; ++i) {
        auto friend_info = mgr->FriendAt(i);
        if (!friend_info)
            continue;

        const QString icon = friend_info->_icon;
        if (icon.isEmpty() || icon.startsWith(QLatin1Char(':'))
            || icon.startsWith(QStringLiteral("file:"))
            || icon.startsWith(QStringLiteral("data:"))
            || icon.startsWith(QStringLiteral("http")))
            continue;

        const QString target = UserMgr::ResourceCachePath(friend_info->_uid, icon);
        if (!target.isEmpty() && QFileInfo::exists(target) && QFileInfo(target).size() > 0) {
            mgr->SetFriendIcon(friend_info->_uid, QUrl::fromLocalFile(target).toString());
            continue;
        }
        RequestResource(friend_info->_uid, icon, target);
    }
}

void FileService::RequestResource(int ownerUid, const QString &iconName, const QString &targetPath)
{
    if (ownerUid <= 0 || iconName.isEmpty() || targetPath.isEmpty())
        return;

    if (QFileInfo::exists(targetPath) && QFileInfo(targetPath).size() > 0) {
        if (ownerUid == UserMgr::GetInstance()->GetUid()) {
            QFile file(targetPath);
            if (file.open(QIODevice::ReadOnly))
                UserMgr::GetInstance()->SetLocalAvatar(file.read(MaxAvatarBytes + 1));
        }
        else {
            UserMgr::GetInstance()->SetFriendIcon(ownerUid,
                QUrl::fromLocalFile(targetPath).toString());
        }
        return;
    }

    if (UserMgr::GetInstance()->GetDownloadFile(iconName))
        return;

    auto info = std::make_shared<DownloadFileInfo>();
    info->_name = iconName;
    info->_client_path = targetPath;
    info->_owner_uid = ownerUid;
    info->_seq = 1;
    UserMgr::GetInstance()->AddDownloadFile(info);
    _download_owners[iconName] = ownerUid;
    FileTcpMgr::GetInstance()->StartDownload(iconName);
}

//新头像开始上传时，旧头像可能还在下载；服务端会以seq=1截断文件，继续下载只会读到残缺数据
void FileService::CancelOwnAvatarDownload()
{
    const int uid = UserMgr::GetInstance()->GetUid();
    for (auto iter = _download_owners.begin(); iter != _download_owners.end();) {
        if (iter.value() == uid) {
            UserMgr::GetInstance()->RemoveDownloadFile(iter.key());
            iter = _download_owners.erase(iter);
        }
        else {
            ++iter;
        }
    }
}

void FileService::UploadAvatar(const QString &path)
{
    const QFileInfo file_info(path);
    if (!file_info.exists() || file_info.size() <= 0 || file_info.size() > MaxAvatarBytes
        || !isPng(path)) {
        _message = QStringLiteral("头像文件不可用");
        emit uploadStateChanged();
        return;
    }

    CancelOwnAvatarDownload();

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        _message = QStringLiteral("头像文件无法读取");
        emit uploadStateChanged();
        return;
    }
    QCryptographicHash hash(QCryptographicHash::Md5);
    hash.addData(&file);
    file.close();

    const QString md5 = QString::fromLatin1(hash.result().toHex());
    const QString name = md5 + QStringLiteral(".png");

    //同一次上传正在等待回包时不重复投递
    if (_uploading && _uploadMd5 == md5)
        return;

    if (!_uploadMd5.isEmpty())
        UserMgr::GetInstance()->RemoveUploadFile(_uploadMd5);

    auto info = std::make_shared<UploadFileInfo>();
    info->_md5 = md5;
    info->_name = name;
    info->_path = path;
    info->_total_size = file_info.size();
    info->_uid = UserMgr::GetInstance()->GetUid();
    info->_req_id = ID_UPLOAD_HEAD_ICON_REQ;
    UserMgr::GetInstance()->AddUploadFile(info);

    _uploadMd5 = md5;
    _uploadName = name;
    _trans_size = 0;
    _total_size = info->_total_size;
    _uploading = true;
    _paused = false;
    UserMgr::GetInstance()->SetUploadPaused(false);

    if (!_resourceConfigured || !_connected) {
        _uploading = false;
        _message = QStringLiteral("资源服务器未连接，头像仅保存在本机");
        UserMgr::GetInstance()->RemoveUploadFile(md5);
        _uploadMd5.clear();
        emit uploadStateChanged();
        return;
    }

    _message = QStringLiteral("正在同步头像");
    emit uploadStateChanged();
    FileTcpMgr::GetInstance()->StartUpload(md5);
}

void FileService::SetUploadPaused(bool paused)
{
    if (!_uploading || _paused == paused)
        return;

    _paused = paused;
    UserMgr::GetInstance()->SetUploadPaused(paused);

    if (paused) {
        _message = QStringLiteral("头像同步已暂停");
    }
    else {
        _message = QStringLiteral("正在同步头像");
        //续传前先问服务器已经写到哪一片，避免按本地旧进度重复追加
        QJsonObject frame;
        frame["uid"] = UserMgr::GetInstance()->GetUid();
        frame["token"] = UserMgr::GetInstance()->GetToken();
        frame["md5"] = _uploadMd5;
        FileTcpMgr::GetInstance()->SendData(ID_SYNC_FILE_REQ,
            QJsonDocument(frame).toJson(QJsonDocument::Compact));
    }
    emit uploadStateChanged();
}

void FileService::ToggleUploadPaused()
{
    SetUploadPaused(!_paused);
}

int FileService::Progress() const
{
    if (_total_size <= 0)
        return 0;
    const qint64 percent = _trans_size * 100 / _total_size;
    return static_cast<int>(qBound<qint64>(0LL, percent, 100LL));
}

void FileService::HandleUploadProgress(const QString &, qint64 trans, qint64 total)
{
    if (!_uploading)
        return;
    _trans_size = trans;
    _total_size = total;
    if (!_paused)
        _message = QStringLiteral("正在同步头像");
    emit uploadStateChanged();
}

void FileService::HandleUploadFinished(const QString &)
{
    if (!_uploading)
        return;
    _uploading = false;
    _paused = false;
    _trans_size = _total_size;
    _message = QStringLiteral("头像已同步");
    _uploadMd5.clear();
    emit uploadStateChanged();
}

void FileService::HandleUploadFailed(const QString &, int error)
{
    if (!_uploading && _uploadMd5.isEmpty())
        return;

    _uploading = false;
    _paused = false;
    UserMgr::GetInstance()->SetUploadPaused(false);
    if (!_uploadMd5.isEmpty()) {
        UserMgr::GetInstance()->RemoveUploadFile(_uploadMd5);
        _uploadMd5.clear();
    }
    _message = QStringLiteral("头像同步失败（错误码 %1），本机头像已保留").arg(error);
    emit uploadStateChanged();
}

void FileService::HandleDownloadFinished(const QString &name, const QString &path)
{
    const int owner_uid = _download_owners.take(name);
    if (owner_uid == UserMgr::GetInstance()->GetUid()) {
        //上传已经在进行时，服务端文件正在被新头像覆盖，不能拿旧下载结果回滚本机头像
        if (_uploading)
            return;
        QFile file(path);
        if (file.open(QIODevice::ReadOnly))
            UserMgr::GetInstance()->SetLocalAvatar(file.read(MaxAvatarBytes + 1));
        return;
    }

    if (owner_uid > 0) {
        auto *mgr = UserMgr::GetInstance().get();
        const QString icon_path = QUrl::fromLocalFile(path).toString();
        //同一个头像文件可能被多个好友使用，按原始资源名一起替换
        const int count = mgr->FriendCount();
        for (int i = 0; i < count; ++i) {
            auto friend_info = mgr->FriendAt(i);
            if (friend_info && friend_info->_icon == name)
                mgr->SetFriendIcon(friend_info->_uid, icon_path);
        }
    }
}

void FileService::HandleDownloadFailed(const QString &name, int error)
{
    const int owner_uid = _download_owners.take(name);
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
