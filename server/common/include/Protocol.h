#pragma once
// 五个服务共用的协议基元：业务错误码与请求/响应配对规则
//
// 这一层只放"跨服务语义完全一致"的定义。各服务的消息 ID 与自有错误码
// 仍留在各自的 const.h：它们只在单条通道上出现，强行合并会把无关服务绑在一起。

#include <cstdint>

// 登录令牌有效期：每次成功登录（含自动登录）按当前时间续期，
// 连续一周没登录才失效，避免"用着用着突然要重新输密码"。
constexpr int kTokenTtlSeconds = 7 * 24 * 3600;

//业务错误码：0 成功；1xxx 通用失败；11xx 控制面与限流失败。
//消息 ID 从 2xxx 开始分段，两套编号不再重叠，日志与文档里可以直接区分
enum ErrorCodes {
	Success = 0,
	Error_Json = 1001, //请求体不是 JSON 对象，或字段类型不合法
	RPCFailed = 1002, //RPC、数据库或缓存执行失败
	VerifyExpired = 1003, //验证码不存在或已过期
	VerifyCodeErr = 1004, //验证码与缓存中的值不一致
	UserExist = 1005, //用户名或邮箱已被注册
	PasswdErr = 1006, //密码错误；注册场景表示两次输入不一致
	EmailNotMatch = 1007, //重置密码时用户名与邮箱不匹配
	PasswdUpFailed = 1008, //重置密码写库失败
	PasswdInvalid = 1009, //登录时邮箱与密码不匹配
	TokenInvalid = 1010, //令牌缺失、失效或与 uid 不匹配
	UidInvalid = 1011, //uid 不存在或当前连接无权操作该用户
	RecallExpired = 1012, //超过可撤回时间
	RecallDenied = 1013, //不是发送者，或消息已撤回
	NotFriend = 1014, //双方已不是好友
	GroupNotFound = 1015, //群不存在、已解散，或请求者不在群里
	GroupDenied = 1016, //当前角色没有这个操作权限
	GroupFull = 1017, //群成员已达上限
	GroupMemberExists = 1018, //被邀请的人已经在群里
	VerifyTooFrequent = 1022, //验证码发送超过冷却或小时配额
	VerifySendFailed = 1023, //验证码已生成，但邮件投递失败
	ServiceUnavailable = 1101, //依赖服务暂不可用，客户端可稍后重试
	SessionFenced = 1102, //会话已被更高 epoch 的新登录顶替
	SessionExpired = 1103, //租约过期且未续租
	InstanceUnknown = 1104, //实例未注册或注册已过期
	TooManyRequests = 1105, //短时间内的请求次数超过上限
	ResponseTooLarge = 1106, //响应超过单帧上限，已回错误包而不是断开连接
};

//请求与响应在协议里始终成对：响应 ID 固定为请求 ID + 1。
//保留这个约定可以让分发层只维护一张请求表，也方便测试做穷举校验
constexpr std::uint16_t ResponseId(std::uint16_t request_id)
{
	return static_cast<std::uint16_t>(request_id + 1);
}

//单帧消息 ID 的上限：6xxx 段已到 6008，留出扩展空间。
//CSession 读头时先按它挡掉明显越界的 ID，具体是否注册由分发表判断
constexpr std::uint16_t MAX_REQUEST_ID = 7000;
