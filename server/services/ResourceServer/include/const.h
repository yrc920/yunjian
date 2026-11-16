#pragma once
// 资源服务器常量、错误码与延迟执行工具
//

#include <functional>
#include <cstdint>

enum ErrorCodes {
	Success = 0,
	Error_Json = 1001, //Json解析错误
	TokenInvalid = 1010, //token与uid不匹配或已失效
	UidInvalid = 1011, //uid无效
	FileNotExists = 1012, //服务器没有这次传输的记录
	FileSaveRedisFailed = 1013, //续传进度写入Redis失败
	FileWritePermissionFailed = 1014, //目标文件无法创建或写入
	FileDataInvalid = 1015, //分片数据与声明的偏移不一致
	FileNameInvalid = 1016, //文件名含路径分隔符或超长
	FileTooLarge = 1017, //文件超过4GB上限
	FileSeqInvalid = 1018, //分片序号不连续
	FileReadPermissionFailed = 1019, //源文件无法打开或读取
	FileOffsetInvalid = 1020, //请求的偏移已超出文件范围
};

//go语言风格的延迟执行
class Defer {
public:
	explicit Defer(std::function<void()> func) : func_(std::move(func)) {}
	~Defer() {
		func_();
	}

private:
	std::function<void()> func_;
};

#define HEAD_TOTAL_LEN 6 //2字节消息ID + 4字节包体长度
#define HEAD_ID_LEN 2
#define HEAD_DATA_LEN 4

//每片原始文件字节数，客户端与服务器必须一致
#define MAX_FILE_LEN (1024 * 2)
//单帧包体上限：2KB原始数据经Base64和JSON包装后仍留出充足余量
#define MAX_MESSAGE_LEN (16 * 1024)
//单个文件上限，与客户端断点续传的4GB约束一致
#define MAX_FILE_SIZE (4LL * 1024 * 1024 * 1024)

#define MAX_SENDQUE 1000 //发送队列最大长度

#define LOGIC_WORKER_COUNT 4 //按会话分派的逻辑线程数
#define FILE_WORKER_COUNT 4 //按文件名分派的落盘线程数
#define DOWNLOAD_WORKER_COUNT 2 //按文件名分派的下载线程数

enum MSG_IDS {
	//沿用 1000 段系统命令编号，接在聊天协议之后；资源服务器使用自己的连接与帧头
	ID_UPLOAD_FILE_REQ = 1037, //通用文件分段上传
	ID_UPLOAD_FILE_RSP = 1038,
	ID_SYNC_FILE_REQ = 1039, //续传前同步服务器进度
	ID_SYNC_FILE_RSP = 1040,
	ID_DOWN_LOAD_FILE_REQ = 1041, //断点下载
	ID_DOWN_LOAD_FILE_RSP = 1042,
	ID_UPLOAD_HEAD_ICON_REQ = 1043, //头像分段上传
	ID_UPLOAD_HEAD_ICON_RSP = 1044,
};

#define USERTOKENPREFIX "utoken_" //与StatusServer、ChatServer共用的token前缀
#define USER_BASE_INFO "ubaseinfo_" //用户基本信息缓存
#define NAME_INFO "nameinfo_" //用户名索引缓存
#define FILE_INFO_PREFIX "file_upload_" //续传进度的Redis键前缀
#define FILE_INFO_EXPIRE 3600 //续传进度保留一小时
