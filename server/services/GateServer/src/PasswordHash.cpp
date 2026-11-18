#include "PasswordHash.h"

#include <iostream>
#include <mutex>

#include <sodium.h>

namespace {
//Argon2id 的运算开销，取 libsodium 的"交互式"档：
//2 次迭代、64 MiB 内存。内网企业聊天不需要 256 MiB 的离线破解成本，
//而 256 MiB × 并发登录会在早高峰直接把 GateServer 的内存和 CPU 打满
constexpr unsigned long long kOpsLimit = crypto_pwhash_OPSLIMIT_INTERACTIVE;
constexpr std::size_t kMemLimit = crypto_pwhash_MEMLIMIT_INTERACTIVE;

//libsodium 要求使用前先初始化，用 once_flag 保证只做一次且不依赖静态初始化顺序
void EnsureSodiumReady()
{
    static std::once_flag flag;
    std::call_once(flag, []() {
        //sodium_init 成功返回 0，失败返回 -1（此后哈希与校验都会失败）
        if (sodium_init() != 0) {
            std::cerr << "libsodium init failed" << std::endl;
        }
    });
}
}

std::string PasswordHash::Hash(const std::string& plain)
{
    EnsureSodiumReady();

    //crypto_pwhash_STRBYTES 已含结尾 '\0'，crypto_pwhash_str 保证以 '\0' 收尾
    char out[crypto_pwhash_STRBYTES];
    if (crypto_pwhash_str(out, plain.data(), plain.size(), kOpsLimit, kMemLimit) != 0) {
        return {}; //参数非法或内存不足
    }
    return std::string(out);
}

bool PasswordHash::Verify(const std::string& plain, const std::string& stored)
{
    EnsureSodiumReady();

    //校验失败（含存储串格式不合法）返回非 0；比较内部为常量时间实现
    return crypto_pwhash_str_verify(stored.c_str(), plain.data(), plain.size()) == 0;
}

bool PasswordHash::NeedsRehash(const std::string& stored)
{
    EnsureSodiumReady();
    //参数已经与当前目标一致时返回 0；旧账号的 256 MiB 哈希会在这里被识别出来
    return crypto_pwhash_str_needs_rehash(stored.c_str(), kOpsLimit, kMemLimit) != 0;
}
