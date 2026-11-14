#include "chatbridge.h"

#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QProcess>
#include <QSaveFile>
#include <QTimer>
#include <QStringList>
#include <QUuid>

#include "chatservice.h"
#include "sessionservice.h"
#include "friendservice.h"
#include "profileservice.h"
#include "fileservice.h"
#include "usermgr.h"
#include "userdata.h"

ChatBridge::ChatBridge(QObject *parent) : QObject(parent)
{
    _friends = new FriendListModel(this);
    _applies = new ApplyListModel(this);
    _messages = new MessageModel(this);
    connect(UserMgr::GetInstance().get(), &UserMgr::UserIconChanged,
            this, &ChatBridge::myProfileChanged);
    //昵称/性别/签名改完后个人资料页与侧栏显示名一起刷新
    connect(UserMgr::GetInstance().get(), &UserMgr::UserProfileChanged,
            this, &ChatBridge::myProfileChanged);
    //备注与标签只影响自己这一侧：会话列表刷新对应行，打开中的资料页与聊天标题同步跟随
    connect(UserMgr::GetInstance().get(), &UserMgr::FriendRelationChanged, this,
            [this](int uid) { refreshFriendSnapshot(uid); });
    //好友改了昵称或签名：标题、副标题与资料页一起刷新
    connect(UserMgr::GetInstance().get(), &UserMgr::FriendProfileChanged, this,
            [this](int uid) { refreshFriendSnapshot(uid); });
    //标签并集变化后筛选条重算；当前选中的标签被删空时退回"全部"
    connect(UserMgr::GetInstance().get(), &UserMgr::FriendTagsChanged, this, [this] {
        if (!_tagFilter.isEmpty() && !tagOptions().contains(_tagFilter))
            _tagFilter.clear();
        emit tagFilterChanged();
    });
    //未读总数变化：左侧栏徽标跟着刷新
    connect(UserMgr::GetInstance().get(), &UserMgr::UnreadChanged,
            this, &ChatBridge::unreadChanged);

    auto *profileService = ProfileService::GetInstance().get();
    connect(profileService, &ProfileService::profileUpdated, this,
            [this](bool ok, const QString &message) {
                _profileSaving = false;
                _profileMessage = message;
                emit profileStateChanged();
                emit profileSaved(ok);
            });
    //备注刷新由 UserMgr::FriendRelationChanged 统一处理，这里只结束等待状态
    connect(profileService, &ProfileService::remarkUpdated, this,
            [this](bool ok, int uid, const QString &, const QStringList &, const QString &message) {
                _remarkSaving = false;
                _remarkMessage = message;
                emit remarkStateChanged();
                emit remarkSaved(ok, uid);
            });
    //1050 的资料可能比本地缓存新，界面快照与标题按它重建一次
    connect(profileService, &ProfileService::profileLoaded, this,
            [this](int uid) { refreshFriendSnapshot(uid); });
    //头像同步只影响个人资料页的一小块状态，不复用聊天列表的刷新信号
    connect(FileService::GetInstance().get(), &FileService::uploadStateChanged,
            this, &ChatBridge::avatarSyncChanged);

    auto *friendService = FriendService::GetInstance().get();
    connect(friendService, &FriendService::userFound, this,
            [this](int uid, const QString &name, const QString &nick,
                   const QString &desc, const QString &icon, int) {
                if (_searchState != SearchSearching)
                    return;
                // 用户在等待期间改了关键词：这次结果已经过期，回到空闲让界面重新搜
                if (_searchText.trimmed() != _pendingQuery) {
                    _searchState = SearchIdle;
                    _searchResult.clear();
                    emit searchResultChanged();
                    emit searchStateChanged();
                    return;
                }

                // 结果统一留在浮层里展示，自己、已是好友都不再自动跳转
                _searchResult = {{QStringLiteral("uid"), uid},
                                 {QStringLiteral("name"), name},
                                 {QStringLiteral("nick"), nick},
                                 {QStringLiteral("icon"), icon},
                                 {QStringLiteral("desc"), desc}};
                _searchMessage.clear();
                if (uid == UserMgr::GetInstance()->GetUid()) {
                    _searchState = SearchSelf;
                    _searchMessage = tr("这是你自己的账号");
                } else if (UserMgr::GetInstance()->CheckFriendById(uid)) {
                    _searchState = SearchAlreadyFriend;
                } else if (UserMgr::GetInstance()->AlreadySentApply(uid)) {
                    _searchState = SearchAlreadyApplied;
                } else {
                    _searchState = SearchStranger;
                }
                emit searchResultChanged();
                emit searchStateChanged();
            });
    connect(friendService, &FriendService::searchFailed, this,
            [this](const QString &message) {
                if (_searchState != SearchSearching)
                    return;
                if (_searchText.trimmed() != _pendingQuery) {
                    _searchState = SearchIdle;
                    _searchResult.clear();
                    emit searchResultChanged();
                    emit searchStateChanged();
                    return;
                }
                _searchResult.clear();
                _searchState = SearchNotFound;
                _searchMessage = message;
                emit searchResultChanged();
                emit searchStateChanged();
            });

    // 好友列表变化时，聊天页的过滤结果要跟着更新；没有查询词就不用算
    auto *userMgr = UserMgr::GetInstance().get();
    connect(userMgr, &UserMgr::FriendListChanged, this, [this] {
        if (!_friendQuery.trimmed().isEmpty())
            rebuildFriendMatches();
    });
    connect(userMgr, &UserMgr::FriendChatChanged, this, [this](int) {
        if (!_friendQuery.trimmed().isEmpty())
            rebuildFriendMatches();
        //下载完成信号发出时 UserMgr 还持有自己的锁，排到事件循环末尾再复制文件
        if (!_pending_saves.isEmpty())
            QTimer::singleShot(0, this, [this] { processPendingSaves(); });
    });
    //换账号后消息 ID 与本地目录都换了，旧的另存记录不能继续用
    connect(userMgr, &UserMgr::SessionCleared, this, [this] {
        _pending_saves.clear();
        _saved_paths.clear();
    });

    // 申请回执：成功后把这条记录补进「新的朋友」，失败留在浮层里提示
    connect(friendService, &FriendService::applyFinished, this,
            [this](bool ok, const QString &message) {
                if (_applyUid <= 0)
                    return;
                // 用户在等待期间换了搜索对象：记录照加，但不再改别人浮层的状态
                const bool sameTarget = _searchResult.value(QStringLiteral("uid")).toInt() == _applyUid;
                if (!ok) {
                    if (sameTarget)
                        setApplyState(ApplyFailed, message);
                    return;
                }
                auto *mgr = UserMgr::GetInstance().get();
                mgr->AddSentApply(_applyUid, _applyName, _applyNick, _applyIcon, _applyVerify);
                if (sameTarget)
                    setApplyState(ApplySent, tr("申请已发送，等待对方通过"));
            });

    // 我通过别人的申请后，服务端已经建好会话和欢迎消息，直接进入聊天
    connect(SessionService::GetInstance().get(), &SessionService::friendApproved,
            this, &ChatBridge::openChat);

    // 进入聊天界面时默认打开第一个会话，避免右侧一片空白
    if (_friends->rowCount() > 0) {
        const QModelIndex first = _friends->index(0, 0);
        openChat(first.data(FriendListModel::UidRole).toInt());
    }
}

void ChatBridge::setSearchText(const QString &text)
{
    if (_searchText == text)
        return;

    _searchText = text;
    emit searchTextChanged();

    //输入框被清空时顺手回到空闲态，浮层里的旧结果一起收掉
    if (text.trimmed().isEmpty())
        clearSearch();
}

void ChatBridge::search()
{
    const QString text = _searchText.trimmed();
    if (text.isEmpty() || _searchState == SearchSearching)
        return;
    if (!SessionService::GetInstance()->isReady()) {
        _searchResult.clear();
        _searchState = SearchNotFound;
        _searchMessage = tr("聊天服务尚未连接，请稍后再试");
        emit searchResultChanged();
        emit searchStateChanged();
        return;
    }

    // 先切到「搜索中」，界面据此禁用重复回车；旧结果同时清掉，避免闪一下上一个人
    _pendingQuery = text;
    _searchResult.clear();
    _searchState = SearchSearching;
    _searchMessage.clear();
    setApplyState(ApplyIdle);
    emit searchResultChanged();
    emit searchStateChanged();

    if (!FriendService::GetInstance()->searchUser(text)) {
        _searchState = SearchNotFound;
        _searchMessage = tr("上一次搜索尚未结束，请稍后重试");
        emit searchStateChanged();
    }
}

void ChatBridge::clearSearch()
{
    if (_searchState == SearchIdle && _searchResult.isEmpty() && _searchText.isEmpty())
        return;

    _searchText.clear();
    _searchResult.clear();
    _searchMessage.clear();
    _pendingQuery.clear();
    _searchState = SearchIdle;
    setApplyState(ApplyIdle);
    emit searchTextChanged();
    emit searchResultChanged();
    emit searchStateChanged();
}

// 打开加好友浮层；从聊天页过来时先落到联系人，右侧仍显示「新的朋友」
void ChatBridge::openAddFriend()
{
    if (_mode != ContactMode) {
        setMode(ContactMode);
        setPane(ApplyFriendPage);
    }
    if (_addFriendOpen)
        return;
    setApplyState(ApplyIdle);
    _addFriendOpen = true;
    emit addFriendOpenChanged();
}

// 关闭浮层时把查询、结果和状态一起复位，下次打开从引导文案开始
void ChatBridge::closeAddFriend()
{
    const bool wasOpen = _addFriendOpen;
    _addFriendOpen = false;
    clearSearch();
    if (wasOpen)
        emit addFriendOpenChanged();
}

// 聊天页过滤：只在本地好友数据里找，不发请求
void ChatBridge::setFriendQuery(const QString &query)
{
    if (_friendQuery == query)
        return;
    _friendQuery = query;
    emit friendQueryChanged();
    rebuildFriendMatches();
}

void ChatBridge::clearFriendSearch()
{
    if (_friendQuery.isEmpty() && _friendMatches.isEmpty())
        return;
    _friendQuery.clear();
    _friendMatches.clear();
    emit friendQueryChanged();
    emit friendMatchesChanged();
}

// 重建聊天页过滤结果：只读 UserMgr 的好友数据，不复制业务状态
void ChatBridge::rebuildFriendMatches()
{
    QVariantList matches;
    const QString query = _friendQuery.trimmed();
    if (!query.isEmpty()) {
        auto *mgr = UserMgr::GetInstance().get();
        const int count = mgr->FriendCount();
        for (int row = 0; row < count; ++row) {
            auto info = mgr->FriendAt(row);
            if (!info)
                continue;
            // 备注也要参与匹配：用户可能只记得自己给对方起的名字
            if (!info->_name.contains(query, Qt::CaseInsensitive)
                && !info->_nick.contains(query, Qt::CaseInsensitive)
                && !info->_back.contains(query, Qt::CaseInsensitive))
                continue;

            // 摘要与好友列表保持一致；图片消息只显示占位
            QString lastText = info->_last_msg;
            if (!info->_chat_thread->_chat_msgs.empty()) {
                const auto &last = info->_chat_thread->_chat_msgs.back();
                lastText = last->_imageSource.isEmpty()
                    ? last->_msg_content : QStringLiteral("[图片]");
            }
            matches.append(QVariantMap{
                {QStringLiteral("uid"), info->_uid},
                {QStringLiteral("name"), info->_name},
                {QStringLiteral("nick"), info->_nick},
                {QStringLiteral("displayName"), DisplayName(info->_back, info->_nick,
                                                            info->_name)},
                {QStringLiteral("icon"), info->_icon},
                {QStringLiteral("lastText"), lastText},
            });
        }
    }
    _friendMatches = matches;
    emit friendMatchesChanged();
}

void ChatBridge::applyTo(int uid, const QString &name, const QString &nick,
                         const QString &icon, const QString &verify, const QString &remark,
                         const QStringList &tags)
{
    if (uid <= 0)
        return;
    if (!SessionService::GetInstance()->isReady()) {
        setApplyState(ApplyFailed, tr("聊天服务尚未连接，请稍后再试"));
        return;
    }

    // 先交给协议层，确认真的发出去了再记下目标；失败时不覆盖上一次在途申请
    if (!FriendService::GetInstance()->applyFriend(uid, verify, remark, tags)) {
        setApplyState(ApplyFailed, tr("上一次申请还在发送，请稍候"));
        return;
    }

    _applyUid = uid;
    _applyName = name;
    _applyNick = nick;
    _applyIcon = icon;
    _applyVerify = verify;
    _applyRemark = remark;
    _applyTags = tags;
    setApplyState(ApplySending, tr("正在发送申请…"));
}

void ChatBridge::retryApply()
{
    // 上一次根本没发出去：回到普通状态，让「添加到通讯录」重新走表单
    if (_applyUid <= 0) {
        setApplyState(ApplyIdle);
        return;
    }
    applyTo(_applyUid, _applyName, _applyNick, _applyIcon, _applyVerify, _applyRemark,
            _applyTags);
}

void ChatBridge::setApplyState(int state, const QString &message)
{
    if (_applyState == state && _applyMessage == message)
        return;
    _applyState = state;
    _applyMessage = message;
    emit applyStateChanged();
}

void ChatBridge::authApply(int uid, const QString &remark)
{
    FriendService::GetInstance()->authFriend(uid, remark);
}

void ChatBridge::saveMyProfile(const QString &nick, int sex, const QString &desc)
{
    if (_profileSaving)
        return;
    if (!SessionService::GetInstance()->isReady()) {
        _profileMessage = tr("聊天服务尚未连接，请稍后再试");
        emit profileStateChanged();
        return;
    }
    //昵称与签名去掉首尾空白；签名内部允许换行，原样保留
    if (!ProfileService::GetInstance()->updateProfile(nick.trimmed(), sex, desc.trimmed())) {
        _profileMessage = tr("上一次保存还在进行，请稍候");
        emit profileStateChanged();
        return;
    }
    _profileSaving = true;
    _profileMessage.clear();
    emit profileStateChanged();
}

void ChatBridge::saveFriendRemark(int uid, const QString &remark, const QStringList &tags)
{
    if (uid <= 0 || _remarkSaving)
        return;
    if (!SessionService::GetInstance()->isReady()) {
        _remarkMessage = tr("聊天服务尚未连接，请稍后再试");
        emit remarkStateChanged();
        return;
    }
    //空备注表示清除；标签提交的是编辑后的完整集合，空数组表示清空分组
    if (!ProfileService::GetInstance()->updateFriendRelation(uid, remark.trimmed(), tags)) {
        _remarkMessage = tr("上一次保存还在进行，请稍候");
        emit remarkStateChanged();
        return;
    }
    _remarkSaving = true;
    _remarkMessage.clear();
    emit remarkStateChanged();
}

QStringList ChatBridge::tagOptions() const
{
    return UserMgr::GetInstance()->AllTags();
}

void ChatBridge::setTagFilter(const QString &tag)
{
    const QString next = tag.trimmed();
    if (_tagFilter == next)
        return;
    _tagFilter = next;
    _friends->setTagFilter(_tagFilter);
    emit tagFilterChanged();
}

void ChatBridge::clearTagFilter()
{
    setTagFilter(QString());
}

int ChatBridge::unreadTotal() const
{
    return UserMgr::GetInstance()->UnreadTotal();
}

void ChatBridge::clearProfileMessages()
{
    if (_profileMessage.isEmpty() && _remarkMessage.isEmpty())
        return;
    _profileMessage.clear();
    _remarkMessage.clear();
    emit profileStateChanged();
    emit remarkStateChanged();
}

QString ChatBridge::myIcon() const
{
    return UserMgr::GetInstance()->GetIcon();
}

bool ChatBridge::avatarUploading() const
{
    return FileService::GetInstance()->IsUploading(); //控制进度条显隐
}

bool ChatBridge::avatarUploadPaused() const
{
    return FileService::GetInstance()->IsPaused(); //控制按钮文案
}

int ChatBridge::avatarProgress() const
{
    return FileService::GetInstance()->Progress(); //0..100
}

QString ChatBridge::avatarMessage() const
{
    return FileService::GetInstance()->Message(); //“正在同步/暂停/失败”提示
}

void ChatBridge::toggleAvatarUpload()
{
    FileService::GetInstance()->ToggleUploadPaused(); //暂停与继续共用
}

QVariantMap ChatBridge::myProfile() const
{
    auto info = UserMgr::GetInstance()->GetUserInfo();
    if (!info)
        return {};
    return {{QStringLiteral("uid"), info->_uid},
            {QStringLiteral("name"), info->_name},
            {QStringLiteral("nick"), info->_nick},
            {QStringLiteral("desc"), info->_desc},
            {QStringLiteral("icon"), myIcon()},
            {QStringLiteral("sex"), info->_sex}};
}

QString ChatBridge::myName() const
{
    auto info = UserMgr::GetInstance()->GetUserInfo();
    if (!info)
        return {};

    // 昵称优先，没有昵称才退回用户名
    return info->_nick.isEmpty() ? info->_name : info->_nick;
}

QString ChatBridge::myUserName() const
{
    auto info = UserMgr::GetInstance()->GetUserInfo();
    return info ? info->_name : QString();
}

void ChatBridge::showChatMode()
{
    closeAddFriend();
    clearFriendSearch();
    setMode(ChatMode);

    // 有当前会话就回到会话页，没有就停在空态
    setPane(_currentUid != 0 ? ChatPage : EmptyPane);
}

void ChatBridge::showContactMode()
{
    closeAddFriend();
    clearFriendSearch();
    setMode(ContactMode);

    // 点「联系人」直接落在「新的朋友」，这是列表里第一个入口
    setPane(ApplyFriendPage);
}

void ChatBridge::showSettings()
{
    closeAddFriend();
    clearFriendSearch();
    setMode(SettingsMode);
    setPane(UserInfoPage);
}

void ChatBridge::openChat(int uid)
{
    auto info = UserMgr::GetInstance()->GetFriendById(uid);
    if (!info)
        return;

    closeAddFriend();
    clearFriendSearch();

    _currentUid = uid;
    ChatService::GetInstance()->ensurePrivateChat(uid);
    //标题与列表共用同一条显示名规则：备注 > 昵称 > 用户名
    _currentName = DisplayName(info->_back, info->_nick, info->_name);
    _currentSubtitle = info->_desc;
    //好友可能刚改过签名，打开会话时拉一次最新资料；失败继续用本地缓存
    ProfileService::GetInstance()->requestProfile(uid);

    // 消息模型切到这个人，随后触发的 countChanged 会让界面滚到底
    _messages->setFriend(uid);
    emit currentChanged();

    setMode(ChatMode);
    setPane(ChatPage);
    // 进入会话即视为已读；之后的推送在 UserMgr 里会跳过这个会话
    UserMgr::GetInstance()->SetActivePeer(uid);
}

void ChatBridge::openProfile(int uid)
{
    auto info = UserMgr::GetInstance()->GetFriendById(uid);
    if (!info)
        return;

    closeAddFriend();
    rebuildProfile(info);
    //资料页同样拉一次最新昵称与签名
    ProfileService::GetInstance()->requestProfile(uid);

    setMode(ContactMode);
    setPane(FriendInfoPage);
}

//按当前数据重建资料页快照；字段缺失时 QML 侧有兜底
void ChatBridge::rebuildProfile(const std::shared_ptr<FriendInfo> &info)
{
    if (!info)
        return;

    const QString display = DisplayName(info->_back, info->_nick, info->_name);
    _profile = {
        {QStringLiteral("uid"), info->_uid},
        {QStringLiteral("name"), info->_name},
        {QStringLiteral("icon"), info->_icon},
        {QStringLiteral("nick"), info->_nick},
        {QStringLiteral("remark"), info->_back},
        {QStringLiteral("desc"), info->_desc},
        {QStringLiteral("sex"), info->_sex},
        {QStringLiteral("tags"), QVariant::fromValue(info->_tags)},
        {QStringLiteral("displayName"), display},
        {QStringLiteral("initial"), display.isEmpty() ? QStringLiteral("?") : display.left(1)},
    };
    emit profileChanged();
}

//好友关系侧数据变化：会话标题、副标题与资料页快照都要跟着更新
void ChatBridge::refreshFriendSnapshot(int uid)
{
    auto info = UserMgr::GetInstance()->GetFriendById(uid);
    if (!info)
        return;

    if (_currentUid == uid) {
        _currentName = DisplayName(info->_back, info->_nick, info->_name);
        _currentSubtitle = info->_desc;
        emit currentChanged();
    }
    if (_pane == FriendInfoPage && _profile.value(QStringLiteral("uid")).toInt() == uid)
        rebuildProfile(info);
}

void ChatBridge::openApplyList()
{
    closeAddFriend();
    setMode(ContactMode);
    setPane(ApplyFriendPage);
}

void ChatBridge::setMode(int mode)
{
    if (_mode == mode)
        return;
    _mode = mode;
    //标签筛选只作用于联系人列表；聊天模式必须看到全部会话
    _friends->setFilterEnabled(mode == ContactMode);
    //会话按最近消息排序，联系人保持加好友顺序
    _friends->setSortByRecent(mode == ChatMode);
    //离开会话页后不再把新消息当成"正在看"，未读恢复累加
    if (mode != ChatMode)
        UserMgr::GetInstance()->SetActivePeer(0);
    emit modeChanged();
}

void ChatBridge::setPane(int pane)
{
    if (_pane == pane)
        return;
    _pane = pane;
    // 只有真正停在会话页才算"正在看"，切到资料页/申请页后新消息重新计入未读
    if (pane != ChatPage)
        UserMgr::GetInstance()->SetActivePeer(0);
    emit paneChanged();
}

// 把编辑器片段送出去：图片先挂本地消息再落库上传，文字直接进协议。
// 任意一段失败就返回 false，QML 因此保留输入内容
bool ChatBridge::sendParts(const QVariantList &parts)
{
    auto *mgr = UserMgr::GetInstance().get();
    if (!SessionService::GetInstance()->isReady() || !mgr->GetFriendById(_currentUid))
        return false;

    bool hasContent = false;
    for (const QVariant &part : parts) {
        const QVariantMap item = part.toMap();
        const QString kind = item.value("kind").toString();
        hasContent |= kind == "image" || kind == "video" || kind == "file"
            || !item.value("text").toString().trimmed().isEmpty();
    }
    if (!hasContent)
        return false;

    //先检查整批附件都在本机，避免发到一半才失败、留下半批消息
    for (const QVariant &part : parts) {
        const QVariantMap item = part.toMap();
        const QString kind = item.value("kind").toString();
        const QString path = kind == "image" ? item.value("upload").toString()
                                             : item.value("path").toString();
        if ((kind == "image" && path.isEmpty()) || kind == "text")
            continue;
        if (!QFileInfo(path).isFile())
            return false;
    }
    //按编辑器顺序发送：文本分帧，图片先发 1031，视频/文件先发 1034；
    //回执到达后再把资源交给资源服务器
    QStringList sent_uploads; //整批成功后才删输入区的临时压缩图
    for (const QVariant &part : parts) {
        const QVariantMap item = part.toMap();
        const QString kind = item.value("kind").toString();
        if (kind == "image") {
            const QString upload = item.value("upload").toString();
            //source 是气泡缩略图，upload 是输入区预先压缩好的上传文件
            if (!ChatService::GetInstance()->sendImage(_currentUid, item.value("source").toString(),
                    upload, item.value("width").toInt(), item.value("height").toInt()))
                return false;
            if (!upload.isEmpty())
                sent_uploads.append(upload);
        }
        else if (kind == "video") {
            if (!ChatService::GetInstance()->sendVideo(_currentUid,
                    item.value("path").toString(), item.value("name").toString(),
                    item.value("size").toLongLong()))
                return false;
        }
        else if (kind == "file") {
            if (!ChatService::GetInstance()->sendFile(_currentUid,
                    item.value("path").toString(), item.value("name").toString(),
                    item.value("size").toLongLong()))
                return false;
        }
        else if (!ChatService::GetInstance()->sendText(_currentUid,
                     item.value("text").toString())) {
            return false;
        }
    }
    for (const QString &path : sent_uploads)
        QFile::remove(path); //发送失败时保留文件，用户可以直接重试
    return true;
}

void ChatBridge::downloadResource(qint64 messageId)
{
    ChatService::GetInstance()->DownloadResource(messageId);
}

void ChatBridge::toggleResourceTransfer(qint64 messageId, const QString &msgId)
{
    ChatService::GetInstance()->ToggleResourceTransfer(messageId, msgId);
}

QString ChatBridge::resourceUrl(qint64 messageId)
{
    QString path = savedPath(messageId);
    if (path.isEmpty())
        path = ChatService::LocalPathForMessage(messageId);
    return path.isEmpty() ? QString() : QUrl::fromLocalFile(path).toString();
}

bool ChatBridge::openResource(qint64 messageId)
{
    const bool hadSaved = _saved_paths.contains(messageId);
    const QString saved = savedPath(messageId);
    if (hadSaved && saved.isEmpty())
        emit resourceNotice(tr("另存的文件已被移动或删除，已打开本地缓存"));
    const QString local = saved.isEmpty() ? ChatService::LocalPathForMessage(messageId) : saved;
    if (local.isEmpty()) {
        UserMgr::GetInstance()->MarkResourceMissing(messageId);
        emit resourceNotice(tr("本地文件已被移动或删除，请重新下载"));
        return false;
    }
    return QDesktopServices::openUrl(QUrl::fromLocalFile(local)); //文件用系统默认程序打开
}

//另存为：本机已有资源就直接复制；还没下载时排队，下载完成后由 processPendingSaves 收尾
int ChatBridge::saveResourceAs(qint64 messageId, const QUrl &target)
{
    if (!target.isLocalFile())
        return 0;
    const QString destination = target.toLocalFile();
    if (destination.isEmpty())
        return 0;

    const QString source = ChatService::LocalPathForMessage(messageId);
    if (!source.isEmpty()) {
        if (!copyToPath(source, destination))
            return 0;
        _saved_paths.insert(messageId, destination);
        return 1;
    }

    auto message = UserMgr::GetInstance()->FindMessage(messageId);
    if (!message || message->_resource_name.isEmpty())
        return 0;
    if (message->_transfer_state == ResourceTransferState::Done)
        UserMgr::GetInstance()->MarkResourceMissing(messageId); //本地已丢失，重新下载
    _pending_saves.insert(messageId, destination);
    if (message->_transfer_state == ResourceTransferState::DownloadPaused)
        ChatService::GetInstance()->ToggleResourceTransfer(messageId, message->_msg_id);
    else
        ChatService::GetInstance()->DownloadResource(messageId);
    return 2;
}

//下载结束后检查排队中的另存为；成功/失败都通过信号回到界面，不静默丢失
void ChatBridge::processPendingSaves()
{
    if (_pending_saves.isEmpty())
        return;
    auto *mgr = UserMgr::GetInstance().get();
    for (auto it = _pending_saves.begin(); it != _pending_saves.end();) {
        const qint64 messageId = it.key();
        const QString destination = it.value();
        auto message = mgr->FindMessage(messageId);
        if (!message) {
            it = _pending_saves.erase(it);
            emit resourceSaved(false, destination);
            continue;
        }
        if (message->_transfer_state == ResourceTransferState::Failed) {
            it = _pending_saves.erase(it);
            emit resourceSaved(false, destination);
            continue;
        }
        if (message->_transfer_state != ResourceTransferState::Done) {
            ++it;
            continue;
        }
        const QString source = ChatService::LocalPathForMessage(messageId);
        const bool ok = !source.isEmpty() && copyToPath(source, destination);
        if (ok)
            _saved_paths.insert(messageId, destination);
        it = _pending_saves.erase(it);
        emit resourceSaved(ok, destination);
    }
}

//QSaveFile 先写临时文件再原子替换，复制中途失败不会破坏目标位置的原文件
bool ChatBridge::copyToPath(const QString &source, const QString &destination)
{
    const QFileInfo sourceInfo(source);
    const QFileInfo targetInfo(destination);
    if (sourceInfo.absoluteFilePath() == targetInfo.absoluteFilePath())
        return true;

    QFile input(source);
    if (!input.open(QIODevice::ReadOnly))
        return false;
    QDir().mkpath(targetInfo.absolutePath());
    QSaveFile output(destination);
    if (!output.open(QIODevice::WriteOnly))
        return false;
    while (!input.atEnd()) {
        const QByteArray chunk = input.read(1 << 20);
        if (output.write(chunk) != chunk.size()) {
            output.cancelWriting();
            return false;
        }
    }
    return output.commit();
}

//已另存的位置优先；文件被移动/删除时清掉记录并返回空
QString ChatBridge::savedPath(qint64 messageId)
{
    const QString path = _saved_paths.value(messageId);
    if (path.isEmpty())
        return {};
    if (QFileInfo::exists(path))
        return path;
    _saved_paths.remove(messageId);
    return {};
}

//在系统文件管理器中定位资源：Windows 选中文件，macOS 用 Finder 显示，Linux 打开所在目录
bool ChatBridge::revealResource(qint64 messageId)
{
    const bool hasSaved = _saved_paths.contains(messageId);
    const QString saved = savedPath(messageId);
    if (hasSaved && saved.isEmpty()) {
        emit resourceNotice(tr("另存的文件已被移动或删除"));
        return false; //用户明确另存过，就不能悄悄跳回下载缓存
    }
    const QString path = saved.isEmpty() ? ChatService::LocalPathForMessage(messageId) : saved;
    if (path.isEmpty()) {
        UserMgr::GetInstance()->MarkResourceMissing(messageId);
        emit resourceNotice(tr("本地文件已被移动或删除，请重新下载"));
        return false;
    }
#if defined(Q_OS_WIN)
    return QProcess::startDetached(QStringLiteral("explorer.exe"),
        {QStringLiteral("/select,"), QDir::toNativeSeparators(path)});
#elif defined(Q_OS_MACOS)
    return QProcess::startDetached(QStringLiteral("open"), {QStringLiteral("-R"), path});
#else
    return QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
#endif
}

void ChatBridge::markResourceMissing(qint64 messageId)
{
    UserMgr::GetInstance()->MarkResourceMissing(messageId);
}

bool ChatBridge::copyText(const QString &text)
{
    if (text.isEmpty())
        return false;
    QGuiApplication::clipboard()->setText(text);
    return true;
}

//图片复制优先用压缩图，缺失时用气泡缩略图；data URL 也能直接解码
bool ChatBridge::copyImage(qint64 messageId, const QUrl &fallback)
{
    QImage image;
    QString path = savedPath(messageId);
    if (path.isEmpty())
        path = ChatService::LocalPathForMessage(messageId);
    if (!path.isEmpty())
        image = QImage(path);
    if (image.isNull() && !fallback.isEmpty()) {
        if (fallback.isLocalFile()) {
            image = QImage(fallback.toLocalFile());
        }
        else if (fallback.scheme() == QStringLiteral("data")) {
            const QByteArray encoded = QByteArray::fromPercentEncoding(
                fallback.toString().section(QLatin1Char(','), 1).toLatin1());
            image = QImage::fromData(encoded);
        }
    }
    if (image.isNull())
        return false;
    QGuiApplication::clipboard()->setImage(image);
    return true;
}

QVariantList ChatBridge::mediaItems()
{
    QVariantList items;
    auto info = UserMgr::GetInstance()->GetFriendById(_currentUid);
    if (!info || !info->_chat_thread)
        return items;

    for (const auto &message : info->_chat_thread->_chat_msgs) {
        if (!message)
            continue;
        if (message->_msg_type == ChatMsgType::Image) {
            QString url = savedPath(message->_message_id);
            if (url.isEmpty())
                url = ChatService::LocalPathForMessage(message->_message_id);
            if (!url.isEmpty())
                url = QUrl::fromLocalFile(url).toString();
            else if (!message->_imageSource.isEmpty())
                url = message->_imageSource; //退到缩略图或 data URL
            if (url.isEmpty())
                continue;
            items.append(QVariantMap{{QStringLiteral("messageId"), message->_message_id},
                                     {QStringLiteral("type"), 1},
                                     {QStringLiteral("url"), url},
                                     {QStringLiteral("title"), tr("图片")}});
        }
        else if (message->_msg_type == ChatMsgType::Video
                 && message->_transfer_state == ResourceTransferState::Done) {
            QString path = savedPath(message->_message_id);
            if (path.isEmpty())
                path = ChatService::LocalPathForMessage(message->_message_id);
            if (path.isEmpty())
                continue;
            items.append(QVariantMap{{QStringLiteral("messageId"), message->_message_id},
                                     {QStringLiteral("type"), 2},
                                     {QStringLiteral("url"),
                                      QUrl::fromLocalFile(path).toString()},
                                     {QStringLiteral("title"),
                                      message->_file_name.isEmpty()
                                          ? tr("视频") : message->_file_name}});
        }
    }
    return items;
}
