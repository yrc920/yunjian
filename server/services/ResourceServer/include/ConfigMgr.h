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
	ConfigMgr(const ConfigMgr&) = delete; //单例不允许拷贝
	ConfigMgr& operator=(const ConfigMgr&) = delete; //单例不允许赋值

	//全局唯一实例，首次调用时才读取工作目录下的config.ini
	static ConfigMgr& Inst() {
		static ConfigMgr cfg_mgr;
		return cfg_mgr;
	}

	SectionInfo operator[](const std::string& section); //按段取配置
	std::string GetValue(const std::string& section, const std::string& key); //取单字段
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

	//按键取字段，配置里没有这个键时返回空串，不抛异常
	std::string operator[](const std::string& key) {
		auto iter = _section_datas.find(key);
		if (iter == _section_datas.end())
			return "";
		return iter->second;
	}

	std::string GetValue(const std::string& key) { //与operator[]等价，供显式调用
		auto iter = _section_datas.find(key);
		if (iter == _section_datas.end())
			return "";
		return iter->second;
	}

	std::map<std::string, std::string> _section_datas; //段内的键值对
};
