#include "MsgNode.h"
#include <cstring>

MsgNode::MsgNode(std::size_t max_len) : _cur_len(0), _total_len(max_len)
{
	_data = new char[_total_len + 1](); //分配内存，并初始化为0
	_data[_total_len] = '\0'; //确保数据以null结尾，方便字符串处理
}

MsgNode::~MsgNode()
{
	delete[] _data; //释放内存
}

void MsgNode::Clear()
{
	::memset(_data, 0, _total_len); //清空数据
	_cur_len = 0; //重置当前长度
}


RecvNode::RecvNode(std::size_t max_len, std::uint16_t msg_id) : MsgNode(max_len), _msg_id(msg_id)
{

}


SendNode::SendNode(const char* msg, std::size_t max_len, std::uint16_t msg_id) : MsgNode(max_len + HEAD_TOTAL_LEN)
, _msg_id(msg_id)
{
	//先发送id, 转为网络字节序
	std::uint16_t msg_id_host = boost::asio::detail::socket_ops::host_to_network_short(msg_id);
	memcpy(_data, &msg_id_host, HEAD_ID_LEN);
	//转为网络字节序；长度字段是 2 字节，调用方保证 max_len <= 65531
	std::uint16_t max_len_host = boost::asio::detail::socket_ops::host_to_network_short(
		static_cast<std::uint16_t>(max_len));
	memcpy(_data + HEAD_ID_LEN, &max_len_host, HEAD_DATA_LEN);
	memcpy(_data + HEAD_ID_LEN + HEAD_DATA_LEN, msg, max_len);
}
