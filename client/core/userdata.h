#ifndef USERDATA_H
#define USERDATA_H

#include <QString>
#include <QStringList>
#include <memory>
#include <atomic>
#include <QJsonArray>
#include <vector>
#include <QJsonObject>
#include <QSet>
#include <utility>
#include <QMetaType>
#include "global.h"
/******************************************************************************
 *
 * @file       userdata.h
 * @brief      用户数据结构定义，包含搜索结果、好友申请、好友信息等数据结构
 *
 * @author     西故
 * @date       2026/05/09
 * @history
 *****************************************************************************/

//用户数据结构
class SearchInfo {
public:
    SearchInfo(int uid, QString name, QString nick, QString desc, int sex, QString icon);

    int _uid; //用户ID
    QString _name; //用户名
    QString _nick; //用户昵称
    QString _desc; //用户描述
    int _sex; //用户性别
    QString _icon; //用户头像URL
};

//加好友的申请数据结构
class AddFriendApply {
public:
    AddFriendApply(int from_uid, QString name, QString desc, QString icon, QString nick, int sex);
    int _from_uid;
    QString _name;
    QString _desc;
    QString _icon;
    QString _nick;
    int     _sex;
};

//好友申请信息数据结构
//_direction 区分方向：0 别人申请我，1 我申请别人；两边都显示在「新的朋友」里
struct ApplyInfo {
    ApplyInfo(int uid, QString name, QString desc, QString icon, QString nick, int sex, int status,
              int direction = 0) :
        _uid(uid),_name(name),_desc(desc), _icon(icon),_nick(nick),_sex(sex),_status(status),
        _direction(direction){}

    ApplyInfo(std::shared_ptr<AddFriendApply> addinfo) : _uid(addinfo->_from_uid),
        _name(addinfo->_name), _desc(addinfo->_desc), _icon(addinfo->_icon),
        _nick(addinfo->_nick),_sex(addinfo->_sex), _status(0), _direction(0) {}

    void SetIcon(QString head){ _icon = head; }

    int _uid;
    QString _name;
    QString _desc;
    QString _icon;
    QString _nick;
    int _sex;
    int _status;
    int _direction = 0;
};

struct TextChatData;

//用户认证信息数据结构
struct AuthInfo {
    AuthInfo(int uid, QString name, QString nick, QString icon, int sex, qint64 thread_id = 0,
             qint64 message_id = 0, QString welcome_msg_id = {}) :
        _uid(uid), _name(name), _nick(nick), _icon(icon), _sex(sex), _thread_id(thread_id),
        _message_id(message_id), _welcome_msg_id(std::move(welcome_msg_id)){}

    int _uid;
    QString _name;
    QString _nick;
    QString _icon;
    int _sex;
    qint64 _thread_id = 0; //两人共用的单聊会话ID, 0 表示尚未建立
    qint64 _message_id = 0; //欢迎消息中的最后一条, 便于定位
    QString _welcome_msg_id; //欢迎消息的客户端ID前缀
    QString _back; //我当初给对方设的备注，通过申请后由服务端回传
    QStringList _tags; //我当初给对方选的分组，通过申请后一并生效
    std::vector<std::shared_ptr<TextChatData>> _welcome_msgs; //通过申请时写入的两条欢迎消息
};

//用户认证响应数据结构
struct AuthRsp {
    AuthRsp(int peer_uid, QString peer_name, QString peer_nick, QString peer_icon,
            int peer_sex, qint64 thread_id = 0, qint64 message_id = 0,
            QString welcome_msg_id = {}) :
        _uid(peer_uid),_name(peer_name),_nick(peer_nick), _icon(peer_icon),_sex(peer_sex),
        _thread_id(thread_id),_message_id(message_id),
        _welcome_msg_id(std::move(welcome_msg_id)){}

    int _uid;
    QString _name;
    QString _nick;
    QString _icon;
    int _sex;
    qint64 _thread_id = 0;
    qint64 _message_id = 0;
    QString _welcome_msg_id;
    QString _back; //同意时自己给对方设的备注
    QStringList _tags; //同意时自己给对方设的分组，当前弹层不选，保持为空
    std::vector<std::shared_ptr<TextChatData>> _welcome_msgs;
};

//消息状态：0 发送中、1 已发送、2 失败、3 未读、4 已读。
//3/4 目前由历史同步按视角给出，等本地库接入后再补已读回执
//消息可撤回时间：与 ChatServer 的 RECALL_WINDOW_SECONDS 保持一致，
//界面据此决定右键菜单里是否出现「撤回」
constexpr int kRecallWindowSeconds = 120;

//消息状态：只影响自己发的消息，对方消息不显示状态图标
enum class ChatMessageStatus : int {
    Sending = 0,
    Sent = 1,
    Failed = 2,
    Unread = 3,
    Read = 4,
};

//消息类型：文本走气泡，图片/文件直接显示资源
enum class ChatMsgType : int {
    Text = 0,
    Image = 1,
    Video = 2,
    File = 3,
};

//图片/视频/文件在界面上的传输状态：图片只用到进度，视频与文件还要按状态显示按钮
enum class ResourceTransferState : int {
    None = 0, //接收方还没开始下载
    Preparing = 1, //视频在准备首帧、消息等待落库
    Uploading = 2,
    UploadPaused = 3,
    Downloading = 4,
    DownloadPaused = 5,
    Done = 6,
    Failed = 7,
};

//一个单聊会话：thread_id 来自服务端，_chat_msgs 始终按 message_id 排序
struct ChatThreadData {
    explicit ChatThreadData(qint64 thread_id = 0) : _thread_id(thread_id) {}

    qint64 _thread_id = 0;
    std::vector<std::shared_ptr<TextChatData>> _chat_msgs;
};

struct FriendInfo {
    FriendInfo(int uid, QString name, QString nick, QString icon,
               int sex, QString desc, QString back, QString last_msg=""):_uid(uid),
        _name(name),_nick(nick),_icon(icon),_sex(sex),_desc(desc),
        _back(back),_last_msg(last_msg),_chat_thread(std::make_shared<ChatThreadData>()){}

    FriendInfo(std::shared_ptr<AuthInfo> auth_info):_uid(auth_info->_uid),
        _nick(auth_info->_nick),_icon(auth_info->_icon),_name(auth_info->_name),
        _sex(auth_info->_sex),_back(auth_info->_back),_tags(auth_info->_tags),
        _chat_thread(std::make_shared<ChatThreadData>(auth_info->_thread_id))
    {
        _chat_thread->_chat_msgs = auth_info->_welcome_msgs;
    }

    FriendInfo(std::shared_ptr<AuthRsp> auth_rsp):_uid(auth_rsp->_uid),
        _nick(auth_rsp->_nick),_icon(auth_rsp->_icon),_name(auth_rsp->_name),
        _sex(auth_rsp->_sex),_back(auth_rsp->_back),_tags(auth_rsp->_tags),
        _chat_thread(std::make_shared<ChatThreadData>(auth_rsp->_thread_id))
    {
        _chat_thread->_chat_msgs = auth_rsp->_welcome_msgs;
    }

    void AppendChatMsgs(const std::vector<std::shared_ptr<TextChatData>> text_vec);

    int _uid;
    QString _name;
    QString _nick;
    QString _icon;
    int _sex;
    QString _desc;
    QString _back;
    QStringList _tags; //自己给这个好友打的分组标签，只在本地视角使用
    QString _last_msg;
    //会话最后活动时间（UNIX 秒），登录首屏排序用；历史拉回后以最后一条消息时间为准
    qint64 _last_time = 0;
    //服务端下发的未读条数，登录时恢复；之后由客户端本地增减
    int _unread = 0;
    //是否仍在好友关系里：删好友后置 false，对象与历史保留，会话列表仍能看到
    bool _is_friend = true;
    std::shared_ptr<ChatThreadData> _chat_thread;
};

//好友信息数据结构
struct UserInfo {
    UserInfo(int uid, QString name, QString nick, QString icon, int sex, QString last_msg = "") :
        _uid(uid), _name(name), _nick(nick), _icon(icon), _sex(sex), _last_msg(last_msg){}

    UserInfo(std::shared_ptr<AuthInfo> auth):
        _uid(auth->_uid), _name(auth->_name) ,_nick(auth->_nick),
        _icon(auth->_icon), _sex(auth->_sex), _last_msg(""){}

    UserInfo(int uid, QString name, QString icon):
        _uid(uid), _name(name), _nick(_name), _icon(icon), _sex(0), _last_msg(""){}

    UserInfo(std::shared_ptr<AuthRsp> auth):
        _uid(auth->_uid), _name(auth->_name), _nick(auth->_nick),
        _icon(auth->_icon), _sex(auth->_sex), _last_msg(""){}

    UserInfo(std::shared_ptr<SearchInfo> search_info):
        _uid(search_info->_uid),_name(search_info->_name),_nick(search_info->_nick),
        _icon(search_info->_icon),_sex(search_info->_sex),_last_msg(""){
    }

    UserInfo(std::shared_ptr<FriendInfo> friend_info):
        _uid(friend_info->_uid),_name(friend_info->_name),_nick(friend_info->_nick),
        _icon(friend_info->_icon),_sex(friend_info->_sex),_last_msg("")
    {
        _chat_msgs = friend_info->_chat_thread->_chat_msgs;
    }

    int _uid;
    QString _name;
    QString _nick;
    QString _icon;
    int _sex;
    QString _desc; //个性签名，登录时由服务端下发，本人修改后同步更新
    QString _last_msg;
    std::vector<std::shared_ptr<TextChatData>> _chat_msgs;
};

struct TextChatData{
    TextChatData(QString msg_id, QString msg_content, int fromuid, int touid,
                 qint64 message_id = 0, qint64 thread_id = 0,
                 ChatMessageStatus status = ChatMessageStatus::Sending)
        :_msg_id(msg_id),_msg_content(msg_content),_from_uid(fromuid),_to_uid(touid),
         _message_id(message_id),_thread_id(thread_id),_status(status){
    }
    QString _msg_id;
    qint64 _message_id = 0;
    qint64 _thread_id = 0;
    ChatMessageStatus _status = ChatMessageStatus::Sending;
    QString _msg_content;
    QString _imageSource; //图片消息的本地显示地址（data/file URL），始终指向缩略图
    int _imageWidth = 0;
    int _imageHeight = 0;
    int _transferProgress = -1; //图片/视频/文件的传输进度 0..100，-1 表示不显示进度条
    ChatMsgType _msg_type = ChatMsgType::Text; //消息类型，历史同步与发送时确定
    QString _resource_name; //图片/视频/文件在资源服务器上的名字，文本为空
    QString _resource_md5; //资源内容的 md5，上传重试时直接复用
    QString _file_name; //文件/视频的原始显示名
    qint64 _file_size = 0; //文件/视频的字节数
    QString _thumb_name; //视频首帧资源名，接收方先下载它做预览
    QString _local_path; //完整资源在本机的位置，点开查看器/另存为时使用
    ResourceTransferState _transfer_state = ResourceTransferState::None;
    int _from_uid;
    int _to_uid;
    //服务端落库时间（UNIX 秒）。0 表示老数据或本地占位，界面按"刚刚"兜底
    qint64 _time = 0;
    //已撤回：正文已清空，界面只显示一行提示，不再显示气泡
    bool _recalled = false;
    //自己撤回时本地保留原文，供「重新编辑」填回输入框；服务端那份已经清空
    QString _recalled_text;
};

struct TextChatMsg{
    TextChatMsg(int fromuid, int touid, QJsonArray arrays, qint64 thread_id = 0):
        _from_uid(fromuid),_to_uid(touid),_thread_id(thread_id){
        for(auto  msg_data : arrays){
            auto msg_obj = msg_data.toObject();
            auto content = msg_obj["content"].toString();
            auto msgid = msg_obj["msgid"].toString();
            auto msg_ptr = std::make_shared<TextChatData>(msgid, content,fromuid, touid,
                msg_obj["message_id"].toVariant().toLongLong(),
                msg_obj["thread_id"].toVariant().toLongLong() > 0
                    ? msg_obj["thread_id"].toVariant().toLongLong() : thread_id,
                static_cast<ChatMessageStatus>(msg_obj["status"].toInt(
                    static_cast<int>(ChatMessageStatus::Sent))));
            //实时消息也带服务端落库时间，时间条与列表排序都靠它
            msg_ptr->_time = msg_obj["time"].toVariant().toLongLong();
            _chat_msgs.push_back(msg_ptr);
        }
    }
    int _to_uid;
    int _from_uid;
    qint64 _thread_id = 0;
    std::vector<std::shared_ptr<TextChatData>> _chat_msgs;
};

//一次在途的头像/文件上传：窗口里的分片已发出，等待服务器逐片确认
struct UploadFileInfo {
    QString _md5;
    QString _name;
    QString _path;
    qint64 _total_size = 0;
    qint64 _trans_size = 0; //连续确认到的字节数，界面进度用它
    int _seq = 0; //已经发出的最大分片序号
    int _max_seq = 0; //总分片数
    int _confirmed_seq = 0; //连续确认到的分片序号
    QSet<int> _flighting; //已发送、未确认
    QSet<int> _acked; //已确认、但还没连成连续段
    int _uid = 0;
    ReqId _req_id = ID_UPLOAD_HEAD_ICON_REQ;
    std::atomic<bool> _paused{false}; //视频/文件可以单独暂停，网络线程按它决定是否继续发片
    //聊天图片/视频/文件上传完成后回写消息状态用；头像与普通文件为0
    qint64 _chat_msg_id = 0;
    int _touid = 0;
    qint64 _thread_id = 0;
    QString _unique_id;
};

//一次断点下载：seq 从 1 开始，写完后由 FileTcpMgr 继续请求下一片
struct DownloadFileInfo {
    QString _name;
    QString _client_path;
    int _owner_uid = 0;
    int _seq = 1;
    qint64 _total_size = 0;
    qint64 _current_size = 0;
    std::atomic<bool> _paused{false}; //暂停后不再请求下一片，继续时从当前偏移续传
};

//界面统一显示名：备注 > 昵称 > 用户名
//备注只在自己这一侧生效；昵称为空时回落到用户名，避免用户清空昵称后列表出现空白
inline QString DisplayName(const QString &remark, const QString &nick, const QString &name)
{
    if (!remark.isEmpty())
        return remark;
    return nick.isEmpty() ? name : nick;
}

Q_DECLARE_METATYPE(std::shared_ptr<SearchInfo>)
Q_DECLARE_METATYPE(std::shared_ptr<AddFriendApply>)
Q_DECLARE_METATYPE(std::shared_ptr<AuthInfo>)
Q_DECLARE_METATYPE(std::shared_ptr<AuthRsp>)
Q_DECLARE_METATYPE(std::shared_ptr<TextChatMsg>)

#endif // USERDATA_H
