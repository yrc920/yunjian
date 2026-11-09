#pragma once
// Redis 连接池与基础读写接口
//

#include "const.h"
#include <hiredis/hiredis.h>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <string>
#include "Singleton.h"

class RedisMgr : public Singleton<RedisMgr>,
	public std::enable_shared_from_this<RedisMgr>
{
	friend class Singleton<RedisMgr>;
public:
	~RedisMgr();

	bool Get(const std::string& key, std::string& value);
	bool Set(const std::string& key, const std::string& value);
	//写入并设置过期秒数，续传进度需要自动过期
	bool SetExp(const std::string& key, const std::string& value, int expire_seconds);
	bool Del(const std::string& key);

	void Close();

private:
	RedisMgr();

	redisContext* Borrow(); //取一条空闲连接，必要时新建
	void GiveBack(redisContext* context, bool healthy);
	redisContext* Connect(); //新建带认证的连接

	std::queue<redisContext*> _idle;
	std::mutex _mtx;
	std::condition_variable _cv;
	std::size_t _total = 0; //空闲与借出的连接总数
	std::atomic<bool> _stopped{false};

	std::string _host;
	int _port = 6379;
	std::string _password;
	static constexpr std::size_t kPoolSize = 8;
};
