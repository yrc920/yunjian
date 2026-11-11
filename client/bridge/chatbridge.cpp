#include "chatbridge.h"

#include <QFile>
#include <QStringList>
#include <QUuid>

#include "chatservice.h"
#include "sessionservice.h"
#include "friendservice.h"
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
            if (!info->_name.contains(query, Qt::CaseInsensitive)
                && !info->_nick.contains(query, Qt::CaseInsensitive))
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
                {QStringLiteral("icon"), info->_icon},
                {QStringLiteral("lastText"), lastText},
            });
        }
    }
    _friendMatches = matches;
    emit friendMatchesChanged();
}

void ChatBridge::applyTo(int uid, const QString &name, const QString &nick,
                         const QString &icon, const QString &verify, const QString &remark)
{
    if (uid <= 0)
        return;
    if (!SessionService::GetInstance()->isReady()) {
        setApplyState(ApplyFailed, tr("聊天服务尚未连接，请稍后再试"));
        return;
    }

    // 先交给协议层，确认真的发出去了再记下目标；失败时不覆盖上一次在途申请
    if (!FriendService::GetInstance()->applyFriend(uid, verify, remark)) {
        setApplyState(ApplyFailed, tr("上一次申请还在发送，请稍候"));
        return;
    }

    _applyUid = uid;
    _applyName = name;
    _applyNick = nick;
    _applyIcon = icon;
    _applyVerify = verify;
    _applyRemark = remark;
    setApplyState(ApplySending, tr("正在发送申请…"));
}

void ChatBridge::retryApply()
{
    // 上一次根本没发出去：回到普通状态，让「添加到通讯录」重新走表单
    if (_applyUid <= 0) {
        setApplyState(ApplyIdle);
        return;
    }
    applyTo(_applyUid, _applyName, _applyNick, _applyIcon, _applyVerify, _applyRemark);
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
    _currentName = info->_nick.isEmpty() ? info->_name : info->_nick;

    // 消息模型切到这个人，随后触发的 countChanged 会让界面滚到底
    _messages->setFriend(uid);
    emit currentChanged();

    setMode(ChatMode);
    setPane(ChatPage);
}

void ChatBridge::openProfile(int uid)
{
    auto info = UserMgr::GetInstance()->GetFriendById(uid);
    if (!info)
        return;

    closeAddFriend();

    // 界面只认这一份快照，字段缺失时 QML 侧有兜底
    _profile = {
        {QStringLiteral("uid"), uid},
        {QStringLiteral("name"), info->_name},
        {QStringLiteral("icon"), info->_icon},
        {QStringLiteral("nick"), info->_nick},
        {QStringLiteral("remark"), info->_back},
        {QStringLiteral("desc"), info->_desc},
        {QStringLiteral("sex"), info->_sex},
        {QStringLiteral("initial"), info->_name.isEmpty() ? QStringLiteral("?")
                                                         : info->_name.left(1)},
    };
    emit profileChanged();

    setMode(ContactMode);
    setPane(FriendInfoPage);
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
    emit modeChanged();
}

void ChatBridge::setPane(int pane)
{
    if (_pane == pane)
        return;
    _pane = pane;
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
        hasContent |= item.value("kind").toString() == "image"
            || !item.value("text").toString().trimmed().isEmpty();
    }
    if (!hasContent)
        return false;

    //按编辑器顺序发送：文本分帧，图片先发 1031 再由回执触发资源上传
    QStringList sent_uploads; //整批成功后才删输入区的临时压缩图
    for (const QVariant &part : parts) {
        const QVariantMap item = part.toMap();
        if (item.value("kind").toString() == "image") {
            const QString upload = item.value("upload").toString();
            //source 是气泡缩略图，upload 是输入区预先压缩好的上传文件
            if (!ChatService::GetInstance()->sendImage(_currentUid, item.value("source").toString(),
                    upload, item.value("width").toInt(), item.value("height").toInt()))
                return false;
            if (!upload.isEmpty())
                sent_uploads.append(upload);
        } else if (!ChatService::GetInstance()->sendText(_currentUid, item.value("text").toString())) {
            return false;
        }
    }
    for (const QString &path : sent_uploads)
        QFile::remove(path); //发送失败时保留文件，用户可以直接重试
    return true;
}
