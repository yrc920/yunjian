#pragma once
// 单元测试的极简断言与用例注册
//
// CTest 不提供断言，它只做两件事：运行一个命令、看退出码。
// 因此每个测试程序自己判断成功与否，全部用例通过时返回 0，否则返回非 0，
// CTest 据此把结果记为通过或失败，并保留失败输出。
//
// 用法：一个 .cpp 里写若干 XIGU_TEST(用例名) { ... }，末尾 XIGU_TEST_MAIN()。
//   XIGU_TEST(拒绝空请求) {
//       CHECK(Validate(...) == Error_Json);
//   }
//
// 断言失败不会中断当前用例，而是记录后继续，这样一次运行能看到全部问题；
// 用例之间互不影响：单个用例抛出的异常被捕获并记为失败，后续用例照常执行。
//

#include <cstdio>
#include <exception>
#include <functional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace xigutest {

struct Case
{
	const char* name;
	std::function<void()> body;
};

inline std::vector<Case>& Registry()
{
	static std::vector<Case> cases;
	return cases;
}

inline int& Failures()
{
	static int count = 0;
	return count;
}

//通过的断言数：只在收尾时汇总，用来确认用例确实跑了而不是提前返回
inline int& Passed()
{
	static int count = 0;
	return count;
}

//用例在静态初始化阶段登记，main 里统一执行
struct Registrar
{
	Registrar(const char* name, std::function<void()> body)
	{
		Registry().push_back(Case{ name, std::move(body) });
	}
};

//把断言里的实际值转成可打印文本；不认识类型时只打印占位符，
//避免为了几个用例引入额外的字符串化依赖
template <typename T>
std::string Show(const T& value)
{
	using Raw = std::decay_t<T>;
	if constexpr (std::is_same_v<Raw, std::string>) {
		return "\"" + value + "\"";
	}
	else if constexpr (std::is_same_v<Raw, bool>) {
		return value ? "true" : "false";
	}
	else if constexpr (std::is_enum_v<Raw>) {
		return std::to_string(static_cast<long long>(value));
	}
	else if constexpr (std::is_arithmetic_v<Raw>) {
		return std::to_string(value);
	}
	else if constexpr (std::is_convertible_v<T, const char*>) {
		return "\"" + std::string(value) + "\"";
	}
	else {
		return "<value>";
	}
}

inline void Fail(const char* file, int line, const std::string& message)
{
	++Failures();
	std::printf("    %s:%d  %s\n", file, line, message.c_str());
}

inline void Record(bool passed, const char* file, int line, const std::string& message)
{
	if (passed) {
		++Passed();
		return;
	}
	Fail(file, line, message);
}

//返回是否通过：集成测试用它实现「这一步失败就没必要往下走」
inline bool RecordOrStop(bool passed, const char* file, int line, const std::string& message)
{
	if (passed) {
		++Passed();
		return true;
	}
	Fail(file, line, message);
	return false;
}

//顺序流程的收尾：打印总数并给出退出码
inline int Finish()
{
	std::printf("\n通过 %d 项，失败 %d 项\n", Passed(), Failures());
	return Failures() == 0 ? 0 : 1;
}

template <typename A, typename B>
void RecordEq(const A& actual, const B& expected, const char* file, int line,
	const char* actual_text, const char* expected_text)
{
	if (actual == expected) {
		++Passed();
		return;
	}
	Fail(file, line, std::string(actual_text) + " == " + expected_text
		+ "\n      实际: " + Show(actual) + "\n      期望: " + Show(expected));
}

//按登记顺序执行全部用例，返回进程退出码
inline int RunAll()
{
	int failed_cases = 0;
	for (const auto& item : Registry()) {
		const int before = Failures();
		std::printf("[ RUN  ] %s\n", item.name);
		try {
			item.body();
		}
		catch (const std::exception& error) {
			Fail(__FILE__, __LINE__, std::string("用例抛出异常: ") + error.what());
		}
		catch (...) {
			Fail(__FILE__, __LINE__, "用例抛出未知异常");
		}
		const int failed = Failures() - before;
		if (failed == 0) {
			std::printf("[  OK  ] %s\n", item.name);
		}
		else {
			++failed_cases;
			std::printf("[ FAIL ] %s（%d 处断言失败）\n", item.name, failed);
		}
	}
	std::printf("\n共 %d 个用例，失败 %d 个\n",
		static_cast<int>(Registry().size()), failed_cases);
	return failed_cases == 0 ? 0 : 1;
}

} // namespace xigutest

//用例名允许带空格与中文，所以不能用它拼函数名：
//符号由行号生成，名字只作为字符串进注册表
#define XIGU_CONCAT_INNER(left, right) left##right
#define XIGU_CONCAT(left, right) XIGU_CONCAT_INNER(left, right)

#define XIGU_TEST(name) \
	static void XIGU_CONCAT(xigu_case_, __LINE__)(); \
	static ::xigutest::Registrar XIGU_CONCAT(xigu_reg_, __LINE__)( \
		#name, XIGU_CONCAT(xigu_case_, __LINE__)); \
	static void XIGU_CONCAT(xigu_case_, __LINE__)()

#define CHECK(condition) \
	::xigutest::Record(static_cast<bool>(condition), __FILE__, __LINE__, \
		"CHECK(" #condition ")")

#define CHECK_EQ(actual, expected) \
	::xigutest::RecordEq((actual), (expected), __FILE__, __LINE__, #actual, #expected)

#define CHECK_THROWS(expression) \
	do { \
		bool xigu_threw = false; \
		try { (void)(expression); } \
		catch (...) { xigu_threw = true; } \
		::xigutest::Record(xigu_threw, __FILE__, __LINE__, \
			"CHECK_THROWS(" #expression ") 没有抛出异常"); \
	} while (false)

//集成测试用：前置条件不成立时直接结束用例，
//避免拿着空的 thread_id 继续发请求，把一条失败扩散成一屏失败
#define REQUIRE(condition) \
	if (!::xigutest::RecordOrStop(static_cast<bool>(condition), __FILE__, __LINE__, \
		"REQUIRE(" #condition ")")) return 1

#define XIGU_TEST_MAIN() \
	int main() { return ::xigutest::RunAll(); }

//顺序流程（集成测试）的收尾：没有失败才算通过
#define XIGU_TEST_SUMMARY() (::xigutest::Finish())
