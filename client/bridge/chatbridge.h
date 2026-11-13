#ifndef CHATBRIDGE_H
#define CHATBRIDGE_H

#include <QObject>
#include <QHash>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include "friendlistmodel.h"
#include "applylistmodel.h"
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
 *   会话条目         → 右侧聊天页
 *   联系人条目       → 右侧好友资料页
 *   资料页「发消息」 → 回到会话页
 *
 * 同时托管：三个列表模型（会话与联系人共用一个好友模型，申请单独一个）、
 * 联系人页的加好友浮层状态、聊天页的好友过滤，以及发消息、加好友、同意申请的协议调用。
 */
class ChatBridge : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(FriendListModel *friends READ friends CONSTANT)
    Q_PROPERTY(ApplyListModel *applies READ applies CONSTANT)
    Q_PROPERTY(MessageModel *messages READ messages CONSTANT)
    Q_PROPERTY(int mode READ mode NOTIFY modeChanged)
    Q_PROPERTY(int pane READ pane NOTIFY paneChanged)
    Q_PROPERTY(QVariantMap profile READ profile NOTIFY profileChanged)
    Q_PROPERTY(int currentUid READ currentUid NOTIFY currentChanged)
    Q_PROPERTY(QString currentName READ currentName NOTIFY currentChanged)
    Q_PROPERTY(QString myIcon READ myIcon NOTIFY myProfileChanged)
    Q_PROPERTY(QVariantMap myProfile READ myProfile NOTIFY myProfileChanged)
    Q_PROPERTY(QString myName READ myName CONSTANT)
    Q_PROPERTY(QString myUserName READ myUserName CONSTANT)
    Q_PROPERTY(QString searchText READ searchText WRITE setSearchText NOTIFY searchTextChanged)
    Q_PROPERTY(int searchState READ searchState NOTIFY searchStateChanged)
    Q_PROPERTY(QVariantMap searchResult READ searchResult NOTIFY searchResultChanged)
    Q_PROPERTY(QString searchMessage READ searchMessage NOTIFY searchStateChanged)
    Q_PROPERTY(bool addFriendOpen READ addFriendOpen NOTIFY addFriendOpenChanged)
    Q_PROPERTY(QString friendQuery READ friendQuery WRITE setFriendQuery NOTIFY friendQueryChanged)
    Q_PROPERTY(QVariantList friendMatches READ friendMatches NOTIFY friendMatchesChanged)
    Q_PROPERTY(int applyState READ applyState NOTIFY applyStateChanged)
    Q_PROPERTY(QString applyMessage READ applyMessage NOTIFY applyStateChanged)
    Q_PROPERTY(bool avatarUploading READ avatarUploading NOTIFY avatarSyncChanged)
    Q_PROPERTY(bool avatarUploadPaused READ avatarUploadPaused NOTIFY avatarSyncChanged)
    Q_PROPERTY(int avatarProgress READ avatarProgress NOTIFY avatarSyncChanged)
    Q_PROPERTY(QString avatarMessage READ avatarMessage NOTIFY avatarSyncChanged)

public:
    //左侧栏目：会话 / 联系人 / 设置
    enum Mode { ChatMode = 0, ContactMode = 1, SettingsMode = 2 };
    //右侧页面取值与 QML 的 pane 判断一致
    enum Pane { EmptyPane = 0, ChatPage = 1, FriendInfoPage = 2, ApplyFriendPage = 3, UserInfoPage = 4 };
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

    FriendListModel *friends() const { return _friends; }
    ApplyListModel *applies() const { return _applies; }
    MessageModel *messages() const { return _messages; }

    int mode() const { return _mode; }
    int pane() const { return _pane; }
    QVariantMap profile() const { return _profile; }

    // 当前会话
    int currentUid() const { return _currentUid; }
    QString currentName() const { return _currentName; }

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

    // 联系人页的加好友浮层；打开时搜索框保持焦点
    bool addFriendOpen() const { return _addFriendOpen; }
    Q_INVOKABLE void openAddFriend();
    Q_INVOKABLE void closeAddFriend();

    // 聊天页的好友过滤：只匹配名字与昵称，不触发服务端搜索
    QString friendQuery() const { return _friendQuery; }
    void setFriendQuery(const QString &query);
    QVariantList friendMatches() const { return _friendMatches; }
    Q_INVOKABLE void clearFriendSearch();

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

    //侧栏导航
    Q_INVOKABLE void showChatMode();
    Q_INVOKABLE void showContactMode();
    Q_INVOKABLE void showSettings();

    //好友页面入口
    Q_INVOKABLE void openChat(int uid);
    Q_INVOKABLE void openProfile(int uid);
    Q_INVOKABLE void openApplyList();

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
    //当前会话可查看的图片与已下载视频，按消息顺序排列，供查看器左右切换
    Q_INVOKABLE QVariantList mediaItems(); //当前会话可查看的图片与已下载视频
    Q_INVOKABLE void search();
    Q_INVOKABLE void clearSearch();
    // 发出申请；name/nick/icon 用于在「新的朋友」里立刻显示这条记录
    Q_INVOKABLE void applyTo(int uid, const QString &name, const QString &nick,
                             const QString &icon, const QString &verify, const QString &remark);
    Q_INVOKABLE void authApply(int uid, const QString &remark);

signals:
    void modeChanged();
    void paneChanged();
    void profileChanged();
    void myProfileChanged();
    void currentChanged();
    void searchTextChanged();
    void searchStateChanged();
    void searchResultChanged();
    void addFriendOpenChanged();
    void friendQueryChanged();
    void friendMatchesChanged();
    void applyStateChanged();
    void avatarSyncChanged();
    //另存为的最终结果：异步下载完成或失败后回到界面提示
    void resourceSaved(bool ok, const QString &path);
    void resourceNotice(const QString &message); //本地文件缺失、另存位置失效等提示

private:
    void setMode(int mode);
    void setPane(int pane);
    void rebuildFriendMatches();
    void setApplyState(int state, const QString &message = {});
    void processPendingSaves(); //消息下载完成后处理排队的另存为
    static bool copyToPath(const QString &source, const QString &destination);
    QString savedPath(qint64 messageId); //已另存的位置，文件不存在时清掉记录并返回空

    FriendListModel *_friends = nullptr;
    ApplyListModel *_applies = nullptr;
    MessageModel *_messages = nullptr;

    int _mode = ChatMode;
    int _pane = EmptyPane;
    QVariantMap _profile; // 资料页展示的好友信息

    int _currentUid = 0;
    QString _currentName;
    QString _searchText;
    int _searchState = SearchIdle;
    QVariantMap _searchResult;
    QString _searchMessage;
    QString _pendingQuery; // 本次在途搜索的关键词，用于丢弃改词后回来的旧结果
    bool _addFriendOpen = false;
    QString _friendQuery;
    QVariantList _friendMatches;

    // 最近一次申请的目标与文案，失败重试时原样再发
    int _applyUid = 0;
    QString _applyName;
    QString _applyNick;
    QString _applyIcon;
    QString _applyVerify;
    QString _applyRemark;
    int _applyState = ApplyIdle;
    QString _applyMessage;

    //资源还没下载完时的另存为目标：下载完成后自动复制，失败则丢弃
    QHash<qint64, QString> _pending_saves;
    //已经另存到用户目录的位置：打开/定位优先用它，而不是账号缓存
    QHash<qint64, QString> _saved_paths;
};

#endif // CHATBRIDGE_H
