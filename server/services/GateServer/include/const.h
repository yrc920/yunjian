#pragma once
// 这个头文件包含了所有的公共头文件, 以及一些公共的定义

#include <boost/beast/http.hpp>
#include <boost/beast.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/filesystem.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <memory>
#include <iostream>
#include <string>
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
namespace ssl = boost::asio::ssl;       //from <boost/asio/ssl.hpp>
using tcp = boost::asio::ip::tcp;       //from <boost/asio/ip/tcp.hpp>

#include "Protocol.h"
#include "Defer.h"

#define CODEPREFIX "code_"

//验证码发送限流：同一邮箱 60 秒冷却，每小时 5 次；同一 IP 每小时 20 次
#define CODE_COOLDOWN_SECONDS 60
#define CODE_RATE_WINDOW_SECONDS 3600
#define CODE_EMAIL_HOURLY_LIMIT 5
#define CODE_IP_HOURLY_LIMIT 20
#define COOLDOWNPREFIX "code_cd_"

//登录失败限流：同一账号 60 秒 5 次，同一 IP 60 秒 20 次。
//只统计失败，成功后清零，正常用户偶发输错不会被长期锁住
#define LOGIN_FAIL_PREFIX "loginfail_"
#define LOGIN_FAIL_IP_PREFIX "loginfail_ip_"
#define LOGIN_FAIL_WINDOW_SECONDS 60
#define LOGIN_ACCOUNT_FAIL_LIMIT 5
#define LOGIN_IP_FAIL_LIMIT 20

//连接池等待上限：数据库/缓存故障时快速失败，避免 HTTP IO 线程被永久挂住
constexpr int MYSQL_POOL_WAIT_MS = 2000;
//口令哈希线程数：每个线程峰值 64 MiB，默认 2 个；
//队列排满后直接回可重试错误，不让登录请求堆在 IO 线程上
constexpr int DEFAULT_PASSWORD_THREADS = 2;
constexpr int DEFAULT_PASSWORD_QUEUE = 128;
#define RATEEMAILPREFIX "code_hr_"
#define RATEIPPREFIX "code_ip_"
