#include "global.h"

QString gate_url_prefix = ""; //初始化
bool tcp_tls_enabled = false; //默认明文，与服务端未配证书时的行为一致

//密码发送前的可逆变形: 长度对 255 取模, 再把前 length 个字符与 length 异或。
//网关按收到的字符串做 Argon2 哈希, 库里的历史密码也是变形值的哈希,
//所以客户端每一条发密码的请求都要过这个函数, 发原文会被判为密码错误
std::function<QString(QString)> xorString = [](QString input) {
    QString result = input;
    int length = input.length() % 255;
    for (int i = 0; i < length; ++i)
        result[i] = QChar(static_cast<ushort>(input[i].unicode() ^ static_cast<ushort>(length)));
    return result;
};
