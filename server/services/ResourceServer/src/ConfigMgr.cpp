#include "ConfigMgr.h"

ConfigMgr::ConfigMgr() {
	boost::filesystem::path current_path = boost::filesystem::current_path();
	boost::filesystem::path config_path = current_path / "config.ini";
	std::cout << "Config path: " << config_path << std::endl;

	//配置缺失时直接抛给main，避免服务带着空端口或空存储目录启动
	boost::property_tree::ptree pt;
	boost::property_tree::read_ini(config_path.string(), pt);

	for (const auto& section_pair : pt) {
		const std::string& section_name = section_pair.first;
		const boost::property_tree::ptree& section_tree = section_pair.second;

		std::map<std::string, std::string> section_config;
		for (const auto& key_value_pair : section_tree) {
			section_config[key_value_pair.first] = key_value_pair.second.get_value<std::string>();
		}
		SectionInfo section_info;
		section_info._section_datas = section_config;
		_config_map[section_name] = section_info;
	}
}

ConfigMgr::~ConfigMgr()
{
	_config_map.clear();
}

SectionInfo ConfigMgr::operator[](const std::string& section)
{
	auto iter = _config_map.find(section);
	if (iter == _config_map.end())
		return SectionInfo();
	return iter->second;
}

std::string ConfigMgr::GetValue(const std::string& section, const std::string& key)
{
	auto iter = _config_map.find(section);
	if (iter == _config_map.end())
		return "";
	return iter->second.GetValue(key);
}

boost::filesystem::path ConfigMgr::GetStoragePath()
{
	auto path = GetValue("Storage", "Path");
	if (path.empty())
		path = "storage";
	return boost::filesystem::path(path);
}
