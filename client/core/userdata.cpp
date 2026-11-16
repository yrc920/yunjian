#include "userdata.h"
#include <algorithm>

SearchInfo::SearchInfo(int uid, QString name, QString nick, QString desc, int sex, QString icon,
                       int relation) :
    _uid(uid), _relation(relation), _name(name), _nick(nick), _desc(desc), _sex(sex), _icon(icon) {
}

AddFriendApply::AddFriendApply(int from_uid, QString name, QString desc, QString icon, QString nick, int sex)
    : _from_uid(from_uid), _name(name), _desc(desc), _icon(icon), _nick(nick), _sex(sex)
{

}

//消息身份：客户端消息号优先，缺省时用服务端消息号。
//发出去还没回执的消息只有客户端号，回执/同步回来的那一份带服务端号，
//两边必须能对上，否则重登后同一条消息会出现两次（一条失败、一条已发送）。
//服务端的群系统消息没有客户端号，这里给它一个稳定的合成键参与去重
QString MessageKey(const std::shared_ptr<TextChatData> &message)
{
    if (!message)
        return {};
    if (!message->_msg_id.isEmpty() && !message->_msg_id.startsWith(QLatin1String("srv:")))
        return QStringLiteral("c:") + message->_msg_id;
    if (message->_message_id > 0)
        return QStringLiteral("s:") + QString::number(message->_message_id);
    return message->_msg_id;
}

//按 msgid 去重后追加：同一条消息可能先以「发送中」进入列表，
//回执或历史同步再带服务端 message_id 回来，这里只做原地更新
void ChatThreadData::AppendMsgs(const std::vector<std::shared_ptr<TextChatData>>& msgs)
{
    for (const auto &text : msgs) {
        if (!text)
            continue;
        const QString key = MessageKey(text);
        auto existing = std::find_if(_chat_msgs.begin(), _chat_msgs.end(), [&](const auto &item) {
            return MessageKey(item) == key;
        });
        if (existing == _chat_msgs.end()) {
            _chat_msgs.push_back(text);
        } else if ((*existing)->_message_id == 0 || text->_message_id > 0) {
            (*existing)->_message_id = text->_message_id;
            (*existing)->_thread_id = text->_thread_id;
            (*existing)->_status = text->_status;
            (*existing)->_msg_content = text->_msg_content;
            //回执带服务端落库时间，覆盖发送时用的本地时间；为 0 时保留原值
            if (text->_time > 0)
                (*existing)->_time = text->_time;
        }
    }
    //有服务端 ID 的消息按 ID 升序；尚未确认的排在最后，拿到回执后自然归位
    std::stable_sort(_chat_msgs.begin(), _chat_msgs.end(), [](const auto &left, const auto &right) {
        if (left->_message_id == 0 || right->_message_id == 0)
            return left->_message_id != 0 && right->_message_id == 0;
        return left->_message_id < right->_message_id;
    });
}
