#pragma once
// 进程内单例模板：首次 GetInstance 时构造，进程退出时释放
//

#include <memory>
#include <mutex>

template <typename T>
class Singleton {
protected:
	Singleton() = default;
	Singleton(const Singleton<T>&) = delete;
	Singleton& operator=(const Singleton<T>&) = delete;

	static std::shared_ptr<T> _instance;

public:
	static std::shared_ptr<T> GetInstance() {
		static std::once_flag flag; //并发首次调用只构造一次
		std::call_once(flag, []() { _instance = std::shared_ptr<T>(new T); });
		return _instance;
	}

	virtual ~Singleton() = default;
};

template <typename T>
std::shared_ptr<T> Singleton<T>::_instance = nullptr;
