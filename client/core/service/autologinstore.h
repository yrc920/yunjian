#ifndef AUTOLOGINSTORE_H
#define AUTOLOGINSTORE_H

#include <QString>

#include "global.h"

/******************************************************************************
 * @file       autologinstore.h
 * @brief      自动登录凭据：只保存服务端签发的令牌，不保存密码
 *
 * 网关签发的令牌 7 天有效，退出登录或过期后自动登录会失败，退回手工输入密码。
 * 记录存放在本机配置里（用户目录下的 plist），不上传、不参与同步。
 *****************************************************************************/

class AutoLoginStore
{
public:
    struct Record {
        QString Email;
        ServerInfo Info;
        qint64 SavedAt = 0; // Unix 秒，用来判断令牌是否还在有效期内
    };

    // 用户是否勾选了「自动登录」；没有设置过时默认开启
    static bool Remembered();
    static void SetRemembered(bool on);

    // 读取可用记录：未勾选、字段不全、超过 7 天都返回 false
    static bool Load(Record &out);
    static void Save(const Record &record);
    static void Clear();
};

#endif // AUTOLOGINSTORE_H
