#ifndef CHATBRIDGE_H
#define CHATBRIDGE_H

#include <QObject>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include "applylistmodel.h"
#include "conversationfiltermodel.h"
#include "conversationmodel.h"
#include "friendlistmodel.h"
#include "grouplistmodel.h"
#include "messagemodel.h"

/**
 * @brief 聊天主界面的桥接对象
 *
 * 界面左边显示什么、右边显示什么，全部由这里的 mode / pane 决定：
 * 侧栏点「聊天」或「联系人」会同时改这两项，列表点条目则改对应的一项。
 * QML 只做渲染，自己不保存导航状态。
 *
 *   点侧栏「聊天」   → mode=会话，右侧回到当前会话（没有会话则为空）
 *   点侧栏「联系人」 → mode=联系人，右侧显示「新的朋友」
 *   会话条目         → 右侧聊天页（单聊或群聊）
 *   联系人条目       → 右侧好友资料页
 *   群聊标题         → 右侧群资料页
 *
 * 当前会话用 thread_id 标识：单聊的对方 uid 只是额外信息，群聊没有"对方"。
 * 同时托管：会话模型、联系人模型、申请模型、消息模型、联系人页的加好友浮层、
 * 发起群聊与群资料页的状态，以及发消息、加好友、同意申请、群管理的协议调用。
 */
class ChatBridge : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(ConversationModel *conversations READ conversations CONSTANT)
    // 聊天列表用过滤视图；转发等场景继续用完整 conversations
    Q_PROPERTY(ConversationFilterModel *filteredConversations READ filteredConversations CONSTANT)
    Q_PROPERTY(FriendListModel *friends READ friends CONSTANT)
    Q_PROPERTY(GroupListModel *groups READ groups CONSTANT)
    Q_PROPERTY(ApplyListModel *applies READ applies CONSTANT)
    Q_PROPERTY(MessageModel *messages READ messages CONSTANT)
    Q_PROPERTY(int mode READ mode NOTIFY modeChanged)
    Q_PROPERTY(int pane READ pane NOTIFY paneChanged)
    Q_PROPERTY(QVariantMap profile READ profile NOTIFY profileChanged)
    //当前会话：thread 是主键；单聊另有 uid，群聊另有群资料
    Q_PROPERTY(qint64 currentThreadId READ currentThreadId NOTIFY currentChanged)
    Q_PROPERTY(int currentKind READ currentKind NOTIFY currentChanged)
    Q_PROPERTY(int currentUid READ currentUid NOTIFY currentChanged)
    Q_PROPERTY(QString currentIcon READ currentIcon NOTIFY currentChanged)
    Q_PROPERTY(QString currentName READ currentName NOTIFY currentChanged)
    Q_PROPERTY(QString currentSubtitle READ currentSubtitle NOTIFY currentChanged)
    Q_PROPERTY(bool isGroup READ isGroup NOTIFY currentChanged)
    Q_PROPERTY(QVariantMap groupInfo READ groupInfo NOTIFY groupChanged)
    Q_PROPERTY(QVariantList groupMembers READ groupMembers NOTIFY groupChanged)
    Q_PROPERTY(bool canManageGroup READ canManageGroup NOTIFY groupChanged)
    Q_PROPERTY(bool isGroupOwner READ isGroupOwner NOTIFY groupChanged)
    Q_PROPERTY(QStringList tagOptions READ tagOptions NOTIFY tagFilterChanged)
    Q_PROPERTY(QString tagFilter READ tagFilter NOTIFY tagFilterChanged)
    Q_PROPERTY(int unreadTotal READ unreadTotal NOTIFY unreadChanged)
    Q_PROPERTY(QString chatNotice READ chatNotice NOTIFY chatNoticeChanged)
    //单聊被关系问题挡住时的常驻提示；为空表示可以正常发送
    Q_PROPERTY(QString chatBlockedText READ chatBlockedText NOTIFY chatBlockedChanged)
    //清除聊天记录的二次确认：标题里带上会话名，避免误清别的会话
    Q_PROPERTY(bool clearConfirmVisible READ clearConfirmVisible NOTIFY clearConfirmChanged)
    Q_PROPERTY(QString clearConfirmTitle READ clearConfirmTitle NOTIFY clearConfirmChanged)
    //同一个确认层也承接"删除会话"：文案与按钮跟着这个开关切换
    Q_PROPERTY(bool clearConfirmDelete READ clearConfirmDelete NOTIFY clearConfirmChanged)
    Q_PROPERTY(bool logoutConfirmVisible READ logoutConfirmVisible NOTIFY logoutConfirmChanged)
    //进入会话时的未读数与第一条未读消息号：消息列表据此画"以下 N 条未读"
    Q_PROPERTY(int entryUnread READ entryUnread NOTIFY currentChanged)
    Q_PROPERTY(qint64 entryDividerId READ entryDividerId NOTIFY currentChanged)
    Q_PROPERTY(QString myIcon READ myIcon NOTIFY myProfileChanged)
    Q_PROPERTY(QVariantMap myProfile READ myProfile NOTIFY myProfileChanged)
    Q_PROPERTY(QString myName READ myName CONSTANT)
    Q_PROPERTY(QString myUserName READ myUserName CONSTANT)
    Q_PROPERTY(QString searchText READ searchText WRITE setSearchText NOTIFY searchTextChanged)
    Q_PROPERTY(int searchState READ searchState NOTIFY searchStateChanged)
    Q_PROPERTY(QVariantMap searchResult READ searchResult NOTIFY searchResultChanged)
    Q_PROPERTY(QString searchMessage READ searchMessage NOTIFY searchStateChanged)
    //本地消息搜索：结果项带会话名、发送者、摘要与时间；点击后定位到原消息
    Q_PROPERTY(QVariantList messageSearchResults READ messageSearchResults NOTIFY messageSearchChanged)
    Q_PROPERTY(int messageSearchCount READ messageSearchCount NOTIFY messageSearchChanged)
    Q_PROPERTY(qint64 scrollToMessageId READ scrollToMessageId NOTIFY scrollToMessageChanged)
    Q_PROPERTY(bool addFriendOpen READ addFriendOpen NOTIFY addFriendOpenChanged)
    Q_PROPERTY(int applyState READ applyState NOTIFY applyStateChanged)
    Q_PROPERTY(QString applyMessage READ applyMessage NOTIFY applyStateChanged)
    Q_PROPERTY(bool avatarUploading READ avatarUploading NOTIFY avatarSyncChanged)
    Q_PROPERTY(bool avatarUploadPaused READ avatarUploadPaused NOTIFY avatarSyncChanged)
    Q_PROPERTY(int avatarProgress READ avatarProgress NOTIFY avatarSyncChanged)
    Q_PROPERTY(QString avatarMessage READ avatarMessage NOTIFY avatarSyncChanged)
    Q_PROPERTY(bool profileSaving READ profileSaving NOTIFY profileStateChanged)
    Q_PROPERTY(QString profileMessage READ profileMessage NOTIFY profileStateChanged)
    Q_PROPERTY(bool remarkSaving READ remarkSaving NOTIFY remarkStateChanged)
    Q_PROPERTY(QString remarkMessage READ remarkMessage NOTIFY remarkStateChanged)
    //发起群聊：候选人是全部好友，勾选后在弹层里输入群名
    Q_PROPERTY(bool createGroupOpen READ createGroupOpen NOTIFY createGroupChanged)
    Q_PROPERTY(QVariantList groupCandidates READ groupCandidates NOTIFY createGroupChanged)
    Q_PROPERTY(bool createGroupBusy READ createGroupBusy NOTIFY createGroupChanged)
    Q_PROPERTY(QString createGroupMessage READ createGroupMessage NOTIFY createGroupChanged)
    //群资料页与群操作的结果提示
    Q_PROPERTY(QString groupMessage READ groupMessage NOTIFY groupStateChanged)
    Q_PROPERTY(bool groupBusy READ groupBusy NOTIFY groupStateChanged)
    //当前会话里可 @ 的成员（群聊用，排除自己）
    Q_PROPERTY(QVariantList mentionCandidates READ mentionCandidates NOTIFY groupChanged)

public:
    //左侧栏目：会话 / 联系人 / 设置
    enum Mode { ChatMode = 0, ContactMode = 1, SettingsMode = 2 };
    //右侧页面取值与 QML 的 pane 判断一致
    enum Pane { EmptyPane = 0, ChatPage = 1, FriendInfoPage = 2, ApplyFriendPage = 3,
                UserInfoPage = 4, GroupInfoPage = 5, GeneralPage = 6 };
    //联系人页加好友浮层的展示状态，QML 按这些名字切换结果区域
    enum SearchState { SearchIdle = 0, SearchSearching, SearchStranger, SearchNotFound,
                       SearchSelf, SearchAlreadyFriend, SearchAlreadyApplied };
    //申请发送状态：浮层据此显示发送中、已发送和失败重试
    enum ApplyState { ApplyIdle = 0, ApplySending, ApplySent, ApplyFailed };
    Q_ENUM(Mode)
    Q_ENUM(Pane)
    Q_ENUM(SearchState)
    Q_ENUM(ApplyState)

    explicit ChatBridge(QObject *parent = nullptr);

    ConversationModel *conversations() const { return _conversations; }
    ConversationFilterModel *filteredConversations() const { return _filteredConversations; }
    FriendListModel *friends() const { return _friends; }
    GroupListModel *groups() const { return _groups; }
    ApplyListModel *applies() const { return _applies; }
    MessageModel *messages() const { return _messages; }

    int mode() const { return _mode; }
    int pane() const { return _pane; }
    QVariantMap profile() const { return _profile; }

    // 当前会话
    qint64 currentThreadId() const { return _currentThreadId; }
    int currentKind() const { return _currentKind; }
    int currentUid() const { return _currentUid; }
    QString currentIcon() const;
    QString currentName() const { return _currentName; }
    QString currentSubtitle() const { return _currentSubtitle; }
    bool isGroup() const;
    // 上滑到顶时由界面调用：从本地库再取一页更早的消息。
    //库里已经没有时会立即返回，代价只是一次带索引的空查询
    Q_INVOKABLE void loadOlderMessages();
    QVariantMap groupInfo() const;
    QVariantList groupMembers() const;
    QVariantList mentionCandidates() const;
    bool canManageGroup() const;
    bool isGroupOwner() const;
    QString groupMessage() const { return _groupMessage; }
    bool groupBusy() const { return _groupBusy; }

    // 联系人页的标签筛选：选项来自全部好友标签的并集，空串表示不筛选
    QStringList tagOptions() const;
    QString tagFilter() const { return _tagFilter; }
    Q_INVOKABLE void setTagFilter(const QString &tag);
    Q_INVOKABLE void clearTagFilter();

    // 左侧栏「会话」徽标用的总未读数
    int unreadTotal() const;
    // 一次性提示（如"对方已不是你的好友"），界面弹完即清
    QString chatNotice() const { return _chatNotice; }
    Q_INVOKABLE void clearChatNotice();
    //清除聊天记录：先弹确认，确认后只清本地，不动服务端与其他设备
    Q_INVOKABLE void askClearHistory(qint64 threadId, int kind, const QString &title);
    Q_INVOKABLE void confirmClearHistory();
    Q_INVOKABLE void cancelClearHistory();
    //删除会话：清本机记录并隐藏会话，好友关系与服务端记录都保留
    Q_INVOKABLE void askDeleteConversation(qint64 threadId, int kind, const QString &title);
    //本机会话整理
    Q_INVOKABLE void setThreadPinned(qint64 threadId, bool pinned);
    Q_INVOKABLE void setThreadMuted(qint64 threadId, bool muted);
    Q_INVOKABLE void markThreadUnread(qint64 threadId);
    //设置-通用：清空全部本地聊天记录 / 清空已下载的媒体文件
    Q_INVOKABLE void clearAllHistory();
    Q_INVOKABLE void clearMediaCache();
    // 单聊关系断开时的常驻提示：已删除对方、被删除后发送失败、验证已发出
    QString chatBlockedText() const;
    bool clearConfirmVisible() const { return _clearConfirmVisible; }
    QString clearConfirmTitle() const { return _clearConfirmTitle; }
    bool clearConfirmDelete() const { return _clearConfirmDelete; }
    int entryUnread() const { return _entryUnread; }
    qint64 entryDividerId() const { return _entryDividerId; }
    //窗口是否在前台、消息列表是否在底部：只有两者都成立才算"正在看"，
    //否则新消息要正常计未读，不能因为会话页还开着就当成已读
    Q_INVOKABLE void setWindowActive(bool active);
    Q_INVOKABLE void setChatAtBottom(bool atBottom);
    // 在当前会话里直接发一条朋友验证（不切换区域），验证内容由弹层传入
    Q_INVOKABLE void sendReAdd(const QString &text);

    // 自己：myName 用于显示（昵称优先），myUserName 用于申请文案（用户名）
    QString myIcon() const;
    QVariantMap myProfile() const;
    QString myName() const;
    QString myUserName() const;

    QString searchText() const { return _searchText; }
    void setSearchText(const QString &text);

    // 加好友搜索状态，界面按 SearchState 枚举切换结果区域
    int searchState() const { return _searchState; }
    QVariantMap searchResult() const { return _searchResult; }
    QString searchMessage() const { return _searchMessage; }
    QVariantList messageSearchResults() const { return _messageSearchResults; }
    int messageSearchCount() const { return _messageSearchResults.size(); }
    qint64 scrollToMessageId() const { return _scrollToMessageId; }

    // 联系人页的加好友浮层；打开时搜索框保持焦点
    bool addFriendOpen() const { return _addFriendOpen; }
    Q_INVOKABLE void openAddFriend();
    Q_INVOKABLE void closeAddFriend();


    // 申请发送状态：浮层显示「发送中 / 已发送 / 失败」
    int applyState() const { return _applyState; }
    QString applyMessage() const { return _applyMessage; }
    Q_INVOKABLE void retryApply();

    //头像同步：上传进度与暂停/继续
    bool avatarUploading() const;
    bool avatarUploadPaused() const;
    int avatarProgress() const;
    QString avatarMessage() const;
    Q_INVOKABLE void toggleAvatarUpload();

    //资料编辑：本人昵称/性别/签名，以及自己给好友设的备注
    bool profileSaving() const { return _profileSaving; }
    QString profileMessage() const { return _profileMessage; }
    bool remarkSaving() const { return _remarkSaving; }
    QString remarkMessage() const { return _remarkMessage; }
    Q_INVOKABLE void saveMyProfile(const QString &nick, int sex, const QString &desc);
    // 备注与标签一起保存，标签是"我给好友打的分组"
    Q_INVOKABLE void saveFriendRemark(int uid, const QString &remark, const QStringList &tags);
    Q_INVOKABLE void clearProfileMessages();

    //侧栏导航
    Q_INVOKABLE void showChatMode();
    Q_INVOKABLE void showContactMode();
    Q_INVOKABLE void showSettings();
    Q_INVOKABLE void openMyProfile();
    Q_INVOKABLE void openGeneralSettings();

    //好友页面入口
    Q_INVOKABLE void openChat(int uid);
    // 打开群聊：thread_id 已经确定，不再走建会话请求
    Q_INVOKABLE void openGroup(qint64 threadId);
    // 再次点击当前会话：收起聊天回到空态；草稿与历史仍在本机，重新点开可恢复
    Q_INVOKABLE void clearConversation();
    //通知点击：按会话类型回到对应聊天；私聊优先用 peer uid，兼容还没有 thread_id 的老数据
    Q_INVOKABLE void openThread(qint64 threadId, int kind, int peerUid);
    //多选转发：keys 是 {msgId, messageId} 列表，返回实际发出的条数
    //合并转发：把选中的消息打成一份聊天记录快照，作为一条 type=7 的消息发出
    Q_INVOKABLE int mergeForwardMessages(const QVariantList &keys, qint64 targetThreadId,
        int targetKind, int targetPeerUid);
    Q_INVOKABLE int forwardMessages(const QVariantList &keys, qint64 targetThreadId,
                                    int targetKind, int targetPeerUid);
    //本机删除：只删当前会话的本地记录与缓存，服务端与对方不受影响
    Q_INVOKABLE int deleteMessages(const QVariantList &keys);
    // 撤回自己发的消息：服务端校验时间窗，成功后双方都换成提示行
    Q_INVOKABLE void recallMessage(qint64 messageId);
    // 重发一条失败的消息：文本直接重发，图片/视频/文件用本机文件重新发送
    // 0 成功重发, 1 关系已断开, 2 本地文件缺失或消息不可重发
    Q_INVOKABLE int retryMessage(const QString &msgId);
    // 退出登录：断开长连接、清空账号数据、回到登录页
    Q_INVOKABLE void logout();
    //退出登录不可撤销：先确认，再交给 ShellBridge 真正断开
    Q_INVOKABLE void askLogout();
    Q_INVOKABLE void confirmLogout();
    Q_INVOKABLE void cancelLogout();
    bool logoutConfirmVisible() const { return _logoutConfirmVisible; }
    //好友资料页返回：从聊天进来就回会话，从联系人进来就回联系人空态
    Q_INVOKABLE void backFromProfile();
    // 删除好友：双向删除，不通知对方；会话与历史保留
    Q_INVOKABLE void deleteFriend(int uid);
    // 删除确认：入口只负责发起，真正的删除等用户确认后再执行
    Q_PROPERTY(bool deleteConfirmVisible READ deleteConfirmVisible NOTIFY deleteConfirmChanged)
    Q_PROPERTY(QString deleteTargetName READ deleteTargetName NOTIFY deleteConfirmChanged)
    Q_INVOKABLE void askDeleteFriend(int uid);
    Q_INVOKABLE void confirmDeleteFriend();
    Q_INVOKABLE void cancelDeleteFriend();
    bool deleteConfirmVisible() const { return _deleteConfirmVisible; }
    QString deleteTargetName() const { return _deleteTargetName; }
    Q_INVOKABLE void openProfile(int uid);
    //从聊天页标题进入资料：返回时回到当前会话，而不是联系人列表
    Q_INVOKABLE void openProfileFromChat(int uid);
    Q_INVOKABLE void openApplyList();

    // ---- 群聊 ----
    bool createGroupOpen() const { return _createGroupOpen; }
    QVariantList groupCandidates() const;
    bool createGroupBusy() const { return _createGroupBusy; }
    QString createGroupMessage() const { return _createGroupMessage; }
    Q_INVOKABLE void openCreateGroup();
    Q_INVOKABLE void closeCreateGroup();
    //uids 是勾选的好友；空群名由服务端回落到"群聊"
    Q_INVOKABLE void createGroup(const QString &name, const QVariantList &uids);
    //打开/刷新群资料页
    Q_INVOKABLE void openGroupInfo();
    //联系人页点群行：先切到该群，再进群资料，不经过聊天页
    Q_INVOKABLE void openGroupInfoFor(qint64 threadId);
    // 再次点击已选中的好友/群：右侧回到联系人空态，左侧列表不动
    Q_INVOKABLE void clearContactSelection();
    //从联系人里的群资料退回联系人列表（不切到聊天）
    Q_INVOKABLE void backToContactList();
    //从群资料页回到会话，不重置消息模型与滚动位置
    Q_INVOKABLE void backToChat();
    //action: name / notice / nickname / mute
    Q_INVOKABLE void updateGroupField(const QString &action, const QString &value);
    //群头像：本地裁成 512 PNG 后上传，成功后把资源名写回群资料
    Q_INVOKABLE bool updateGroupIcon(const QUrl &fileUrl);
    Q_INVOKABLE void toggleGroupMute();
    Q_INVOKABLE void inviteGroupMembers(const QVariantList &uids);
    Q_INVOKABLE void removeGroupMember(int uid);
    Q_INVOKABLE void setGroupAdmin(int uid, bool admin);
    Q_INVOKABLE void leaveGroup();
    Q_INVOKABLE void dismissGroup();
    Q_INVOKABLE void clearGroupMessage();

    // 业务动作
    Q_INVOKABLE bool sendParts(const QVariantList &parts);
    //视频/文件：下载、暂停/继续、本机路径、打开与另存为
    Q_INVOKABLE void downloadResource(qint64 messageId);
    Q_INVOKABLE void toggleResourceTransfer(qint64 messageId, const QString &msgId = {});
    Q_INVOKABLE QString resourceUrl(qint64 messageId); //查看器用的 file URL
    Q_INVOKABLE bool openResource(qint64 messageId);
    //返回值：0 失败，1 已复制到目标，2 资源未下载，已排队并在完成后自动保存
    Q_INVOKABLE int saveResourceAs(qint64 messageId, const QUrl &target);
    Q_INVOKABLE bool revealResource(qint64 messageId); //在系统文件管理器中定位
    Q_INVOKABLE void markResourceMissing(qint64 messageId); //本地文件丢失时退回未下载
    Q_INVOKABLE bool copyText(const QString &text);
    //优先复制完整图片，缺失时退回气泡里的 data/file URL
    Q_INVOKABLE bool copyImage(qint64 messageId, const QUrl &fallback = {});
    //视频/文件：把本地文件放进剪贴板，粘贴到 Finder/资源管理器即可
    Q_INVOKABLE bool copyResourceFile(qint64 messageId);
    //当前会话可查看的图片与已下载视频，按消息顺序排列，供查看器左右切换
    Q_INVOKABLE QVariantList mediaItems();
    Q_INVOKABLE void search();
    Q_INVOKABLE void clearSearch();
    //本地消息搜索：query 为空即清空；currentOnly 只搜当前会话
    Q_INVOKABLE void searchMessages(const QString &query, bool currentOnly = false);
    Q_INVOKABLE void clearMessageSearch();
    //点击搜索结果：切到对应会话并按消息号装载上下文
    Q_INVOKABLE void openMessageResult(qint64 threadId, int kind, int peerUid, qint64 messageId);
    Q_INVOKABLE int messageRow(qint64 messageId) const;
    //消息列表定位完成后调用，清掉一次性滚动请求
    Q_INVOKABLE void clearScrollToMessage();
    // 发出申请；name/nick/icon 用于在「新的朋友」里立刻显示这条记录，tags 是给对方打的分组
    Q_INVOKABLE void applyTo(int uid, const QString &name, const QString &nick,
                             const QString &icon, const QString &verify, const QString &remark,
                             const QStringList &tags);
    Q_INVOKABLE void authApply(int uid, const QString &remark, const QStringList &tags);

signals:
    void modeChanged();
    void paneChanged();
    void profileChanged();
    void myProfileChanged();
    void currentChanged();
    void groupChanged();
    void groupStateChanged();
    //单字段保存回执：action 为 name/notice/nickname/mute，失败时 message 带原因
    void groupFieldSaved(bool ok, const QString &action, const QString &message);
    void groupIconFailed(const QString &message);
    void createGroupChanged();
    void searchTextChanged();
    void searchStateChanged();
    void searchResultChanged();
    void messageSearchChanged();
    void scrollToMessageChanged();
    void addFriendOpenChanged();
    void applyStateChanged();
    void avatarSyncChanged();
    //资料与备注的保存状态；成功后 QML 关闭弹层，失败时保留表单并显示 message
    void profileStateChanged();
    void remarkStateChanged();
    void profileSaved(bool ok);
    void remarkSaved(bool ok, int uid);
    //标签选项或当前筛选变化：联系人页的筛选条与列表一起刷新
    void tagFilterChanged();
    //未读总数变化：左侧栏「会话」徽标刷新
    void unreadChanged();
    //一次性提示变化：聊天页弹一条短提示
    void chatNoticeChanged();
    //单聊关系提示变化：聊天页顶部横条显示/隐藏
    void chatBlockedChanged();
    void clearConfirmChanged();
    void logoutConfirmChanged();
    //删除确认弹层开关
    void deleteConfirmChanged();
    //另存为的最终结果：异步下载完成或失败后回到界面提示
    void resourceSaved(bool ok, const QString &path);
    void resourceNotice(const QString &message); //本地文件缺失、另存位置失效等提示

private:
    // 按当前模式与页面重新判断"正在看哪个会话"，供未读计数使用
    void refreshActiveThread();
    //进入会话时记下未读起点：第一条未读的服务端消息号，供消息列表画分割线
    void captureEntryUnread(qint64 thread_id);
    void navigate(int mode, int pane);
    void setPane(int pane);
    //按当前会话重建标题、副标题与群资料快照
    void refreshConversationSnapshot();
    //好友关系侧数据变化后，刷新聊天标题、副标题与打开中的资料页
    void refreshFriendSnapshot(int uid);
    //按好友当前数据重建资料页快照，openProfile 与刷新共用
    void rebuildProfile(const std::shared_ptr<FriendInfo> &info);
    void setApplyState(int state, const QString &message = {});
    void setGroupState(bool busy, const QString &message);
    void processPendingSaves(); //消息下载完成后处理排队的另存为
    static bool copyToPath(const QString &source, const QString &destination);
    QString savedPath(qint64 messageId); //已另存的位置，文件不存在时清掉记录并返回空
    ConversationRef currentConversation() const;

    ConversationModel *_conversations = nullptr;
    ConversationFilterModel *_filteredConversations = nullptr;
    FriendListModel *_friends = nullptr;
    GroupListModel *_groups = nullptr;
    ApplyListModel *_applies = nullptr;
    MessageModel *_messages = nullptr;

    int _mode = ChatMode;
    int _pane = EmptyPane;
    QVariantMap _profile; // 资料页展示的好友信息

    qint64 _currentThreadId = 0;
    int _currentKind = 1;
    int _currentUid = 0;
    QString _currentName;
    QString _currentSubtitle; //单聊是好友签名，群聊是成员数
    QString _tagFilter;       //联系人页当前选中的标签，空串表示全部
    QString _chatNotice;      //一次性提示，界面显示后调用 clearChatNotice 清掉
    int _notFriendUid = 0;    //最近一次因"对方不是好友"发送失败的会话对象
    int _reAddSentUid = 0;    //已经从这里发过验证的会话对象，提示改成"等待通过"
    int _lastContactPane = EmptyPane; //联系人区域的停留页；默认占位，选中联系人后才切资料页
    bool _deleteConfirmVisible = false;
    bool _clearConfirmVisible = false;
    bool _clearConfirmDelete = false;
    bool _logoutConfirmVisible = false;
    qint64 _clearThreadId = 0;
    int _clearKind = 1;
    QString _clearConfirmTitle;
    //进入会话时的未读起点：只在这条会话里有效，切走后由下一次打开覆盖
    int _entryUnread = 0;
    qint64 _entryDividerId = 0;
    bool _windowActive = true;
    bool _chatAtBottom = true;
    int _lastSettingsPane = UserInfoPage; //设置区上次停留的页，切回来时恢复
    //通讯录的选中项独立于聊天内查看的资料
    int _contactProfileUid = 0;
    int _deleteTargetUid = 0;
    QString _deleteTargetName;
    QString _searchText;
    int _searchState = SearchIdle;
    QVariantMap _searchResult;
    QString _searchMessage;
    QVariantList _messageSearchResults;
    qint64 _scrollToMessageId = 0;
    QString _pendingQuery; // 本次在途搜索的关键词，用于丢弃改词后回来的旧结果
    QString _pendingGroupIcon; //正在上传的群头像资源名
    qint64 _pendingGroupThread = 0;
    QString _pendingGroupIconPath;
    bool _addFriendOpen = false;

    // 最近一次申请的目标与文案，失败重试时原样再发
    int _applyUid = 0;
    QString _applyName;
    QString _applyNick;
    QString _applyIcon;
    QString _applyVerify;
    QString _applyRemark;
    QStringList _applyTags; //本次申请选的分组，失败重试时原样再发
    int _applyState = ApplyIdle;
    QString _applyMessage;

    //资料保存状态与最近一次错误提示；两个弹层互斥打开，但状态各自独立
    bool _profileSaving = false;
    QString _profileMessage;
    bool _remarkSaving = false;
    QString _remarkMessage;

    //群聊：发起弹层与群资料页的状态
    bool _createGroupOpen = false;
    bool _createGroupBusy = false;
    QString _createGroupMessage;
    bool _groupBusy = false;
    QString _groupMessage;
    qint64 _pendingOpenGroup = 0; //群资料还没回来时先记下要打开的群

    //资源还没下载完时的另存为目标：下载完成后自动复制，失败则丢弃
    QHash<qint64, QString> _pending_saves;
    //已经另存到用户目录的位置：打开/定位优先用它，而不是账号缓存
    QHash<qint64, QString> _saved_paths;
};

#endif // CHATBRIDGE_H
