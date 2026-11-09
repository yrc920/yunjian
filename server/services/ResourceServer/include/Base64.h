#pragma once
// Base64 编解码，用于文件分片在JSON中传输
//

#include <string>

namespace base64 {
std::string Encode(const std::string& input);
bool Decode(const std::string& input, std::string& output);
}
