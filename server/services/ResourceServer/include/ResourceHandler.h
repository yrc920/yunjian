#pragma once
// 资源服务器协议处理：上传、续传同步与下载
//

#include <memory>
#include <string>
#include "CSession.h"

namespace resource {
void HandleUpload(std::shared_ptr<CSession> session, const std::string& data, bool avatar); //上传一片
void HandleSync(std::shared_ptr<CSession> session, const std::string& data); //查询已落盘进度
void HandleDownload(std::shared_ptr<CSession> session, const std::string& data); //下载一片
}
