#pragma once
// 作用域守卫：析构时执行构造时传入的函数
//
// 用于「任何分支退出都要收尾」的场景（归还数据库连接、恢复状态、记录耗时）。
// 禁止拷贝：同一份清理被执行两次通常比不执行更糟。允许移动，便于从工厂函数返回。

#include <functional>
#include <utility>

class Defer {
public:
	explicit Defer(std::function<void()> func) : _func(std::move(func)) {}
	~Defer() {
		if (_func)
			_func();
	}

	Defer(const Defer&) = delete;
	Defer& operator=(const Defer&) = delete;
	Defer(Defer&& other) noexcept : _func(std::move(other._func)) {
		other._func = nullptr; //移走之后源对象不能再触发一次
	}
	Defer& operator=(Defer&&) = delete;

private:
	std::function<void()> _func;
};
