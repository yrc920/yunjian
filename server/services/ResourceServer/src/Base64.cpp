#include "Base64.h"
#include <array>

namespace {
constexpr char kAlphabet[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

const std::array<int, 256>& DecodeTable()
{
	static const std::array<int, 256> table = [] { //只构造一次
		std::array<int, 256> values{};
		values.fill(-1); //非法字符
		for (int i = 0; i < 64; ++i)
			values[static_cast<unsigned char>(kAlphabet[i])] = i; //字母表反查
		return values;
	}();
	return table;
}
}

std::string base64::Encode(const std::string& input)
{
	std::string output;
	output.reserve((input.size() + 2) / 3 * 4); //每3字节变4字符

	const auto* data = reinterpret_cast<const unsigned char*>(input.data());
	std::size_t index = 0;
	for (; index + 2 < input.size(); index += 3) { //整3字节一组
		const unsigned int value = (data[index] << 16) | (data[index + 1] << 8) | data[index + 2];
		output.push_back(kAlphabet[(value >> 18) & 0x3F]); //高6位
		output.push_back(kAlphabet[(value >> 12) & 0x3F]); //次6位
		output.push_back(kAlphabet[(value >> 6) & 0x3F]); //再次6位
		output.push_back(kAlphabet[value & 0x3F]); //低6位
	}

	const std::size_t remain = input.size() - index; //末尾余1或2字节
	if (remain == 1) {
		const unsigned int value = data[index] << 16;
		output.push_back(kAlphabet[(value >> 18) & 0x3F]);
		output.push_back(kAlphabet[(value >> 12) & 0x3F]);
		output.append("=="); //补两个等号
	}
	else if (remain == 2) {
		const unsigned int value = (data[index] << 16) | (data[index + 1] << 8);
		output.push_back(kAlphabet[(value >> 18) & 0x3F]);
		output.push_back(kAlphabet[(value >> 12) & 0x3F]);
		output.push_back(kAlphabet[(value >> 6) & 0x3F]);
		output.push_back('='); //补一个等号
	}
	return output;
}

bool base64::Decode(const std::string& input, std::string& output)
{
	output.clear();
	output.reserve(input.size() / 4 * 3); //估算输出大小

	const auto& table = DecodeTable();
	unsigned int buffer = 0; //攒6位
	int bits = 0; //当前有效位数
	for (char ch : input) {
		const unsigned char byte = static_cast<unsigned char>(ch);
		if (byte == '=' || byte == '\n' || byte == '\r')
			continue; //跳过填充与换行

		const int value = table[byte];
		if (value < 0)
			return false; //非法字符，整片作废

		buffer = (buffer << 6) | static_cast<unsigned int>(value); //拼入6位
		bits += 6;
		if (bits >= 8) { //够一个字节就输出
			bits -= 8;
			output.push_back(static_cast<char>((buffer >> bits) & 0xFF));
		}
	}
	return true;
}
