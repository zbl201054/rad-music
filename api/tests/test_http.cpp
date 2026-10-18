// HTTP 传输层测试（请求解析 / 响应构造 / 真实 socket 往返）
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "radmusic/cache.hpp"
#include "radmusic/crypto.hpp"
#include "radmusic/download.hpp"
#include "radmusic/http.hpp"
#include "radmusic/import.hpp"
#include "radmusic/storage.hpp"
#include "test_framework.hpp"

using namespace radmusic;

namespace {

std::filesystem::path fresh_root(const std::string& name) {
    const auto root = std::filesystem::path("build") / "tmp_http" / name;
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    return root;
}

std::string write_file(const std::filesystem::path& path, const std::string& content) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.close();
    return path.u8string();
}

// 从完整 HTTP 响应里取出 body（用于断言 JSON）
std::string response_body(const std::string& raw_response) {
    const size_t pos = raw_response.find("\r\n\r\n");
    if (pos == std::string::npos) return std::string();
    return raw_response.substr(pos + 4);
}

int response_status(const std::string& raw_response) {
    const size_t sp = raw_response.find(' ');
    if (sp == std::string::npos) return 0;
    return std::atoi(raw_response.c_str() + sp + 1);
}

Quality lossless_quality() {
    Quality q;
    q.level = QualityLevel::Lossless;
    q.codec = "flac";
    q.lossless = true;
    q.bitrate = 1411;
    q.sample_rate = 44100;
    q.bit_depth = 16;
    q.channels = 2;
    return q;
}

// 一套接线齐全的 API（含缓存 / 下载 / 导入模块）
struct HttpFixture {
    std::filesystem::path root;
    std::unique_ptr<LibraryPaths> paths;
    std::unique_ptr<PluginRegistry> plugins;
    std::unique_ptr<CoreEngine> engine;
    std::unique_ptr<MetaCache> meta;
    std::unique_ptr<LibraryIndex> index;
    std::unique_ptr<CacheManager> cache;
    std::unique_ptr<DownloadManager> downloads;
    std::unique_ptr<ImportManager> importer;
    std::unique_ptr<ApiDispatcher> api;

    explicit HttpFixture(const std::string& name) {
        root = fresh_root(name);
        paths = std::make_unique<LibraryPaths>(root);
        paths->ensure_layout();
        plugins = std::make_unique<PluginRegistry>();
        engine = std::make_unique<CoreEngine>(*paths, *plugins);
        meta = std::make_unique<MetaCache>(*paths);
        index = std::make_unique<LibraryIndex>(*paths);
        index->load();
        cache = std::make_unique<CacheManager>(*paths, *meta, *index);
        downloads = std::make_unique<DownloadManager>(*paths, *meta, *index, *plugins);
        importer = std::make_unique<ImportManager>(*paths, *meta, *index);
        api = std::make_unique<ApiDispatcher>(*paths, *plugins, *engine, *meta, *index);
        api->set_cache(cache.get());
        api->set_downloads(downloads.get());
        api->set_importer(importer.get());
    }

    // 客户端发送 → 服务端处理一次 → 客户端读取
    static std::string roundtrip(HttpServer& server, const std::string& raw_request, int io_timeout_ms = 200) {
        server.set_io_timeout_ms(io_timeout_ms);
        HttpClient client;
        const Status connected = client.connect_to("127.0.0.1", server.port(), 2000);
        if (!connected.ok()) return std::string();
        const Status sent = client.send(raw_request);
        if (!sent.ok()) return std::string();
        const bool handled = server.poll_once(2000);
        if (!handled) return std::string();
        auto response = client.read_response(3000);
        if (!response.ok()) return std::string();
        return response.value();
    }
};

}  // namespace

// ============================ 请求解析 ============================

TEST_CASE(test_http_parse_request_basic) {
    const std::string raw =
        "GET /music/status?sha=abc&verbose=1 HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "X-Trace: 42\r\n"
        "\r\n";
    HttpRequest request;
    size_t header_bytes = 0;
    int error_status = 0;
    std::string error;
    CHECK(parse_http_request(raw, &request, &header_bytes, &error_status, &error));
    CHECK_EQ(request.method, std::string("GET"));
    CHECK_EQ(request.path, std::string("/music/status"));
    CHECK_EQ(request.version, std::string("HTTP/1.1"));
    CHECK_EQ(header_bytes, raw.size());
    CHECK_EQ(request.header("host"), std::string("127.0.0.1"));
    CHECK_EQ(request.header("HOST"), std::string("127.0.0.1"));   // 大小写不敏感
    CHECK_EQ(request.header("x-trace"), std::string("42"));
    CHECK(request.has_header("X-TRACE"));
    CHECK_EQ(request.query.size(), size_t{2});
    CHECK_EQ(request.query_value("sha"), std::string("abc"));
    CHECK_EQ(request.query_value("verbose"), std::string("1"));
    CHECK_EQ(request.query_value("missing", "def"), std::string("def"));
    CHECK(request.keep_alive);   // HTTP/1.1 默认 keep-alive
}

TEST_CASE(test_http_parse_request_with_body) {
    const std::string body = "{\"sha\":\"deadbeef\"}";
    const std::string raw =
        "POST /music/load HTTP/1.1\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Connection: close\r\n"
        "\r\n" + body;
    HttpRequest request;
    size_t header_bytes = 0;
    int error_status = 0;
    std::string error;
    CHECK(parse_http_request(raw, &request, &header_bytes, &error_status, &error));
    CHECK_EQ(request.method, std::string("POST"));
    CHECK_EQ(request.content_length, body.size());
    CHECK_EQ(request.body, body);
    CHECK_EQ(header_bytes, raw.size() - body.size());
    CHECK_FALSE(request.keep_alive);
    CHECK_EQ(request.header("content-type"), std::string("application/json"));
}

TEST_CASE(test_http_parse_request_errors) {
    HttpRequest request;
    size_t header_bytes = 0;
    int status = 0;
    std::string error;

    // 缺少 CRLFCRLF
    CHECK_FALSE(parse_http_request("GET / HTTP/1.1\r\nHost: x\r\n", &request, &header_bytes, &status, &error));
    CHECK_EQ(status, 400);

    // 请求行非法
    CHECK_FALSE(parse_http_request("GARBAGE\r\n\r\n", &request, &header_bytes, &status, &error));
    CHECK_EQ(status, 400);

    // HTTP 版本非法
    CHECK_FALSE(parse_http_request("GET / FOO/1.1\r\n\r\n", &request, &header_bytes, &status, &error));
    CHECK_EQ(status, 400);

    // 头行没有冒号
    CHECK_FALSE(parse_http_request("GET / HTTP/1.1\r\nBadHeader\r\n\r\n", &request, &header_bytes, &status, &error));
    CHECK_EQ(status, 400);

    // chunked 不支持
    const std::string chunked =
        "POST /music/load HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n";
    CHECK_FALSE(parse_http_request(chunked, &request, &header_bytes, &status, &error));
    CHECK_EQ(status, 411);

    // Content-Length 非数字
    const std::string bad_length = "POST /x HTTP/1.1\r\nContent-Length: 12a\r\n\r\n";
    CHECK_FALSE(parse_http_request(bad_length, &request, &header_bytes, &status, &error));
    CHECK_EQ(status, 400);

    // body 长度不足
    const std::string short_body = "POST /x HTTP/1.1\r\nContent-Length: 10\r\n\r\nabc";
    CHECK_FALSE(parse_http_request(short_body, &request, &header_bytes, &status, &error));
    CHECK_EQ(status, 400);

    // HTTP/1.0 默认不保持连接
    const std::string http10 = "GET /music/status HTTP/1.0\r\n\r\n";
    CHECK(parse_http_request(http10, &request, &header_bytes, &status, &error));
    CHECK_FALSE(request.keep_alive);
}

TEST_CASE(test_http_url_decode_and_query) {
    CHECK_EQ(url_decode("a%20b"), std::string("a b"));
    CHECK_EQ(url_decode("a+b"), std::string("a b"));
    CHECK_EQ(url_decode("%E4%B8%AD%E6%96%87"), std::string("中文"));
    CHECK_EQ(url_decode("100%"), std::string("100%"));
    CHECK_EQ(url_decode("%zz"), std::string("%zz"));

    const auto query = parse_query("a=1&b=%20&c&d=");
    CHECK_EQ(query.size(), size_t{4});
    CHECK_EQ(query[0].first, std::string("a"));
    CHECK_EQ(query[0].second, std::string("1"));
    CHECK_EQ(query[1].second, std::string(" "));
    CHECK_EQ(query[2].first, std::string("c"));
    CHECK_EQ(query[2].second, std::string(""));
    CHECK_EQ(query[3].first, std::string("d"));
    CHECK_EQ(parse_query("").size(), size_t{0});
}

TEST_CASE(test_http_response_serialization) {
    json::Value body = json::Value::object();
    body.set("code", 0);
    body.set("message", "ok");
    HttpResponse response = HttpResponse::json(body);
    const std::string raw = response.to_string();
    CHECK(raw.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
    CHECK(raw.find("Content-Type: application/json; charset=utf-8\r\n") != std::string::npos);
    CHECK(raw.find("Content-Length: ") != std::string::npos);
    CHECK(raw.find("Connection: close\r\n") != std::string::npos);
    CHECK_EQ(response_body(raw), body.dump(-1));
    CHECK(raw.find("\r\n\r\n") != std::string::npos);

    HttpResponse not_found = HttpResponse::text("nope", 404);
    CHECK(not_found.to_string().rfind("HTTP/1.1 404 Not Found\r\n", 0) == 0);
    CHECK_EQ(std::string(http_reason_phrase(411)), std::string("Length Required"));

    // ApiResponse → HTTP：业务码放在 JSON 内，HTTP 状态保持 200
    const ApiResponse failure = ApiResponse::failure(ErrorCode::PlatformError, "no plugin");
    const std::string failure_raw = to_http_response(failure).to_string();
    CHECK(failure_raw.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
    const json::Value parsed = json::Value::parse(response_body(failure_raw));
    CHECK_EQ(parsed.get_int("code"), 1001);
    CHECK_EQ(parsed.get_string("message"), std::string("no plugin"));
}

// ============================ 真实 socket 往返 ============================

TEST_CASE(test_http_server_status_roundtrip) {
    HttpFixture fixture("status");
    HttpServer server("127.0.0.1", 0, *fixture.api);
    const Status started = server.start();
    CHECK(started.ok());
    CHECK(server.port() != 0);          // port=0 → 系统分配
    CHECK(server.running());
    CHECK_EQ(server.bound_address(), std::string("127.0.0.1:") + std::to_string(server.port()));

    const std::string raw_response = HttpFixture::roundtrip(
        server, "GET /music/status HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
    CHECK_FALSE(raw_response.empty());
    CHECK_EQ(response_status(raw_response), 200);
    const json::Value body = json::Value::parse(response_body(raw_response));
    CHECK(body.is_object());
    CHECK_EQ(body.get_int("code"), 0);
    CHECK_EQ(body.get_string("message"), std::string("ok"));
    CHECK_EQ(body.get("data").get_string("state"), std::string("stopped"));
    CHECK_EQ(server.served_requests(), size_t{1});
    CHECK_EQ(server.last_status(), static_cast<uint16_t>(200));

    // 再来一次（同一服务器，串行）
    const std::string again = HttpFixture::roundtrip(server, "GET /music/status HTTP/1.1\r\n\r\n");
    CHECK_EQ(response_status(again), 200);
    CHECK_EQ(server.served_requests(), size_t{2});
    server.stop();
}

TEST_CASE(test_http_server_post_body_and_query) {
    HttpFixture fixture("post");
    HttpServer server("127.0.0.1", 0, *fixture.api);
    CHECK(server.start().ok());

    // 造一个真实本地曲目并写入 base.json 缓存
    const auto audio = fixture.root / "tracks" / std::string(64, 'a') / "audio.flac";
    write_file(audio, "audio bytes for http test");
    bool ok = false;
    const std::string sha = crypto::sha256_file_hex(audio.u8string(), &ok);
    CHECK(ok);
    const auto real_audio = fixture.root / "tracks" / sha / "audio.flac";
    write_file(real_audio, "audio bytes for http test");

    BaseInfo base;
    base.platform = "local";
    base.version = "1.0.0";
    base.sha = sha;
    base.location = real_audio.u8string();
    base.name = "HTTP Song";
    base.author = "HTTP Artist";
    base.duration_seconds = 123;
    base.quality = lossless_quality();
    CHECK(fixture.meta->put(base).ok());

    // POST /music/load：body 是 JSON
    const std::string body = "{\"sha\":\"" + sha + "\"}";
    const std::string load_request = "POST /music/load HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
                                     "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    const std::string load_response = HttpFixture::roundtrip(server, load_request);
    CHECK_EQ(response_status(load_response), 200);
    const json::Value load_json = json::Value::parse(response_body(load_response));
    CHECK_EQ(load_json.get_int("code"), 0);
    CHECK_EQ(load_json.get("data").get("base").get_string("sha"), sha);

    // POST /music/start → playing
    const std::string start_response = HttpFixture::roundtrip(
        server, "POST /music/start HTTP/1.1\r\nContent-Length: 0\r\n\r\n");
    const json::Value start_json = json::Value::parse(response_body(start_response));
    CHECK_EQ(start_json.get_int("code"), 0);
    CHECK_EQ(start_json.get("data").get_string("state"), std::string("playing"));

    // GET 查询参数：/music/info?sha=...
    const std::string info_response =
        HttpFixture::roundtrip(server, "GET /music/info?sha=" + sha + " HTTP/1.1\r\n\r\n");
    const json::Value info_json = json::Value::parse(response_body(info_response));
    CHECK_EQ(info_json.get_int("code"), 0);
    CHECK_EQ(info_json.get("data").get("base").get_string("name"), std::string("HTTP Song"));
    CHECK_EQ(info_json.get("data").get_string("quality_key"), std::string("local.lossless-flac-1411"));

    // 未装配的模块此时已装配：缓存统计可用
    const std::string cache_response = HttpFixture::roundtrip(server, "GET /music/cache HTTP/1.1\r\n\r\n");
    const json::Value cache_json = json::Value::parse(response_body(cache_response));
    CHECK_EQ(cache_json.get_int("code"), 0);
    CHECK(cache_json.get("data").get("layers").is_object());

    // 未知路径：HTTP 200，业务码 1011
    const std::string unknown = HttpFixture::roundtrip(server, "GET /music/nope HTTP/1.1\r\n\r\n");
    CHECK_EQ(response_status(unknown), 200);
    CHECK_EQ(json::Value::parse(response_body(unknown)).get_int("code"), 1011);
    server.stop();
}

TEST_CASE(test_http_server_transport_errors) {
    HttpFixture fixture("errors");
    HttpServer server("127.0.0.1", 0, *fixture.api);
    server.set_io_timeout_ms(150);      // 让半开连接快速失败
    CHECK(server.start().ok());

    // 非法 JSON body → 400
    const std::string bad_json = "{not json";
    const std::string bad_request = "POST /music/load HTTP/1.1\r\nContent-Length: " +
                                    std::to_string(bad_json.size()) + "\r\n\r\n" + bad_json;
    const std::string bad_response = HttpFixture::roundtrip(server, bad_request, 150);
    CHECK_EQ(response_status(bad_response), 400);
    CHECK(json::Value::parse(response_body(bad_response)).get_int("code") != 0);

    // chunked → 411
    const std::string chunked = "POST /music/load HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n";
    const std::string chunked_response = HttpFixture::roundtrip(server, chunked, 150);
    CHECK_EQ(response_status(chunked_response), 411);

    // 半开连接（只有残缺请求头）→ 408，且不会把服务端拖死
    const std::string half_open = "GET /music/status HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    const std::string timeout_response = HttpFixture::roundtrip(server, half_open, 150);
    CHECK_EQ(response_status(timeout_response), 408);

    // 服务端仍然可用
    const std::string ok_response = HttpFixture::roundtrip(server, "GET /music/status HTTP/1.1\r\n\r\n", 150);
    CHECK_EQ(response_status(ok_response), 200);
    CHECK_EQ(json::Value::parse(response_body(ok_response)).get_int("code"), 0);
    server.stop();
}

TEST_CASE(test_http_server_download_and_import_routes) {
    HttpFixture fixture("routes");
    HttpServer server("127.0.0.1", 0, *fixture.api);
    CHECK(server.start().ok());

    // 通过 HTTP 创建下载任务（参数校验失败 → 1007）
    const std::string create_body = "{\"source\":\"\",\"item_id\":\"\"}";
    const std::string create_request = "POST /music/download HTTP/1.1\r\nContent-Length: " +
                                       std::to_string(create_body.size()) + "\r\n\r\n" + create_body;
    const json::Value create_json =
        json::Value::parse(response_body(HttpFixture::roundtrip(server, create_request)));
    CHECK_EQ(create_json.get_int("code"), 1007);

    // 空任务列表
    const json::Value list_json = json::Value::parse(
        response_body(HttpFixture::roundtrip(server, "GET /music/download HTTP/1.1\r\n\r\n")));
    CHECK_EQ(list_json.get_int("code"), 0);
    CHECK_EQ(list_json.get("data").get("tasks").size(), size_t{0});

    // 导入扫描：目录里只有不支持的扩展名 → 扫描成功但该项失败（1025）
    const auto scan_dir = fixture.root / "scan_src";
    write_file(scan_dir / "note.txt", "not audio");
    const std::string scan_path = scan_dir.generic_u8string();   // 正斜杠，避免 JSON 里出现 \t 之类的转义
    const std::string scan_body = "{\"paths\":[\"" + scan_path + "\"]}";
    const std::string scan_request = "POST /music/import/scan HTTP/1.1\r\nContent-Length: " +
                                     std::to_string(scan_body.size()) + "\r\n\r\n" + scan_body;
    const std::string scan_response = HttpFixture::roundtrip(server, scan_request);
    CHECK_EQ(response_status(scan_response), 200);
    const json::Value scan_json = json::Value::parse(response_body(scan_response));
    CHECK_EQ(scan_json.get_int("code"), 0);
    CHECK_EQ(scan_json.get("data").get_int("scanned"), 1);
    CHECK_EQ(scan_json.get("data").get_int("failed"), 1);
    CHECK_EQ(scan_json.get("data").get("items").size(), size_t{1});
    CHECK_EQ(scan_json.get("data").get("items")[0].get_int("code"), 1025);

    // 不存在的路径：扫描成功，但该路径作为"不可用候选"计入 failed（1004），不崩
    const std::string missing_body = "{\"paths\":[\"build/tmp_http/__not_exists__\"]}";
    const std::string missing_request = "POST /music/import/scan HTTP/1.1\r\nContent-Length: " +
                                        std::to_string(missing_body.size()) + "\r\n\r\n" + missing_body;
    const json::Value missing_json =
        json::Value::parse(response_body(HttpFixture::roundtrip(server, missing_request)));
    CHECK_EQ(missing_json.get_int("code"), 0);
    CHECK_EQ(missing_json.get("data").get_int("scanned"), 1);
    CHECK_EQ(missing_json.get("data").get_int("failed"), 1);
    const json::Value& missing_items = missing_json.get("data").get("items");
    CHECK_EQ(missing_items.size(), size_t{1});
    CHECK_EQ(missing_items[0].get_int("code"), 1004);

    // 未知导入任务 id → 1020
    const json::Value status_json = json::Value::parse(
        response_body(HttpFixture::roundtrip(server, "GET /music/import/imp_9999 HTTP/1.1\r\n\r\n")));
    CHECK_EQ(status_json.get_int("code"), 1020);
    server.stop();
}
