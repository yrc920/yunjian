// 资源通道的端到端用例：分段上传 → 断点下载 → 内容比对，外加五条边界
//
// 重点不是吞吐（那是压测脚本的事），而是「分片边界算错一位就会静默损坏文件」：
// 64KB 正好一片、64KB+1 两片、末片的大小与 last 标记、下载越界与不存在的资源。
//
// 上传回包是纯 JSON，下载回包带 4 字节头长——两类包体格式不同，
// 这里按线上实际协议收包，见 TestClient.h 的说明。

#include "TestClient.h"
#include "TestSupport.h"
#include "const.h" //ResourceServer 的消息 ID 与分片上限

#include <boost/filesystem.hpp>
#include <json/json.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>
#include <vector>

using namespace testclient;

namespace {

constexpr std::size_t kChunk = MAX_FILE_LEN; //64KB，与客户端一致

//确定性字节序列：失败时能按偏移复现，不用保存中间文件
std::string MakePayload(std::size_t size, unsigned seed)
{
	std::string data(size, '\0');
	for (std::size_t i = 0; i < size; ++i)
		data[i] = static_cast<char>((i * 29 + seed * 7) % 256);
	return data;
}

//每次运行的 md5 与文件名都不同：md5 是续传键，重复会让上一次的进度记录串进来
std::string RandomMd5()
{
	static std::mt19937 rng(std::random_device{}());
	std::uniform_int_distribution<int> digit(0, 15);
	std::string text(32, '0');
	for (auto& ch : text)
		ch = "0123456789abcdef"[digit(rng)];
	return text;
}

struct UploadResult
{
	bool transport_ok = false;
	Json::Value response;
};

//按 seq 顺序发完整个文件，每片等一个回包（窗口为 1，失败点更明确）
UploadResult Upload(ResourceConn& conn, const LoginResult& login, const std::string& name,
	const std::string& md5, const std::string& payload, std::string& error)
{
	UploadResult result;
	const std::size_t total = payload.size();
	std::size_t offset = 0;
	int seq = 1;
	while (offset < total) {
		const std::size_t length = std::min(kChunk, total - offset);

		Json::Value header;
		header["md5"] = md5;
		header["name"] = name;
		header["seq"] = seq;
		header["trans_size"] = static_cast<Json::Int64>(offset + length);
		header["total_size"] = static_cast<Json::Int64>(total);
		header["last"] = (offset + length >= total) ? 1 : 0;
		header["uid"] = login.uid;
		header["token"] = login.token;

		if (!conn.Send(ID_UPLOAD_FILE_REQ, header, payload.substr(offset, length), error))
			return result;

		std::uint16_t response_id = 0;
		std::string no_payload;
		if (!conn.RecvFrame(response_id, result.response, no_payload, error))
			return result;
		if (response_id != ID_UPLOAD_FILE_RSP) {
			error = "上传回包 ID 异常: " + std::to_string(response_id);
			return result;
		}
		if (IntField(result.response["error"]) != 0) {
			result.transport_ok = true; //链路正常，是业务拒绝了这一片
			return result;
		}
		offset += length;
		++seq;
	}
	result.transport_ok = true;
	return result;
}

//按 seq 拉完整文件；失败时 error 带上服务端返回的错误码
bool Download(ResourceConn& conn, const LoginResult& login, int owner, const std::string& name,
	std::string& out, int& server_error, std::string& error)
{
	out.clear();
	server_error = 0;
	for (int seq = 1; seq <= 200; ++seq) {
		Json::Value header;
		header["uid"] = login.uid;
		header["token"] = login.token;
		header["owner_uid"] = owner;
		header["name"] = name;
		header["seq"] = seq;
		if (!conn.Send(ID_DOWN_LOAD_FILE_REQ, header, "", error))
			return false;

		std::uint16_t response_id = 0;
		Json::Value response;
		std::string chunk;
		if (!conn.RecvFrame(response_id, response, chunk, error))
			return false;
		if (response_id != ID_DOWN_LOAD_FILE_RSP) {
			error = "下载回包 ID 异常: " + std::to_string(response_id);
			return false;
		}
		server_error = IntField(response["error"]);
		if (server_error != 0) {
			error = "下载被拒绝, error=" + std::to_string(server_error);
			return false;
		}
		out += chunk;
		if (BoolField(response["is_last"]))
			return true;
	}
	error = "下载分片数超过上限";
	return false;
}

} // namespace

int main(int argc, char** argv)
{
	Options options;
	if (!ParseOptions(argc, argv, options))
		return 1;

	if (!PortOpen(options.gate) || !PortOpen(options.resource)) {
		std::cout << "SKIP: 网关或资源服务未监听（" << options.gate.host << ":"
			<< options.gate.port << " / " << options.resource.host << ":"
			<< options.resource.port << "），本地栈未启动\n";
		return 77;
	}

	std::string error;
	LoginResult login;
	REQUIRE(HttpLogin(options.gate, options.email, options.passwd, login, error));
	std::cout << "登录成功 uid=" << login.uid << "\n";

	ResourceConn conn;
	REQUIRE(conn.Connect(options.resource, error));

	//本次运行产生的文件，结束时一并清理
	std::vector<std::string> created;

	// ---------- 主用例：150000 字节，三片 ----------
	const std::string payload = MakePayload(150000, 3);
	const std::string md5 = RandomMd5();
	const std::string name = "ctest-" + md5.substr(0, 8) + ".bin";
	created.push_back(name);

	auto upload = Upload(conn, login, name, md5, payload, error);
	REQUIRE(upload.transport_ok);
	CHECK_EQ(IntField(upload.response["error"]), 0);
	CHECK_EQ(IntField(upload.response["last"]), 1);
	CHECK_EQ(Int64Field(upload.response["trans_size"]),
		static_cast<Json::Int64>(payload.size()));

	std::string downloaded;
	int server_error = 0;
	REQUIRE(Download(conn, login, login.uid, name, downloaded, server_error, error));
	CHECK_EQ(downloaded.size(), payload.size());
	CHECK(downloaded == payload);

	// ---------- 边界：正好 64KB，一片就是末片 ----------
	const std::string one_chunk = MakePayload(kChunk, 5);
	const std::string one_name = "ctest-one-" + RandomMd5().substr(0, 8) + ".bin";
	created.push_back(one_name);
	upload = Upload(conn, login, one_name, RandomMd5(), one_chunk, error);
	REQUIRE(upload.transport_ok);
	CHECK_EQ(IntField(upload.response["error"]), 0);
	//单片上传时 last 必须为 1，否则客户端会一直等下一片
	CHECK_EQ(IntField(upload.response["last"]), 1);
	CHECK(Download(conn, login, login.uid, one_name, downloaded, server_error, error));
	CHECK(downloaded == one_chunk);

	// ---------- 边界：64KB + 1，第二片只有 1 字节 ----------
	const std::string two_chunk = MakePayload(kChunk + 1, 9);
	const std::string two_name = "ctest-two-" + RandomMd5().substr(0, 8) + ".bin";
	created.push_back(two_name);
	upload = Upload(conn, login, two_name, RandomMd5(), two_chunk, error);
	REQUIRE(upload.transport_ok);
	CHECK_EQ(IntField(upload.response["error"]), 0);
	CHECK_EQ(Int64Field(upload.response["trans_size"]),
		static_cast<Json::Int64>(kChunk + 1));
	CHECK(Download(conn, login, login.uid, two_name, downloaded, server_error, error));
	CHECK(downloaded.size() == kChunk + 1);
	CHECK(downloaded == two_chunk);

	// ---------- 边界：跳过中间一片 ----------
	//先建立进度（seq=1），再直接发 seq=3：客户端漏片时必须报错，
	//否则服务端会写出空洞，下载下来就是损坏文件
	const std::string gap_name = "ctest-gap-" + RandomMd5().substr(0, 8) + ".bin";
	const std::string gap_md5 = RandomMd5();
	const std::string gap_payload = MakePayload(kChunk * 3, 11);
	Json::Value first;
	first["md5"] = gap_md5;
	first["name"] = gap_name;
	first["seq"] = 1;
	first["trans_size"] = static_cast<Json::Int64>(kChunk);
	first["total_size"] = static_cast<Json::Int64>(gap_payload.size());
	first["last"] = 0;
	first["uid"] = login.uid;
	first["token"] = login.token;
	REQUIRE(conn.Send(ID_UPLOAD_FILE_REQ, first, gap_payload.substr(0, kChunk), error));
	Json::Value first_response;
	std::uint16_t response_id = 0;
	std::string no_payload;
	REQUIRE(conn.RecvFrame(response_id, first_response, no_payload, error));
	CHECK_EQ(IntField(first_response["error"]), 0);

	//上传中途查一次进度：服务器要回已经落盘的字节数，客户端据此决定从哪一片续传
	Json::Value sync_request;
	sync_request["uid"] = login.uid;
	sync_request["token"] = login.token;
	sync_request["md5"] = gap_md5;
	REQUIRE(conn.Send(ID_SYNC_FILE_REQ, sync_request, "", error));
	Json::Value sync_response;
	REQUIRE(conn.RecvFrame(response_id, sync_response, no_payload, error));
	CHECK_EQ(IntField(sync_response["error"]), 0);
	//seq=1 的回包是落盘之后才发的，所以这里读到的进度一定是 64KB
	CHECK_EQ(Int64Field(sync_response["trans_size"]), static_cast<Json::Int64>(kChunk));
	CHECK_EQ(IntField(sync_response["seq"]), 1);

	Json::Value jump = first;
	jump["seq"] = 3;
	jump["trans_size"] = static_cast<Json::Int64>(kChunk * 3);
	jump["last"] = 1;
	REQUIRE(conn.Send(ID_UPLOAD_FILE_REQ, jump, gap_payload.substr(kChunk * 2, kChunk), error));
	Json::Value jump_response;
	REQUIRE(conn.RecvFrame(response_id, jump_response, no_payload, error));
	CHECK_EQ(IntField(jump_response["error"]), ResourceError::FileSeqInvalid);

	// ---------- 边界：畸形帧不拖垮服务 ----------
	//声称 0xFFFFFFF0 字节的头长：早期写法在这里回绕后 substr 越界，
	//现在必须回错误包并且连接还能继续用
	std::string broken;
	broken.push_back(static_cast<char>(0xFF));
	broken.push_back(static_cast<char>(0xFF));
	broken.push_back(static_cast<char>(0xFF));
	broken.push_back(static_cast<char>(0xF0));
	broken += "not-a-header";
	REQUIRE(conn.SendRaw(ID_UPLOAD_FILE_REQ, broken, error));
	Json::Value broken_response;
	REQUIRE(conn.RecvFrame(response_id, broken_response, no_payload, error));
	CHECK(IntField(broken_response["error"]) != 0);

	//连接仍然可用：紧接着发一个正常请求，服务端要能继续处理
	CHECK(Download(conn, login, login.uid, name, downloaded, server_error, error));
	CHECK(downloaded == payload);

	// ---------- 边界：下载不存在的文件 ----------
	CHECK(!Download(conn, login, login.uid, "ctest-no-such-file.bin", downloaded,
		server_error, error));
	CHECK_EQ(server_error, ResourceError::FileNotExists);

	// ---------- 边界：下载序号越界 ----------
	Json::Value out_of_range;
	out_of_range["uid"] = login.uid;
	out_of_range["token"] = login.token;
	out_of_range["owner_uid"] = login.uid;
	out_of_range["name"] = name;
	out_of_range["seq"] = 99; //文件只有 3 片
	REQUIRE(conn.Send(ID_DOWN_LOAD_FILE_REQ, out_of_range, "", error));
	Json::Value range_response;
	std::string range_payload;
	REQUIRE(conn.RecvFrame(response_id, range_response, range_payload, error));
	CHECK(IntField(range_response["error"]) != 0);
	CHECK(range_payload.empty());
	//错误回包也要带回资源名，客户端才能知道是哪次下载失败
	CHECK_EQ(TextField(range_response["name"]), name);

	// ---------- 清理 ----------
	conn.Close();
	if (options.storage.empty()) {
		std::cout << "未提供 --storage，跳过测试文件清理\n";
	}
	else {
		const boost::filesystem::path root = boost::filesystem::path(options.storage)
			/ std::to_string(login.uid);
		boost::system::error_code ignored;
		for (const auto& item : created) {
			boost::filesystem::remove(root / item, ignored);
			boost::filesystem::remove(root / (item + UPLOAD_PART_SUFFIX), ignored);
		}
		//跳号用例只写了前两片，留下的是 .part
		boost::filesystem::remove(root / (gap_name + UPLOAD_PART_SUFFIX), ignored);
	}

	//失败时把最后一次传输/协议错误的原文带出来：断言只打印条件，看不到 error 的内容
	if (::yunjiantest::Failures() > 0 && !error.empty())
		std::cout << "最后一个错误: " << error << "\n";
	return YUNJIAN_TEST_SUMMARY();
}
