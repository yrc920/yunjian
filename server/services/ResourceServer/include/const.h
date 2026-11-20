#pragma once
// 资源服务器常量、错误码与延迟执行工具
//

#include <cstdint>

#include "Protocol.h"

//连接池等待上限：数据库/缓存故障时快速失败，避免文件线程被永久挂住
constexpr int MYSQL_POOL_WAIT_MS = 2000;

//上传中的临时后缀：最后一片落盘并改名后才成为正式资源，
//清理线程只动这个后缀，绝不会碰已经完成的文件
constexpr const char* UPLOAD_PART_SUFFIX = ".part";
//每用户默认配额（MB），0 表示不限制；可用 [Storage] QuotaMB 覆盖
constexpr int DEFAULT_QUOTA_MB = 1024;
//残留 .part 的保留时长：超过这个时间没有新分片写入就视为放弃
constexpr int DEFAULT_STALE_HOURS = 24;
//清理线程的巡检间隔（分钟）
constexpr int DEFAULT_SWEEP_MINUTES = 60;

//资源通道自有错误码：12xx 段与通用错误码、聊天消息 ID 都不重叠，
//客户端与日志里看到 12xx 就能直接定位到文件传输
enum ResourceError {
	FileNotExists = 1201, //服务器没有这次传输的记录
	FileSaveRedisFailed = 1202, //续传进度写入Redis失败
	FileWritePermissionFailed = 1203, //目标文件无法创建或写入
	FileDataInvalid = 1204, //分片数据与声明的偏移不一致
	FileNameInvalid = 1205, //文件名含路径分隔符或超长
	FileTooLarge = 1206, //文件超过4GB上限
	FileSeqInvalid = 1207, //分片序号不连续
	FileReadPermissionFailed = 1208, //源文件无法打开或读取
	FileOffsetInvalid = 1209, //请求的偏移已超出文件范围
	FileQuotaExceeded = 1210, //用户已用空间加上本次上传会超过配额
};

#include "Defer.h"

#define HEAD_TOTAL_LEN 6 //2字节消息ID + 4字节包体长度
#define HEAD_ID_LEN 2
#define HEAD_DATA_LEN 4

//每片原始文件字节数，客户端与服务器必须一致。
//分片以原始字节进帧（4 字节头长 + JSON 头 + 数据），10MB 图片 160 次往返
#define MAX_FILE_LEN (64 * 1024)
//单帧包体上限：64KB 数据 + 几百字节 JSON 头，留出一倍余量
#define MAX_MESSAGE_LEN (128 * 1024)
//单个文件上限，与客户端断点续传的4GB约束一致
#define MAX_FILE_SIZE (4LL * 1024 * 1024 * 1024)

#define MAX_SENDQUE 1000 //发送队列最大长度

#define LOGIC_WORKER_COUNT 4 //按会话分派的逻辑线程数
#define FILE_WORKER_COUNT 4 //按文件名分派的落盘线程数
#define DOWNLOAD_WORKER_COUNT 2 //按文件名分派的下载线程数

enum MSG_IDS {
	//6xxx 段：资源通道使用独立连接与 2+4 字节帧头，
	//与聊天主通道的 2xxx–5xxx 明确分开
	ID_UPLOAD_FILE_REQ = 6001, //通用文件分段上传
	ID_UPLOAD_FILE_RSP = 6002,
	ID_SYNC_FILE_REQ = 6003, //续传前同步服务器进度
	ID_SYNC_FILE_RSP = 6004,
	ID_DOWN_LOAD_FILE_REQ = 6005, //断点下载
	ID_DOWN_LOAD_FILE_RSP = 6006,
	ID_UPLOAD_HEAD_ICON_REQ = 6007, //头像分段上传
	ID_UPLOAD_HEAD_ICON_RSP = 6008,
};

#define USERTOKENPREFIX "utoken_" //与StatusServer、ChatServer共用的token前缀
#define USER_BASE_INFO "ubaseinfo_" //用户基本信息缓存
#define NAME_INFO "nameinfo_" //用户名索引缓存
#define FILE_INFO_PREFIX "file_upload_" //续传进度的Redis键前缀
#define FILE_INFO_EXPIRE 3600 //续传进度保留一小时
