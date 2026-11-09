#pragma once
// 配置管理类，读取工作目录下的 config.ini
//

#include <fstream>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/filesystem.hpp>
#include <map>
#include <iostream>

struct SectionInfo;

class ConfigMgr
{
public:
	~ConfigMgr();
	ConfigMgr(const ConfigMgr&) = delete;
	ConfigMgr& operator=(const ConfigMgr&) = delete;

	static ConfigMgr& Inst() {
		static ConfigMgr cfg_mgr;
		return cfg_mgr;
	}

	SectionInfo operator[](const std::string& section);
	std::string GetValue(const std::string& section, const std::string& key);
	//文件落盘根目录，配置为空时退回到工作目录下的 storage
	boost::filesystem::path GetStoragePath();

private:
	ConfigMgr();

	std::map<std::string, SectionInfo> _config_map;
};

struct SectionInfo {
	SectionInfo() {}
	SectionInfo(const SectionInfo& src) { _section_datas = src._section_datas; }
	SectionInfo& operator=(const SectionInfo& src) {
		if (&src == this)
			return *this;
		_section_datas = src._section_datas;
		return *this;
	}
	~SectionInfo() { _section_datas.clear(); }

	std::string operator[](const std::string& key) {
		auto iter = _section_datas.find(key);
		if (iter == _section_datas.end())
			return "";
		return iter->second;
	}

	std::string GetValue(const std::string& key) {
		auto iter = _section_datas.find(key);
		if (iter == _section_datas.end())
			return "";
		return iter->second;
	}

	std::map<std::string, std::string> _section_datas;
};
