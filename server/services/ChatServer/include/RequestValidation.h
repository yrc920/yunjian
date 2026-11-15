#pragma once

#include "const.h"
#include <json/json.h>
#include <string>

//在业务分发前统一检查请求字段和会话身份，只有校验通过才交给对应的handler处理
//这里校验登录请求的字段格式，token是否有效仍由LoginHandler查询Redis后判断
//uid/fromuid在不同消息中含义不同：搜索uid和touid是目标用户，不能用来判断操作者身份
inline ErrorCodes ValidateChatRequest(short id, const Json::Value& root, int session_uid)
{
    if (!root.isObject()) return Error_Json; //请求体必须是JSON对象
    //先检查是否能表示为int，再读取数值，避免数组、对象或超出int范围的数字触发转换异常
    auto positiveId = [&root](const char* key) {
        return root[key].isInt() && root[key].asInt() > 0;
    };
    auto stringField = [&root](const char* key) { return root[key].isString(); }; //检查字符串字段的类型
    //标签数组可以缺省(旧客户端不传), 出现时必须是字符串数组; 空标签没有意义, 直接拒绝
    auto tagsField = [&root](const char* key) {
        if (!root.isMember(key)) return true;
        const auto& tags = root[key];
        if (!tags.isArray() || tags.size() > MAX_TAG_COUNT) return false;
        for (const auto& tag : tags) {
            if (!tag.isString()) return false;
            const auto text = tag.asString();
            if (text.empty() || Utf8CharCount(text) > MAX_TAG_LEN) return false;
        }
        return true;
    };
    //一条连接只绑定一个用户，已经登录的连接不能再次登录，否则原用户映射可能无法正确清理
    if (id == MSG_CHAT_LOGIN) {
        if (session_uid != 0) return TokenInvalid;
        return positiveId("uid") && stringField("token") && !root["token"].asString().empty()
            ? Success : Error_Json;
    }
    if (session_uid <= 0) return TokenInvalid; //除登录外，其余请求都需要已认证的会话
    //搜索内容既可以是数字uid，也可以是用户名，按字符串接收；被搜索的人不必是当前登录用户
    if (id == ID_SEARCH_USER_REQ) {
        return stringField("uid") && !root["uid"].asString().empty() ? Success : Error_Json;
    }
    if (id == ID_LOAD_CHAT_THREAD_REQ || id == ID_LOAD_CHAT_MSG_REQ) {
        if (!positiveId("fromuid") || root["fromuid"].asInt() != session_uid
            || !root["thread_id"].isInt64() || root["thread_id"].asInt64() < 0)
            return Error_Json;
        if (id == ID_LOAD_CHAT_MSG_REQ
            && (!root["message_id"].isInt64() || root["message_id"].asInt64() < 0))
            return Error_Json;
        return Success;
    }
    if (id == ID_CREATE_PRIVATE_CHAT_REQ) {
        return positiveId("fromuid") && root["fromuid"].asInt() == session_uid
            && positiveId("touid") && root["touid"].asInt() != session_uid
            ? Success : UidInvalid;
    }
    //图片消息先落库再上传资源, 字段约束与资源上传协议保持一致
    if (id == ID_IMG_CHAT_MSG_REQ) {
        if (!positiveId("fromuid") || root["fromuid"].asInt() != session_uid
            || !positiveId("touid") || root["touid"].asInt() == session_uid)
            return UidInvalid;
        if (root.isMember("thread_id")
            && (!root["thread_id"].isInt64() || root["thread_id"].asInt64() < 0))
            return Error_Json;
        if (!stringField("md5") || !stringField("name") || !stringField("token")
            || !stringField("unique_id"))
            return Error_Json;
        const auto md5 = root["md5"].asString();
        const auto name = root["name"].asString();
        const auto unique_id = root["unique_id"].asString();
        //md5 是后续续传键, name 是落盘文件名, 不能带路径分隔符
        if (md5.size() != 32 || name.empty() || name.size() > 128
            || name.find('/') != std::string::npos || name.find('\\') != std::string::npos
            || name.front() == '.' || unique_id.empty())
            return Error_Json;
        return Success;
    }
    //视频/文件消息：资源文件名、原始文件名、大小与可选的视频首帧资源一起落库
    if (id == ID_RESOURCE_CHAT_MSG_REQ) {
        if (!positiveId("fromuid") || root["fromuid"].asInt() != session_uid
            || !positiveId("touid") || root["touid"].asInt() == session_uid)
            return UidInvalid;
        if (!root["msg_type"].isInt()
            || (root["msg_type"].asInt() != 2 && root["msg_type"].asInt() != 3))
            return Error_Json;
        if (root.isMember("thread_id")
            && (!root["thread_id"].isInt64() || root["thread_id"].asInt64() < 0))
            return Error_Json;
        if (!stringField("md5") || !stringField("name") || !stringField("token")
            || !stringField("unique_id") || !stringField("file_name"))
            return Error_Json;
        const auto md5 = root["md5"].asString();
        const auto name = root["name"].asString();
        const auto unique_id = root["unique_id"].asString();
        const auto file_name = root["file_name"].asString();
        if (md5.size() != 32 || name.empty() || name.size() > 128
            || name.find('/') != std::string::npos || name.find('\\') != std::string::npos
            || name.front() == '.' || unique_id.empty()
            || file_name.empty() || file_name.size() > 255)
            return Error_Json;
        if (!root["file_size"].isInt64() || root["file_size"].asInt64() <= 0)
            return Error_Json;
        //视频首帧是可选的独立资源，字段存在时也必须是安全的文件名
        if (root.isMember("thumb")) {
            if (!root["thumb"].isString())
                return Error_Json;
            const auto thumb = root["thumb"].asString();
            if (!thumb.empty()
                && (thumb.size() > 128 || thumb.find('/') != std::string::npos
                    || thumb.find('\\') != std::string::npos || thumb.front() == '.'))
                return Error_Json;
        }
        return Success;
    }
    //修改本人资料只认会话绑定的身份，请求体里没有操作者字段；
    //昵称和签名允许为空（昵称为空时界面回落到用户名），性别只接受 0 未知 / 1 男 / 2 女
    if (id == ID_UPDATE_PROFILE_REQ) {
        if (!stringField("nick") || !stringField("desc") || !root["sex"].isInt())
            return Error_Json;
        const int sex = root["sex"].asInt();
        return (sex >= 0 && sex <= 2) ? Success : Error_Json;
    }
    //备注与标签只作用于自己这一侧的好友关系，可以直接清空
    if (id == ID_UPDATE_FRIEND_REMARK_REQ) {
        if (!positiveId("touid") || root["touid"].asInt() == session_uid)
            return UidInvalid;
        return stringField("remark") && tagsField("tags") ? Success : Error_Json;
    }
    //拉取好友最新资料: 只读操作, 目标不能是自己
    if (id == ID_GET_PROFILE_REQ) {
        return positiveId("touid") && root["touid"].asInt() != session_uid
            ? Success : UidInvalid;
    }
    //已读上报与撤回都不带操作者字段，身份只认会话；
    //归属校验（这个 thread 是不是他的、这条消息是不是他发的）留到 DAO 里做
    if (id == ID_MARK_READ_REQ) {
        return root["thread_id"].isInt64() && root["thread_id"].asInt64() > 0
            && root["message_id"].isInt64() && root["message_id"].asInt64() > 0
            ? Success : Error_Json;
    }
    if (id == ID_RECALL_MSG_REQ) {
        return root["message_id"].isInt64() && root["message_id"].asInt64() > 0
            ? Success : Error_Json;
    }
    //删好友：只带对方 uid，不能删自己
    if (id == ID_DELETE_FRIEND_REQ) {
        return positiveId("touid") && root["touid"].asInt() != session_uid
            ? Success : UidInvalid;
    }
    //好友申请用uid表示申请人，同意好友、文本消息和心跳用fromuid表示操作者
    //请求中声明的操作者必须与服务端绑定的用户一致，不能仅凭客户端传来的uid执行操作
    const char* actor = id == ID_ADD_FRIEND_REQ ? "uid" : "fromuid";
    if (!positiveId(actor)) return Error_Json;
    if (root[actor].asInt() != session_uid) return UidInvalid;
    if (id == ID_HEART_BEAT_REQ) return Success; //心跳没有目标用户，只需校验发送者
    //剩余请求都指向其他用户，这里只校验uid格式与非自身约束，不负责判断目标是否存在或是否为好友
    if (!positiveId("touid") || root["touid"].asInt() == session_uid) return UidInvalid;
    //申请说明和备注允许为空，但必须是字符串；标签可以不带，带了就要符合长度与数量限制
    if (id == ID_ADD_FRIEND_REQ) {
        return stringField("applyname") && stringField("bakname") && tagsField("tags")
            ? Success : Error_Json;
    }
    if (id == ID_AUTH_FRIEND_REQ) return stringField("back") && tagsField("tags")
        ? Success : Error_Json;
    //文本消息按数组传入，每一项都要有字符串类型的内容和消息标识，避免处理到一半才发现字段错误
    if (id == ID_TEXT_CHAT_MSG_REQ) {
        const auto& texts = root["text_array"];
        if (!texts.isArray() || texts.empty()) return Error_Json;
        for (const auto& text : texts) {
            if (!text.isObject() || !text["content"].isString() || !text["msgid"].isString()
                || text["msgid"].asString().empty())
                return Error_Json;
        }
        if (root.isMember("thread_id")
            && (!root["thread_id"].isInt64() || root["thread_id"].asInt64() < 0))
            return Error_Json;
        return Success;
    }
    return Error_Json; //不接受未定义的请求格式
}
