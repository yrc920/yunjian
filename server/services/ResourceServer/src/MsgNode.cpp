#include "MsgNode.h"
#include <cstring>

MsgNode::MsgNode(std::size_t max_len) : _cur_len(0), _total_len(max_len)
{
	_data = new char[_total_len + 1](); //多1字节给'\0'
	_data[_total_len] = '\0';
}

MsgNode::~MsgNode()
{
	delete[] _data; //数组释放
}

void MsgNode::Clear()
{
	std::memset(_data, 0, _total_len); //清掉上一帧内容
	_cur_len = 0;
}

RecvNode::RecvNode(std::size_t max_len, std::uint16_t msg_id)
	: MsgNode(max_len), _msg_id(msg_id) {} //只分配包体

SendNode::SendNode(const char* msg, std::size_t max_len, std::uint16_t msg_id)
	: MsgNode(max_len + HEAD_TOTAL_LEN) //预留6字节头
{
	std::uint16_t id_be = boost::asio::detail::socket_ops::host_to_network_short(msg_id); //网络序ID
	memcpy(_data, &id_be, HEAD_ID_LEN);

	std::uint32_t len_be = boost::asio::detail::socket_ops::host_to_network_long(
		static_cast<std::uint32_t>(max_len)); //4字节网络序长度
	memcpy(_data + HEAD_ID_LEN, &len_be, HEAD_DATA_LEN);

	memcpy(_data + HEAD_TOTAL_LEN, msg, max_len); //拷贝JSON包体
}
