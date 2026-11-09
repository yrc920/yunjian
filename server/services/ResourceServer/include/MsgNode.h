#pragma once
// 消息节点，2字节消息ID + 4字节包体长度
//

#include <boost/asio.hpp>
#include <string>
#include <cstdint>
#include <cstddef>
#include "const.h"

//持有定长缓冲区，便于分两段读取头部和包体
class MsgNode
{
public:
	explicit MsgNode(std::size_t max_len); //分配max_len+1字节，末尾留'\0'
	~MsgNode();
	void Clear(); //清零并复位游标

	std::size_t _cur_len; //已读/已写长度
	std::size_t _total_len; //缓冲区总长度
	char* _data; //实际数据
};

//完整请求包体
class RecvNode : public MsgNode {
	friend class LogicWorker;
public:
	RecvNode(std::size_t max_len, std::uint16_t msg_id); //只分配包体，不再带头部

private:
	std::uint16_t _msg_id; //消息ID，逻辑线程按它查处理函数
};

//待发送的完整帧，构造时写入头部
class SendNode : public MsgNode {
public:
	SendNode(const char* msg, std::size_t max_len, std::uint16_t msg_id); //构造时写入2+4头部
};
