#ifndef USERMGR_H
#define USERMGR_H

#include <QObject>
#include <QByteArray>
#include <QList>
#include <QSize>
#include <memory>
#include <singleton.h>
#include "userdata.h"
#include <vector>
#include <QMap>
#include <mutex>
/******************************************************************************
 *
 * @file       usermgr.h
 * @brief      用户管理类，负责保存用户信息
 *
 * @author     西故
 * @date       2026/05/03
 * @history
 *****************************************************************************/

class UserMgr : public QObject,public Singleton<UserMgr>,
    public std::enable_shared_from_this<UserMgr>
{
    Q_OBJECT //使得UserMgr可以使用Qt的信号和槽机制

public:
    friend class Singleton<UserMgr>; //使基类创建实例时可以访问私有构造函数
    ~UserMgr(); //使基类析构时可以访问派生类的析构函数

    void SetUserInfo(std::shared_ptr<UserInfo> user_info); //设置用户信息
    std::shared_ptr<UserInfo> GetUserInfo();
    //本人资料修改成功: 更新昵称/性别/签名并通知界面
    void UpdateMyProfile(const QString &nick, int sex, const QString &desc);
    //好友备注与标签修改成功: 只改自己这一侧，界面刷新对应行
    bool UpdateFriendRelation(int uid, const QString &remark, const QStringList &tags);
    //好友文字资料刷新成功: 昵称、签名与性别随时会变；sex<0 表示这次没有性别信息
    bool UpdateFriendProfile(int uid, const QString &nick, const QString &desc, int sex = -1);
    //所有好友标签的并集，按名称排序，供联系人页筛选条使用
    QStringList AllTags() const;
    int GetUid(); //获取用户ID
    void SetToken(const QString &token); //资源服务器用同一个会话令牌校验身份
    QString GetToken() const;
    static QString LocalAvatarPath(); //当前账号的头像文件
    static QString ResourceCachePath(int ownerUid, const QString &name); //好友头像缓存
    //本机资源统一按登录账号分层，换账号不会读到别人的图片和头像缓存
    static QString AccountRootPath(); //AppData/users/<uid>
    //图片、视频、文件按类型分目录，资源名都是服务器下发的 <md5>.<ext>
    static QString ResourcePath(ChatMsgType type, const QString &name);
    static QString ResourceThumbPath(ChatMsgType type, const QString &name);
    static QString ChatImagePath(const QString &name); //本账号的聊天压缩图
    static QString ChatImageThumbPath(const QString &name); //本账号的气泡缩略图
    static QString PendingImageDir(); //输入区待上传的临时压缩图目录
    static void CleanPendingImages(); //登录后清掉本账号上次退出时的临时图
    static bool IsCompleteAvatarFile(const QString &path); //文件存在且是完整PNG，半截文件返回false
    QString GetIcon() const;
    bool SetLocalAvatar(const QByteArray &png); //发布已落盘的头像，不修改服务器头像字段

    //上传/下载状态由网络线程读写，和好友数据共用同一把递归锁
    void AddUploadFile(std::shared_ptr<UploadFileInfo> info);
    std::shared_ptr<UploadFileInfo> GetUploadFile(const QString &md5);
    void RemoveUploadFile(const QString &md5);
    //资源断线时取走全部聊天资源上传记录，头像记录仍由 FileService 自己收尾
    QList<std::shared_ptr<UploadFileInfo>> TakeResourceUploads();
    void SetUploadPaused(bool paused);
    bool UploadPaused() const;
    void AddDownloadFile(std::shared_ptr<DownloadFileInfo> info);
    std::shared_ptr<DownloadFileInfo> GetDownloadFile(const QString &name);
    void RemoveDownloadFile(const QString &name);
    //头像下载完成后同时替换好友列表与好友申请里的同一 uid，避免两个列表显示不同头像
    void SetContactIcon(int uid, const QString &icon);

    void AppendApplyList(QJsonArray array);
    //登录下发的会话列表：单聊补 thread_id，群聊写入群会话；两者共用同一份游标
    void AppendFriendList(QJsonArray array);
    bool AlreadySentApply(int uid); //是否已经向这个人发过还未通过的申请
    void AddApplyList(std::shared_ptr<ApplyInfo> app); //别人申请我，追加到申请列表
    void AddSentApply(int uid, const QString &name, const QString &nick,
                      const QString &icon, const QString &desc);
    void SetApplyStatus(int uid, int status, int direction = 0); //0 待处理 / 1 已通过
    bool CheckFriendById(int uid); //检查指定用户ID是否是好友
    void AddFriend(std::shared_ptr<AuthRsp> auth_rsp);
    void AddFriend(std::shared_ptr<AuthInfo> auth_info);

    // ---- 会话与消息：单聊、群聊统一按 thread_id 访问 ----
    //按会话追加消息：单聊写好友对象里的容器，群聊写群对象；随后发出对应的变化通知。
    //conv 的 thread_id 允许为 0（客户端还没建单聊会话），此时按 peer_uid 回落到好友容器
    void AppendChatMsgs(const ConversationRef &conv, std::vector<std::shared_ptr<TextChatData>> msgs);
    void SetChatThreads(const QJsonArray &threads); //登录时重置并写入全部会话映射
    void AppendChatThreads(const QJsonArray &threads); //追加会话映射, 不动已有消息
    void RefreshFriendList(); //仅通知列表重建, 数据已由调用方改好
    void SetThreadId(int friend_id, qint64 thread_id); //补会话ID并清除 pending
    qint64 ThreadId(int friend_id) const; //好友对应的会话ID, 0 表示还没有
    int FriendIdByThread(qint64 thread_id) const; //反查单聊会话属于哪个好友, 群聊返回0
    //按 thread 取消息容器；单聊与群聊都走这里，找不到返回空
    std::shared_ptr<ChatThreadData> ThreadData(qint64 thread_id) const;
    ChatThreadKind ThreadKind(qint64 thread_id) const;
    int ThreadPeer(qint64 thread_id) const; //单聊对方 uid，群聊 0
    QString ThreadTitle(qint64 thread_id) const; //单聊显示名或群名称
    int ThreadMemberCount(qint64 thread_id) const; //群成员数，单聊返回0
    qint64 LastMessageId(const ConversationRef &conv) const; //当前最大消息ID, 作为翻页游标
    bool HasPendingPrivateChat(int friend_id) const; //是否正在等待 1026 回包
    void MarkPrivateChatPending(int friend_id, bool pending);
    //对端已读：把自己发的、消息号不超过 last_read_id 的消息标成已读
    bool MarkMessagesReadTo(int peer_uid, qint64 last_read_id);
    //撤回：把指定消息标记为已撤回并清空正文与资源，单聊与群聊都只看到提示
    bool MarkMessageRecalled(qint64 message_id);
    //发送超时或回执失败：把某条还在等待的消息标失败，界面给出重发入口
    bool MarkMessageFailed(const ConversationRef &conv, const QString &msg_id);
    //从本地会话里删掉一条消息（重发前先移除旧的失败记录，避免出现两条）
    bool RemoveChatMessage(const ConversationRef &conv, const QString &msg_id);
    //未读：只按会话计数，打开会话时清零；群聊另外记录"有人@我"
    void IncThreadUnread(qint64 thread_id);
    void ClearThreadUnread(qint64 thread_id);
    int ThreadUnread(qint64 thread_id) const;
    bool ThreadAtMe(qint64 thread_id) const;
    //未读里出现 @我：会话行显示"[有人@我]"，打开会话时随未读一起清掉
    void MarkThreadAtMe(qint64 thread_id);
    //收到未知群的消息时先建占位，避免系统消息与首条聊天没有容器可放
    void EnsureGroup(qint64 thread_id);
    int UnreadTotal() const;
    //导航角标：跳过免打扰的群，其余的未读照常累加
    int UnreadBadgeTotal() const;
    //当前正在看的会话；设置后该会话的未读立即清零，收到新消息也不再计数
    void SetActiveThread(qint64 thread_id);
    qint64 ActiveThread() const { return _active_thread; }
    void UpdateChatMessage(const ConversationRef &conv, const QString &msg_id, qint64 message_id,
                           ChatMessageStatus status); //回执补 ID/状态并重新排序
    //上传进度按资源名回写：同一资源的多条消息一起更新，完成/失败同时改消息状态
    void UpdateResourceUploadProgress(const QString &resource_name, qint64 trans, qint64 total);
    void FinishResourceUpload(const QString &resource_name);
    void FailResourceUpload(const QString &resource_name);
    std::shared_ptr<UploadFileInfo> GetUploadFileByName(const QString &name) const;
    //接收方收到图片/视频/文件通知：按 msgid 合并或追加，避免与历史同步重复
    void UpsertResourceMessage(const ConversationRef &conv, std::shared_ptr<TextChatData> message);
    //下载进度与结果：按资源名更新所有引用该资源的消息
    void UpdateResourceDownloadProgress(const QString &resource_name, qint64 current, qint64 total);
    void FinishResourceDownload(const QString &resource_name, const QString &local_path,
                                const QString &display_path, const QSize &pixel_size);
    void FailResourceDownload(const QString &resource_name);
    //视频首帧单独下载：只替换预览地址，不改变视频本身的下载状态
    void SetVideoThumb(const QString &thumb_name, const QString &local_path,
                       const QSize &pixel_size);
    void SetResourceState(qint64 message_id, ResourceTransferState state, int progress);
    //消息还没拿到服务端 ID 时按客户端 msgid 更新，视频首帧就绪后用它刷新预览
    void SetResourceStateByUniqueId(const ConversationRef &conv, const QString &msg_id,
                                    ResourceTransferState state, int progress);
    void SetLocalVideoThumb(const ConversationRef &conv, const QString &msg_id,
                            const QString &local_path, const QSize &pixel_size);
    //按消息号找消息；输出所属会话，单聊与群聊共用
    std::shared_ptr<TextChatData> FindMessage(qint64 message_id, qint64 *thread_id = nullptr) const;
    //服务端还没分配 ID 时按客户端 msgid 找消息，视频/文件在 1035 前失败时用它重试
    std::shared_ptr<TextChatData> FindMessageByUniqueId(const QString &msg_id,
        qint64 *thread_id = nullptr) const;
    //本地文件被用户移动或删除：清掉路径并把状态退回未下载，允许重新下载
    void MarkResourceMissing(qint64 message_id);
    void FailPendingMessages(); //断线时把发送中的消息统一标失败

    //供列表模型按行读取：模型不复制数据，只引用这里的原始对象
    int FriendCount() const;
    std::shared_ptr<FriendInfo> FriendAt(int index) const;
    //会话对象：好友 + 已删好友但仍有历史的人，联系人列表按它遍历
    int PeerCount() const;
    std::shared_ptr<FriendInfo> PeerAt(int index) const;
    //删好友成功：从联系人列表移除，保留对象与消息，会话列表仍能看到历史
    void RemoveFriend(int uid);
    //删好友失败：把乐观移除的那一行放回去，列表与服务器保持一致
    void RestoreFriend(int uid);
    std::shared_ptr<FriendInfo> GetFriendById(int uid);
    int ApplyCount() const;
    std::shared_ptr<ApplyInfo> ApplyAt(int index) const;

    // ---- 群聊 ----
    //登录或 1028 下发的一条群会话：新建或更新资料，不动已有消息
    void UpsertGroupThread(const QJsonObject &item);
    //1026/1063 的群资料：名称、公告、我的角色与设置
    void ApplyGroupProfile(const QJsonObject &group);
    //群成员整体替换：资料页与气泡显示名都读这一份
    void SetGroupMembers(qint64 thread_id, const QJsonArray &members);
    void AddGroupMember(qint64 thread_id, const GroupMember &member);
    //成员数变化：服务端下发的人数优先，缺省时按成员表长度
    void SetGroupMemberCount(qint64 thread_id, int count);
    //被移出或解散：从会话列表移除，消息一并丢弃（服务端已经级联删除）
    void RemoveGroup(qint64 thread_id);
    std::shared_ptr<GroupInfo> GetGroup(qint64 thread_id) const;
    int GroupCount() const;
    std::shared_ptr<GroupInfo> GroupAt(int index) const;
    //群消息里的发送者显示名：群昵称 > 好友备注 > 昵称 > 用户名
    QString GroupSenderName(qint64 thread_id, int uid) const;
    QString GroupSenderIcon(qint64 thread_id, int uid) const;
    void SetGroupMuted(qint64 thread_id, bool muted);

    //新会话开始前清空上一个账号的数据，避免重新登录后好友与申请重复
    void ClearSession();

signals:
    void SessionCleared();            //换账号前清空会话，业务层据此丢弃在途资源任务
    void UserIconChanged();
    void UserProfileChanged();        //本人昵称/性别/签名变化，资料页与侧栏据此刷新
    void FriendRelationChanged(int uid); //备注或标签变化，列表只刷新对应行
    void FriendProfileChanged(int uid);  //好友昵称/签名变化，来自 1050 资料刷新
    void FriendTagsChanged();         //标签集合变化，联系人页筛选条重算
    void FriendListAboutToChange();
    void ApplyListAboutToChange();
    void FriendListChanged();        // 登录拿到好友列表、通过好友申请
    void ApplyListChanged();         // 好友申请列表变化
    //某个会话的消息变化：thread_id 是主键；单聊还没建会话时 thread_id 为 0，
    //第二个参数带上对方 uid，消息模型据此仍能匹配当前会话
    void ConversationChanged(qint64 thread_id, int peer_uid);
    void UnreadChanged(); // 未读数变化：会话行与左侧栏徽标一起刷新
    void FriendIconChanged(int uid); // 好友头像下载完成，只刷新对应行
    void ApplyIconChanged(int uid);  // 好友申请里同一个人头像变化，只刷新申请行
    //群资料、成员或我在群里的设置变化；消息变化仍走 ConversationChanged
    void GroupInfoChanged(qint64 thread_id);
    void GroupListChanged(); //群会话增删或排序依据变化，会话模型整表重建

private:
    UserMgr();

    //按会话找消息容器：群聊查群索引，单聊查好友索引；找不到返回空
    std::shared_ptr<ChatThreadData> findThreadData(qint64 thread_id) const;
    //消息容器与对应的会话引用，找不到时 out 为空
    bool resolveConversation(const ConversationRef &conv, std::shared_ptr<ChatThreadData> &data,
                             ChatThreadKind &kind) const;

    mutable std::recursive_mutex _mtx; //数据被主线程与网络线程共同访问
    std::shared_ptr<UserInfo> _user_info;
    QString _token;
    QString _localIcon; //本机头像 data URL, 为空时回落到服务端 icon
    std::vector<std::shared_ptr<ApplyInfo>> _apply_list; //好友申请列表，新的排最前
    long long _apply_seq = 0; //申请列表的自增序号，保证新条目一定比旧的大
    std::vector<std::shared_ptr<FriendInfo>> _friend_list; //列表模型按行读取的顺序容器
    QMap<int, std::shared_ptr<FriendInfo>> _friend_map; //uid 索引, 与列表指向同一对象
    std::vector<std::shared_ptr<GroupInfo>> _group_list; //群会话按加入顺序保存
    QMap<qint64, std::shared_ptr<GroupInfo>> _group_map; //thread_id 索引
    QMap<int, bool> _pending_private_chats; //正在等待建会话回包的好友, 防止重复请求
    QMap<QString, std::shared_ptr<UploadFileInfo>> _uploads; //按md5索引
    QMap<QString, std::shared_ptr<DownloadFileInfo>> _downloads; //按资源名索引
    QMap<qint64, int> _unread; //thread_id -> 未读条数，只在当前登录会话内有效
    QSet<qint64> _at_me_threads; //有未读 @我 的群会话
    qint64 _active_thread = 0; //正在看的会话，0 表示不在会话里
    bool _upload_paused = false;
};

#endif // USERMGR_H
