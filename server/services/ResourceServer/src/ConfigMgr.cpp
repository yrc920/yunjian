#include "ConfigMgr.h"

ConfigMgr::ConfigMgr() {
	boost::filesystem::path current_path = boost::filesystem::current_path(); //服务必须从自己的产物目录启动
	boost::filesystem::path config_path = current_path / "config.ini"; //固定读cwd下的配置
	std::cout << "Config path: " << config_path << std::endl;

	//配置缺失时直接抛给main，避免服务带着空端口或空存储目录启动
	boost::property_tree::ptree pt;
	boost::property_tree::read_ini(config_path.string(), pt); //解析INI

	for (const auto& section_pair : pt) {
		const std::string& section_name = section_pair.first; //段名
		const boost::property_tree::ptree& section_tree = section_pair.second; //段内键值

		std::map<std::string, std::string> section_config;
		for (const auto& key_value_pair : section_tree) {
			section_config[key_value_pair.first] = key_value_pair.second.get_value<std::string>(); //全部按字符串保存
		}
		SectionInfo section_info;
		section_info._section_datas = section_config; //复制进SectionInfo
		_config_map[section_name] = section_info; //按段名索引
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
		return SectionInfo(); //缺失段返回空对象，调用方取字段得到空串
	return iter->second;
}

std::string ConfigMgr::GetValue(const std::string& section, const std::string& key)
{
	auto iter = _config_map.find(section);
	if (iter == _config_map.end())
		return ""; //段不存在
	return iter->second.GetValue(key); //键不存在时SectionInfo也返回空串
}

boost::filesystem::path ConfigMgr::GetStoragePath()
{
	auto path = GetValue("Storage", "Path");
	if (path.empty())
		path = "storage"; //未配置时用工作目录下的storage
	return boost::filesystem::path(path); //相对/绝对路径都交给filesystem
}
