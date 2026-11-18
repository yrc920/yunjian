#pragma once
// 配置管理：读取工作目录下的 config.ini，按「段名 + 键名」取字符串
//
// 服务按当前工作目录读取配置，因此每个服务必须从自己的产物目录启动；
// 文件缺失或格式错误时构造函数抛异常，由 main 决定如何报错退出。
//

#include <map>
#include <string>
#include <boost/filesystem.hpp>

//一个 section 内的键值对；键不存在时返回空串
class SectionInfo
{
public:
	std::string operator[](const std::string& key) const { return GetValue(key); }

	std::string GetValue(const std::string& key) const {
		auto iter = _section_datas.find(key);
		if (iter == _section_datas.end())
			return "";
		return iter->second;
	}

private:
	friend class ConfigMgr;
	std::map<std::string, std::string> _section_datas;
};

class ConfigMgr
{
public:
	ConfigMgr(const ConfigMgr&) = delete;
	ConfigMgr& operator=(const ConfigMgr&) = delete;

	//全局唯一实例，首次调用时读取 cwd/config.ini
	static ConfigMgr& Inst();

	//返回引用：配置解析后不再变更，避免每次读字段都复制整段 map；段不存在时返回空对象
	const SectionInfo& operator[](const std::string& section) const;
	std::string GetValue(const std::string& section, const std::string& key) const;

	//文件落盘根目录：配置为空时退回到工作目录下的 storage
	boost::filesystem::path GetStoragePath() const;

private:
	ConfigMgr();

	std::map<std::string, SectionInfo> _config_map;
};
