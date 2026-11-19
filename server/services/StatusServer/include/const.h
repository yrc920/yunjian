#pragma once
// 这个头文件包含了所有的公共头文件, 以及一些公共的定义
//

#include <boost/beast/http.hpp>
#include <boost/beast.hpp>
#include <boost/asio.hpp>
#include <boost/filesystem.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <memory>
#include <iostream>
#include <string>
#include <functional>
#include "Singleton.h"
#include <map>
#include <unordered_map>
#include <json/json.h>
#include <json/value.h>
#include <json/reader.h>
#include <atomic>
#include <queue>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <hiredis/hiredis.h>
#include <cassert>

namespace beast = boost::beast;         //from <boost/beast.hpp>
namespace http = beast::http;           //from <boost/beast/http.hpp>
namespace net = boost::asio;            //from <boost/asio.hpp>
using tcp = boost::asio::ip::tcp;       //from <boost/asio/ip/tcp.hpp>

enum ErrorCodes {
	Success = 0,
	Error_Json = 1001, //Json解析错误
	RPCFailed = 1002, //RPC请求错误
	VerifyExpired = 1003, //验证码过期
	VerifyCodeErr = 1004, //验证码错误
	UserExist = 1005, //用户已经存在
	PasswdErr = 1006, //密码错误
	EmailNotMatch = 1007, //邮箱不匹配
	PasswdUpFailed = 1008, //更新密码失败
	PasswdInvalid = 1009, //密码更新失败
	TokenInvalid = 1010,   //Token失效
	UidInvalid = 1011,  //uid无效
	ServiceUnavailable = 1101, //状态服务暂时不可用，客户端可以稍后重试
	SessionFenced = 1102, //会话已被更高 epoch 的新登录顶替
	SessionExpired = 1103, //租约过期且未续租
	InstanceUnknown = 1104, //实例未注册或注册已过期
	TooManyRequests = 1105, //同一账号短时间内的登录次数超过上限
};


#define USERTOKENPREFIX  "utoken_" //用户Token前缀
#define SESSIONPREFIX "sess_" //会话租约前缀，值为 hash: sid/inst/epoch
#define EPOCHPREFIX "epoch_" //每个 uid 的围栏令牌计数器，只增不减
#define INSTANCEPREFIX "inst_" //实例注册信息前缀，值为 hash
#define INSTANCE_SET "instances" //实例名集合，避免扫描全库

//租约 90 秒、每 30 秒续租：连续错过两次才判定离线，能容忍一次网络抖动
constexpr int LEASE_TTL_SECONDS = 90;
constexpr int RENEW_INTERVAL_SECONDS = 30;
//实例心跳随续租一起提交（30 秒一次）、90 秒过期：容忍两次遗漏，崩溃后不再分配新连接
constexpr int INSTANCE_TTL_SECONDS = 90;
constexpr int INSTANCE_HEARTBEAT_SECONDS = 30;
//令牌 7 天有效，登录时刷新；过期后必须重新走密码登录
constexpr int TOKEN_TTL_SECONDS = 7 * 24 * 3600;
//单次批量查询上限，超过由调用方分批
constexpr int MAX_BATCH_LOOKUP = 200;
//同一 uid 在 10 秒内允许的登录次数，防止客户端重连 bug 造成互踢死循环
constexpr int ALLOCATE_RATE_WINDOW_SECONDS = 10;
constexpr int ALLOCATE_RATE_LIMIT = 5;
