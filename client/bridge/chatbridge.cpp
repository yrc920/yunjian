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
#include "groupservice.h"
#include "shellbridge.h"
#include "tcpmgr.h"
#include "usermgr.h"
#include "userdata.h"

ChatBridge::ChatBridge(QObject *parent) : QObject(parent)
{
    _conversations = new ConversationModel(this);
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
    //群资料或成员变化：当前会话的标题、副标题与群资料页一起刷新
    connect(UserMgr::GetInstance().get(), &UserMgr::GroupInfoChanged, this,
            [this](qint64 threadId) {
                if (isGroup() && threadId == _currentThreadId)
                    refreshConversationSnapshot();
            });
    connect(UserMgr::GetInstance().get(), &UserMgr::GroupListChanged, this, [this] {
        if (isGroup())
            refreshConversationSnapshot();
    });
    //删好友回包：成功后从联系人列表移除，会话与历史保留；
    //删除后不能停在对方资料页上，会话页或空态二选一
    if (auto *tcp = TcpMgr::GetInstance().get()) {
        connect(tcp, &TcpMgr::sig_delete_friend_rsp, this, [this](const QJsonObject &data) {
            const int uid = data.value("uid").toInt();
            if (data.value("error").toInt(1) != 0) {
                //乐观移除失败：把这一行放回去，避免列表与服务器不一致
                if (uid > 0)
                    UserMgr::GetInstance()->RestoreFriend(uid);
                _chatNotice = tr("删除好友失败，请稍后重试");
                emit chatNoticeChanged();
                return;
            }
            if (uid <= 0)
                return;
            //删除后不能停在对方资料页；正在聊的这个会话保留（历史可查）
            setPane(uid == _currentUid ? ChatPage : EmptyPane);
        });
        //发送失败且原因是关系已不存在：给一句明确提示，别让用户猜"为什么发不出去"
        connect(tcp, &TcpMgr::sig_chat_msg_failed, this, [this](const QJsonObject &data) {
            if (!data.value("not_friend").toBool())
                return;
            //顶部横条已经常驻说明原因，这里不再弹一次性提示，避免同一句话出现两次
            _notFriendUid = data.value("touid").toInt();
            emit chatBlockedChanged();
        });
    }

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
                   const QString &desc, const QString &icon, int, int relation) {
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
                //关系以服务端为准：自己的本地缓存可能还留着"好友"对象（会话要保留），
                //但对方早已单方面删除，这时必须允许重新申请
                if (uid == UserMgr::GetInstance()->GetUid()) {
                    _searchState = SearchSelf;
                    _searchMessage = tr("这是你自己的账号");
                } else if (relation == 1) {
                    _searchState = SearchAlreadyFriend;
                } else if (relation == 2) {
                    _searchState = SearchAlreadyApplied;
                } else if (relation == 3) {
                    _searchState = SearchStranger;
                    _searchMessage = tr("对方已向你发送好友申请，可在「新的朋友」里处理");
                } else if (relation == 4) {
                    _searchState = SearchStranger;
                    _searchMessage = tr("你们曾是好友，可以重新添加");
                } else if (relation == 0) {
                    _searchState = SearchStranger;
                } else if (UserMgr::GetInstance()->CheckFriendById(uid)) {
                    //服务端未返回关系（旧版本）时退回本地判断
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
        //关系恢复（重新通过验证）后顶部的关系提示要立刻收起来
        emit chatBlockedChanged();
        if (!_friendQuery.trimmed().isEmpty())
            rebuildFriendMatches();
    });
    connect(userMgr, &UserMgr::ConversationChanged, this, [this](qint64, int) {
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
                //从聊天页补的验证：顶部横条改成"等待对方通过"，不用再点一次
                if (_applyUid == _currentUid) {
                    _reAddSentUid = _applyUid;
                    emit chatBlockedChanged();
                }
            });

    // 群服务：建群、群资料、群设置与成员操作的回包都在这里落到界面状态
    auto *groupService = GroupService::GetInstance().get();
    connect(groupService, &GroupService::createFinished, this,
            [this](bool ok, qint64 threadId, const QString &message) {
                _createGroupBusy = false;
                _createGroupMessage = message;
                emit createGroupChanged();
                if (!ok)
                    return;
                _createGroupOpen = false;
                _createGroupMessage.clear();
                emit createGroupChanged();
                openGroup(threadId);
            });
    connect(groupService, &GroupService::infoLoaded, this,
            [this](bool ok, qint64 threadId, const QString &message) {
                if (ok && _pendingOpenGroup == threadId) {
                    _pendingOpenGroup = 0;
                    openGroup(threadId);
                }
                if (threadId != _currentThreadId || _pane != GroupInfoPage)
                    return;
                if (ok)
                    _groupMessage.clear();
                setGroupState(false, ok ? QString() : message);
                emit groupChanged();
            });
    connect(groupService, &GroupService::updateFinished, this,
            [this](bool ok, qint64 threadId, const QString &action, const QString &message) {
                if (threadId != _currentThreadId)
                    return;
                setGroupState(false, ok ? QString() : message);
                Q_UNUSED(action);
            });
    connect(groupService, &GroupService::memberOpFinished, this,
            [this](bool ok, qint64 threadId, const QString &action, const QString &message) {
                if (threadId != _currentThreadId)
                    return;
                setGroupState(false, message);
                if (!ok)
                    return;
                if (action == "dismiss" || action == "leave") {
                    if (action == "leave")
                        UserMgr::GetInstance()->RemoveGroup(threadId);
                    _currentThreadId = 0;
                    _currentUid = 0;
                    _currentKind = static_cast<int>(ChatThreadKind::Private);
                    _currentName.clear();
                    _currentSubtitle.clear();
                    _messages->setConversation(0, 0, 0);
                    setPane(EmptyPane);
                    emit currentChanged();
                    emit groupChanged();
                }
            });
    connect(groupService, &GroupService::groupActivated, this,
            [this](qint64 threadId) {
                //被邀请后第一次拿到群：如果正停在这个群的资料页，补一次成员列表
                if (threadId == _currentThreadId)
                    emit groupChanged();
            });
    connect(groupService, &GroupService::groupRemoved, this,
            [this](qint64 threadId, bool dismissed) {
                _chatNotice = dismissed ? tr("群聊已解散") : tr("你已被移出群聊");
                emit chatNoticeChanged();
                if (threadId != _currentThreadId)
                    return;
                _currentThreadId = 0;
                _currentUid = 0;
                _currentKind = static_cast<int>(ChatThreadKind::Private);
                _currentName.clear();
                _currentSubtitle.clear();
                _messages->setConversation(0, 0, 0);
                setPane(EmptyPane);
                emit currentChanged();
                emit groupChanged();
            });

    // 我自己同意了别人的申请：直接进入会话，欢迎消息就是开场白。
    //通过之后关系已恢复，"不是好友"的失败标记要一起清掉
    connect(SessionService::GetInstance().get(), &SessionService::friendAdded, this,
            [this](int uid) {
                if (_notFriendUid == uid || _reAddSentUid == uid) {
                    _notFriendUid = 0;
                    _reAddSentUid = 0;
                    emit chatBlockedChanged();
                }
                openChat(uid);
            });
    // 别人通过我的申请：只更新列表与红点，不切页，避免打断当前操作。
    //关系恢复后要把"发送失败"的标记清掉，否则顶部横条会一直挂着
    connect(SessionService::GetInstance().get(), &SessionService::friendApproved, this,
            [this](int uid) {
                if (_notFriendUid != uid && _reAddSentUid != uid)
                    return;
                _notFriendUid = 0;
                _reAddSentUid = 0;
                emit chatBlockedChanged();
            });
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

void ChatBridge::authApply(int uid, const QString &remark, const QStringList &tags)
{
    FriendService::GetInstance()->authFriend(uid, remark, tags);
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

//角标只统计会打扰人的会话：免打扰的群不计入，会话行仍自己显示小圆点
int ChatBridge::unreadTotal() const
{
    return UserMgr::GetInstance()->UnreadBadgeTotal();
}

//单聊顶部的常驻提示：我删了对方、被对方删除后发送失败、验证已经发出。
//三种情况都要给同一位置的一致文案，用户不需要猜"为什么发不出去"
QString ChatBridge::chatBlockedText() const
{
    if (isGroup() || _currentUid <= 0)
        return {};
    auto info = UserMgr::GetInstance()->GetFriendById(_currentUid);
    const bool sent = _reAddSentUid == _currentUid;
    if (info && !info->_is_friend)
        return sent ? tr("朋友验证已发送，等待对方通过")
                    : tr("你已删除对方，发送朋友验证后可继续聊天");
    if (_notFriendUid == _currentUid)
        return sent ? tr("朋友验证已发送，等待对方通过")
                    : tr("对方还不是你的好友，消息未发送");
    return {};
}

//不离开当前聊天，直接补一条验证：弹层收集一句话，这里负责按当前会话对象发出去。
//走联系人页的加好友浮层会强制切换左栏，聊天上下文被打断，这是之前"割裂"的来源
void ChatBridge::sendReAdd(const QString &text)
{
    if (isGroup() || _currentUid <= 0)
        return;
    auto info = UserMgr::GetInstance()->GetFriendById(_currentUid);
    if (!info)
        return;
    applyTo(_currentUid, DisplayName(info->_back, info->_nick, info->_name),
            info->_nick, info->_icon, text, QString(), QStringList());
}

void ChatBridge::clearChatNotice()
{
    if (_chatNotice.isEmpty())
        return;
    _chatNotice.clear();
    emit chatNoticeChanged();
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

    // 有当前会话就回到会话页，没有就停在空态；群聊没有 peer uid，用 thread 判断
    setPane(_currentThreadId > 0 ? ChatPage : EmptyPane);
}

void ChatBridge::showContactMode()
{
    closeAddFriend();
    clearFriendSearch();
    setMode(ContactMode);

    // 第一次进入落在「新的朋友」；之后回到联系人区域沿用上次停留的页面
    if (_pane != ApplyFriendPage && _pane != FriendInfoPage) {
        _lastContactPane = ApplyFriendPage;
        setPane(ApplyFriendPage);
    }
}

void ChatBridge::showSettings()
{
    closeAddFriend();
    clearFriendSearch();
    setMode(SettingsMode);
    setPane(UserInfoPage);
}

// 撤回：只发消息号，服务端按库里的记录判断归属与时间窗
void ChatBridge::recallMessage(qint64 messageId)
{
    ChatService::GetInstance()->RecallMessage(messageId);
}

int ChatBridge::retryMessage(const QString &msgId)
{
    const ConversationRef conv = currentConversation();
    //关系已经断开时不再把消息重新塞进队列：服务端只会再拒一次，界面却像没反应
    if (conv._kind == ChatThreadKind::Private && conv._peer_uid > 0) {
        auto info = UserMgr::GetInstance()->GetFriendById(conv._peer_uid);
        if ((info && !info->_is_friend) || _notFriendUid == conv._peer_uid)
            return 1;
    }
    return ChatService::GetInstance()->RetryMessage(conv, msgId) ? 0 : 2;
}

// 退出登录交给外壳处理：它负责切页与关闭会话，ChatBridge 只做转发
void ChatBridge::logout()
{
    if (auto *shell = ShellBridge::instance())
        shell->logout();
}

void ChatBridge::deleteFriend(int uid)
{
    if (uid <= 0)
        uid = _profile.value(QStringLiteral("uid")).toInt();
    if (uid <= 0)
        return;
    //先本地移除再发请求：确认后联系人列表立刻少一个人，不用等回包；
    //失败时由回包处理把这一行恢复
    UserMgr::GetInstance()->RemoveFriend(uid);
    FriendService::GetInstance()->deleteFriend(uid);
}

// 打开确认：名字优先取备注，其次昵称，最后用户名
void ChatBridge::askDeleteFriend(int uid)
{
    if (uid <= 0)
        uid = _profile.value(QStringLiteral("uid")).toInt();
    auto info = UserMgr::GetInstance()->GetFriendById(uid);
    if (!info)
        return;

    _deleteTargetUid = uid;
    _deleteTargetName = DisplayName(info->_back, info->_nick, info->_name);
    _deleteConfirmVisible = true;
    emit deleteConfirmChanged();
}

void ChatBridge::confirmDeleteFriend()
{
    const int uid = _deleteTargetUid;
    _deleteConfirmVisible = false;
    _deleteTargetUid = 0;
    _deleteTargetName.clear();
    emit deleteConfirmChanged();
    deleteFriend(uid);
}

void ChatBridge::cancelDeleteFriend()
{
    _deleteConfirmVisible = false;
    _deleteTargetUid = 0;
    _deleteTargetName.clear();
    emit deleteConfirmChanged();
}

void ChatBridge::openChat(int uid)
{
    auto info = UserMgr::GetInstance()->GetFriendById(uid);
    if (!info)
        return;

    //已经停在这个会话上：只补拉资料与已读，不重置消息模型——
    //重置会把滚动位置打回第一条，长会话里非常难受
    if (uid == _currentUid && _currentKind == static_cast<int>(ChatThreadKind::Private)
        && _mode == ChatMode && _pane == ChatPage) {
        ProfileService::GetInstance()->requestProfile(uid);
        ChatService::GetInstance()->MarkSessionRead(currentConversation());
        return;
    }

    closeAddFriend();
    clearFriendSearch();

    _currentKind = static_cast<int>(ChatThreadKind::Private);
    _currentUid = uid;
    _currentThreadId = info->_chat_thread ? info->_chat_thread->_thread_id : 0;
    ChatService::GetInstance()->ensurePrivateChat(uid);
    //标题与列表共用同一条显示名规则：备注 > 昵称 > 用户名
    _currentName = DisplayName(info->_back, info->_nick, info->_name);
    _currentSubtitle = info->_desc;
    //好友可能刚改过签名，打开会话时拉一次最新资料；失败继续用本地缓存
    ProfileService::GetInstance()->requestProfile(uid);

    // 消息模型切到这条会话，随后触发的 countChanged 会让界面滚到底
    _messages->setConversation(_currentThreadId, _currentKind, uid);
    emit currentChanged();
    emit groupChanged();
    emit chatBlockedChanged();

    setMode(ChatMode);
    setPane(ChatPage);
    // 进入会话即视为已读；之后的推送在 UserMgr 里会跳过这个会话
    refreshActiveThread();
    // 历史可能已经在本地：先把当前已有的最后一条报上去，新消息到达时还会再报一次
    ChatService::GetInstance()->MarkSessionRead(currentConversation());
}

//打开群聊：thread_id 由会话行或建群回包给出。群资料还没到就先拉一次，
//回来后在 infoLoaded 里补打开，避免把用户丢在空态
void ChatBridge::openGroup(qint64 threadId)
{
    if (threadId <= 0)
        return;
    auto group = UserMgr::GetInstance()->GetGroup(threadId);
    if (!group) {
        _pendingOpenGroup = threadId;
        GroupService::GetInstance()->RequestGroupInfo(threadId);
        return;
    }

    if (threadId == _currentThreadId
        && _currentKind == static_cast<int>(ChatThreadKind::Group)
        && _mode == ChatMode && _pane == ChatPage) {
        ChatService::GetInstance()->MarkSessionRead(currentConversation());
        return;
    }

    closeAddFriend();
    clearFriendSearch();
    _pendingOpenGroup = 0;
    _currentKind = static_cast<int>(ChatThreadKind::Group);
    _currentUid = 0;
    _currentThreadId = threadId;
    _currentName = group->_name;
    _currentSubtitle = group->_member_count > 0
        ? tr("%1 人").arg(group->_member_count) : QString();

    _messages->setConversation(threadId, _currentKind, 0);
    emit currentChanged();
    emit groupChanged();
    emit chatBlockedChanged();

    setMode(ChatMode);
    setPane(ChatPage);
    refreshActiveThread();
    ChatService::GetInstance()->MarkSessionRead(currentConversation());

    //成员列表用于气泡名字与 @ 候选；没拉过就补一次
    if (group->_member_map.isEmpty())
        GroupService::GetInstance()->RequestGroupInfo(threadId);
}

ConversationRef ChatBridge::currentConversation() const
{
    ConversationRef conv;
    conv._thread_id = _currentThreadId;
    conv._kind = static_cast<ChatThreadKind>(_currentKind);
    conv._peer_uid = _currentKind == static_cast<int>(ChatThreadKind::Private) ? _currentUid : 0;
    return conv;
}

bool ChatBridge::isGroup() const
{
    return _currentKind == static_cast<int>(ChatThreadKind::Group);
}

QVariantMap ChatBridge::groupInfo() const
{
    auto group = UserMgr::GetInstance()->GetGroup(_currentThreadId);
    if (!group)
        return {};
    return {
        {QStringLiteral("threadId"), QVariant::fromValue(group->_thread_id)},
        {QStringLiteral("name"), group->_name},
        {QStringLiteral("notice"), group->_notice},
        {QStringLiteral("owner"), group->_owner_id},
        {QStringLiteral("memberCount"), group->_member_count},
        {QStringLiteral("myRole"), group->_my_role},
        {QStringLiteral("muted"), group->_muted},
        {QStringLiteral("myNick"), group->_my_nick},
        {QStringLiteral("createdAt"), QVariant::fromValue(group->_created_at)},
    };
}

QVariantList ChatBridge::groupMembers() const
{
    QVariantList list;
    auto group = UserMgr::GetInstance()->GetGroup(_currentThreadId);
    if (!group)
        return list;
    const int self = UserMgr::GetInstance()->GetUid();
    for (auto it = group->_member_map.cbegin(); it != group->_member_map.cend(); ++it) {
        const GroupMember &member = it.value();
        const QString name = !member._group_nick.isEmpty()
            ? member._group_nick : DisplayName(QString(), member._nick, member._name);
        list.append(QVariantMap{
            {QStringLiteral("uid"), member._uid},
            {QStringLiteral("name"), name},
            {QStringLiteral("account"), member._name},
            {QStringLiteral("icon"), member._icon},
            {QStringLiteral("role"), member._role},
            {QStringLiteral("groupNick"), member._group_nick},
            {QStringLiteral("initial"), name.isEmpty() ? QStringLiteral("?") : name.left(1)},
            {QStringLiteral("isSelf"), member._uid == self},
        });
    }
    return list;
}

//@ 候选：除自己外的全部成员，按资料页同一套显示名
QVariantList ChatBridge::mentionCandidates() const
{
    QVariantList list;
    auto group = UserMgr::GetInstance()->GetGroup(_currentThreadId);
    if (!group)
        return list;
    const int self = UserMgr::GetInstance()->GetUid();
    for (auto it = group->_member_map.cbegin(); it != group->_member_map.cend(); ++it) {
        const GroupMember &member = it.value();
        if (member._uid == self)
            continue;
        const QString name = !member._group_nick.isEmpty()
            ? member._group_nick : DisplayName(QString(), member._nick, member._name);
        list.append(QVariantMap{
            {QStringLiteral("uid"), member._uid},
            {QStringLiteral("name"), name},
            {QStringLiteral("icon"), member._icon},
        });
    }
    return list;
}

bool ChatBridge::canManageGroup() const
{
    auto group = UserMgr::GetInstance()->GetGroup(_currentThreadId);
    return group && group->IsAdmin();
}

bool ChatBridge::isGroupOwner() const
{
    auto group = UserMgr::GetInstance()->GetGroup(_currentThreadId);
    return group && group->IsOwner();
}

void ChatBridge::refreshConversationSnapshot()
{
    if (isGroup()) {
        auto group = UserMgr::GetInstance()->GetGroup(_currentThreadId);
        if (!group)
            return;
        _currentName = group->_name;
        _currentSubtitle = group->_member_count > 0
            ? tr("%1 人").arg(group->_member_count) : QString();
    }
    else {
        auto info = UserMgr::GetInstance()->GetFriendById(_currentUid);
        if (!info)
            return;
        _currentName = DisplayName(info->_back, info->_nick, info->_name);
        _currentSubtitle = info->_desc;
    }
    emit currentChanged();
    emit groupChanged();
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

    _lastContactPane = FriendInfoPage;
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

    if (!isGroup() && _currentUid == uid) {
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
    _lastContactPane = ApplyFriendPage;
    setMode(ContactMode);
    setPane(ApplyFriendPage);
}

//区域决定左栏与中栏，页决定右栏；两者必须属于同一区域，
//否则会出现"左边联系人、右边还在聊天"的割裂状态
void ChatBridge::setMode(int mode)
{
    if (_mode == mode)
        return;
    _mode = mode;

    const bool chatPane = _pane == ChatPage || _pane == GroupInfoPage || _pane == EmptyPane;
    const bool contactPane = _pane == ApplyFriendPage || _pane == FriendInfoPage;
    if (mode == ContactMode && !contactPane) {
        setPane(_lastContactPane); //回到联系人区域：恢复上次看的资料/申请页
    } else if (mode == ChatMode && !chatPane) {
        setPane(_currentThreadId > 0 ? ChatPage : EmptyPane);
    } else if (mode == SettingsMode && _pane != UserInfoPage) {
        setPane(UserInfoPage);
    }

    //回到会话模式且仍停在会话页时，恢复"正在看"，否则新消息会被算成未读
    refreshActiveThread();
    emit modeChanged();
}

//当前是否真的在看某个会话：只有会话模式 + 停在会话页 + 有 thread 时才成立。
//统一在这里判断，避免各个入口各写一份、切来切去把状态弄丢
void ChatBridge::refreshActiveThread()
{
    const bool viewing = _mode == ChatMode && _pane == ChatPage && _currentThreadId > 0;
    UserMgr::GetInstance()->SetActiveThread(viewing ? _currentThreadId : 0);
}

void ChatBridge::setPane(int pane)
{
    if (_pane == pane)
        return;
    _pane = pane;
    //只有真正停在会话页才算"正在看"；切到资料页/申请页后新消息重新计入未读
    refreshActiveThread();
    emit paneChanged();
}

// 把编辑器片段送出去：图片先挂本地消息再落库上传，文字直接进协议。
// 任意一段失败就返回 false，QML 因此保留输入内容
bool ChatBridge::sendParts(const QVariantList &parts)
{
    const ConversationRef conv = currentConversation();
    const bool ready = conv._kind == ChatThreadKind::Group
        ? UserMgr::GetInstance()->GetGroup(conv._thread_id) != nullptr
        : UserMgr::GetInstance()->GetFriendById(conv._peer_uid) != nullptr;
    if (!SessionService::GetInstance()->isReady() || !ready)
        return false;

    //单聊关系已断：不再把消息塞进队列。顶部横条说明原因，这里补一条即时提示，
    //让"点发送没反应"变成"明确告诉我发不出去"
    if (conv._kind == ChatThreadKind::Private) {
        auto info = UserMgr::GetInstance()->GetFriendById(conv._peer_uid);
        if (info && !info->_is_friend) {
            _chatNotice = tr("你已删除对方，发送朋友验证后才能继续聊天");
            emit chatNoticeChanged();
            return false;
        }
    }

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
    //回执到达后再把资源交给资源服务器。@ 名单只挂在文本段上
    QStringList sent_uploads; //整批成功后才删输入区的临时压缩图
    for (const QVariant &part : parts) {
        const QVariantMap item = part.toMap();
        const QString kind = item.value("kind").toString();
        if (kind == "image") {
            const QString upload = item.value("upload").toString();
            //source 是气泡缩略图，upload 是输入区预先压缩好的上传文件
            if (!ChatService::GetInstance()->sendImage(conv, item.value("source").toString(),
                    upload, item.value("width").toInt(), item.value("height").toInt()))
                return false;
            if (!upload.isEmpty())
                sent_uploads.append(upload);
        }
        else if (kind == "video") {
            if (!ChatService::GetInstance()->sendVideo(conv,
                    item.value("path").toString(), item.value("name").toString(),
                    item.value("size").toLongLong()))
                return false;
        }
        else if (kind == "file") {
            if (!ChatService::GetInstance()->sendFile(conv,
                    item.value("path").toString(), item.value("name").toString(),
                    item.value("size").toLongLong()))
                return false;
        }
        else if (!ChatService::GetInstance()->sendText(conv,
                     item.value("text").toString(),
                     item.value("mentions").toStringList())) {
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
    auto thread = UserMgr::GetInstance()->ThreadData(_currentThreadId);
    if (!thread)
        return items;

    for (const auto &message : thread->_chat_msgs) {
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

// ---- 群聊 ----

//候选人是全部好友：群只能从好友里发起，和微信一致
QVariantList ChatBridge::groupCandidates() const
{
    QVariantList list;
    auto *mgr = UserMgr::GetInstance().get();
    const int count = mgr->FriendCount();
    for (int i = 0; i < count; ++i) {
        auto info = mgr->FriendAt(i);
        if (!info)
            continue;
        const QString name = DisplayName(info->_back, info->_nick, info->_name);
        list.append(QVariantMap{
            {QStringLiteral("uid"), info->_uid},
            {QStringLiteral("name"), name},
            {QStringLiteral("account"), info->_name},
            {QStringLiteral("icon"), info->_icon},
            {QStringLiteral("initial"), name.isEmpty() ? QStringLiteral("?") : name.left(1)},
        });
    }
    return list;
}

void ChatBridge::openCreateGroup()
{
    _createGroupOpen = true;
    _createGroupBusy = false;
    _createGroupMessage.clear();
    emit createGroupChanged();
}

void ChatBridge::closeCreateGroup()
{
    //请求在途时不允许关掉，回包还要落到界面上；已经关过就不再重复发信号
    if (_createGroupBusy || !_createGroupOpen)
        return;
    _createGroupOpen = false;
    _createGroupMessage.clear();
    emit createGroupChanged();
}

void ChatBridge::createGroup(const QString &name, const QVariantList &uids)
{
    if (_createGroupBusy)
        return;
    QList<int> members;
    for (const QVariant &value : uids) {
        const int uid = value.toInt();
        if (uid > 0 && !members.contains(uid))
            members.append(uid);
    }
    if (members.isEmpty()) {
        _createGroupMessage = tr("至少选择一位好友");
        emit createGroupChanged();
        return;
    }
    _createGroupBusy = true;
    _createGroupMessage = tr("正在创建…");
    emit createGroupChanged();
    GroupService::GetInstance()->CreateGroup(name.trimmed(), members);
}

//群资料页：从聊天标题进入，进页先保证成员列表是最新的
void ChatBridge::openGroupInfo()
{
    if (!isGroup() || _currentThreadId <= 0)
        return;
    setMode(ChatMode);
    setPane(GroupInfoPage);
    _groupMessage.clear();
    setGroupState(false, QString());
    GroupService::GetInstance()->RequestGroupInfo(_currentThreadId);
}

void ChatBridge::backToChat()
{
    if (_currentThreadId <= 0)
        return;
    setMode(ChatMode);
    setPane(ChatPage);
    refreshActiveThread();
}

void ChatBridge::updateGroupField(const QString &action, const QString &value)
{
    if (!isGroup() || _currentThreadId <= 0)
        return;
    setGroupState(true, QString());
    GroupService::GetInstance()->UpdateGroup(_currentThreadId, action, value);
}

void ChatBridge::toggleGroupMute()
{
    auto group = UserMgr::GetInstance()->GetGroup(_currentThreadId);
    if (!group)
        return;
    updateGroupField(QStringLiteral("mute"), group->_muted ? QStringLiteral("0")
                                                          : QStringLiteral("1"));
}

void ChatBridge::inviteGroupMembers(const QVariantList &uids)
{
    if (!isGroup() || _currentThreadId <= 0)
        return;
    QList<int> members;
    for (const QVariant &value : uids) {
        const int uid = value.toInt();
        if (uid > 0 && !members.contains(uid))
            members.append(uid);
    }
    if (members.isEmpty())
        return;
    setGroupState(true, QString());
    GroupService::GetInstance()->InviteMembers(_currentThreadId, members);
}

void ChatBridge::removeGroupMember(int uid)
{
    if (!isGroup() || _currentThreadId <= 0 || uid <= 0)
        return;
    setGroupState(true, QString());
    GroupService::GetInstance()->RemoveMember(_currentThreadId, uid);
}

void ChatBridge::setGroupAdmin(int uid, bool admin)
{
    if (!isGroup() || _currentThreadId <= 0 || uid <= 0)
        return;
    setGroupState(true, QString());
    GroupService::GetInstance()->SetAdmin(_currentThreadId, uid, admin);
}

void ChatBridge::leaveGroup()
{
    if (!isGroup() || _currentThreadId <= 0)
        return;
    setGroupState(true, QString());
    GroupService::GetInstance()->LeaveGroup(_currentThreadId);
}

void ChatBridge::dismissGroup()
{
    if (!isGroup() || _currentThreadId <= 0)
        return;
    setGroupState(true, QString());
    GroupService::GetInstance()->DismissGroup(_currentThreadId);
}

void ChatBridge::clearGroupMessage()
{
    if (_groupMessage.isEmpty())
        return;
    _groupMessage.clear();
    emit groupStateChanged();
}

void ChatBridge::setGroupState(bool busy, const QString &message)
{
    _groupBusy = busy;
    //成功时不带文案，失败时保留服务端的原因
    if (!message.isEmpty())
        _groupMessage = message;
    emit groupStateChanged();
}
