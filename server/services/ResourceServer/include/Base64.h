#pragma once
// Base64 编解码，用于文件分片在JSON中传输
//

#include <string>

namespace base64 {
std::string Encode(const std::string& input); //标准Base64编码
bool Decode(const std::string& input, std::string& output); //失败返回false，不产生半截数据
}
