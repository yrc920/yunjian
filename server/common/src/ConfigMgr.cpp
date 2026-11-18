#include "ConfigMgr.h"

#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>

ConfigMgr& ConfigMgr::Inst()
{
	static ConfigMgr inst;
	return inst;
}

ConfigMgr::ConfigMgr()
{
	boost::filesystem::path config_path = boost::filesystem::current_path() / "config.ini";
	boost::property_tree::ptree pt;
	boost::property_tree::read_ini(config_path.string(), pt); //缺失或格式错误由 main 报错退出

	for (const auto& section_pair : pt) {
		std::map<std::string, std::string> values;
		for (const auto& item : section_pair.second)
			values[item.first] = item.second.get_value<std::string>(); //全部按字符串保存

		SectionInfo info;
		info._section_datas = std::move(values);
		_config_map[section_pair.first] = std::move(info);
	}
}

const SectionInfo& ConfigMgr::operator[](const std::string& section) const
{
	auto iter = _config_map.find(section);
	if (iter == _config_map.end()) {
		static const SectionInfo empty; //缺失段返回空对象，取字段得到空串
		return empty;
	}
	return iter->second;
}

std::string ConfigMgr::GetValue(const std::string& section, const std::string& key) const
{
	auto iter = _config_map.find(section);
	if (iter == _config_map.end())
		return "";
	return iter->second.GetValue(key);
}

boost::filesystem::path ConfigMgr::GetStoragePath() const
{
	auto path = GetValue("Storage", "Path");
	if (path.empty())
		path = "storage"; //未配置时用工作目录下的 storage
	return boost::filesystem::path(path);
}
