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
	explicit MsgNode(std::size_t max_len);
	~MsgNode();
	void Clear();

	std::size_t _cur_len;
	std::size_t _total_len;
	char* _data;
};

//完整请求包体
class RecvNode : public MsgNode {
	friend class LogicWorker;
public:
	RecvNode(std::size_t max_len, std::uint16_t msg_id);

private:
	std::uint16_t _msg_id;
};

//待发送的完整帧，构造时写入头部
class SendNode : public MsgNode {
public:
	SendNode(const char* msg, std::size_t max_len, std::uint16_t msg_id);
};
