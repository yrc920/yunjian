#pragma once
// 资源服务器协议处理：上传、续传同步与下载
//

#include <memory>
#include <string>
#include "CSession.h"

namespace resource {
void HandleUpload(std::shared_ptr<CSession> session, const std::string& data, bool avatar);
void HandleSync(std::shared_ptr<CSession> session, const std::string& data);
void HandleDownload(std::shared_ptr<CSession> session, const std::string& data);
}
