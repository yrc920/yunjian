#include "Base64.h"
#include <array>

namespace {
constexpr char kAlphabet[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

//反查表在首次调用时构造，非法字符用-1表示
const std::array<int, 256>& DecodeTable()
{
	static const std::array<int, 256> table = [] {
		std::array<int, 256> values{};
		values.fill(-1);
		for (int i = 0; i < 64; ++i)
			values[static_cast<unsigned char>(kAlphabet[i])] = i;
		return values;
	}();
	return table;
}
}

std::string base64::Encode(const std::string& input)
{
	std::string output;
	output.reserve((input.size() + 2) / 3 * 4);

	const auto* data = reinterpret_cast<const unsigned char*>(input.data());
	std::size_t index = 0;
	for (; index + 2 < input.size(); index += 3) {
		const unsigned int value = (data[index] << 16) | (data[index + 1] << 8) | data[index + 2];
		output.push_back(kAlphabet[(value >> 18) & 0x3F]);
		output.push_back(kAlphabet[(value >> 12) & 0x3F]);
		output.push_back(kAlphabet[(value >> 6) & 0x3F]);
		output.push_back(kAlphabet[value & 0x3F]);
	}

	const std::size_t remain = input.size() - index;
	if (remain == 1) {
		const unsigned int value = data[index] << 16;
		output.push_back(kAlphabet[(value >> 18) & 0x3F]);
		output.push_back(kAlphabet[(value >> 12) & 0x3F]);
		output.append("==");
	}
	else if (remain == 2) {
		const unsigned int value = (data[index] << 16) | (data[index + 1] << 8);
		output.push_back(kAlphabet[(value >> 18) & 0x3F]);
		output.push_back(kAlphabet[(value >> 12) & 0x3F]);
		output.push_back(kAlphabet[(value >> 6) & 0x3F]);
		output.push_back('=');
	}
	return output;
}

bool base64::Decode(const std::string& input, std::string& output)
{
	output.clear();
	output.reserve(input.size() / 4 * 3);

	const auto& table = DecodeTable();
	unsigned int buffer = 0;
	int bits = 0;
	for (char ch : input) {
		const unsigned char byte = static_cast<unsigned char>(ch);
		if (byte == '=' || byte == '\n' || byte == '\r')
			continue;

		const int value = table[byte];
		if (value < 0)
			return false;

		buffer = (buffer << 6) | static_cast<unsigned int>(value);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			output.push_back(static_cast<char>((buffer >> bits) & 0xFF));
		}
	}
	return true;
}
