// Rad music API - HTTP 传输层（Readme 2.1：REST 形态；零第三方依赖）
//
// 设计要点：
//  * HTTP 状态码只表示传输层结果（200/400/404/411/500…）；
//    业务结果仍在 JSON 里：{ "code": 0|1001…, "message": …, "data": … }（Readme 2.2）。
//  * 只实现必要子集：GET/POST/DELETE、Content-Length 定长 body、每个连接只服务一次请求
//    （响应固定带 Connection: close）。不支持 chunked 传输（返回 411）；不做 TLS（应由反向代理终止）。
//  * 单线程 poll 模型：`poll_once()` 便于测试，`serve_forever()` 供独立进程使用。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "radmusic/api.hpp"
#include "radmusic/error.hpp"
#include "radmusic/json.hpp"

namespace radmusic {

struct HttpRequest {
    std::string method;
    std::string target;      // 原始 request-target（含查询串）
    std::string path;        // 去掉查询串的路径
    std::string version;     // HTTP/1.1
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<std::pair<std::string, std::string>> query;
    std::string body;
    size_t content_length = 0;
    bool keep_alive = false;

    std::string header(std::string_view name, std::string def = std::string()) const;
    bool has_header(std::string_view name) const;
    std::string query_value(std::string_view name, std::string def = std::string()) const;
};

std::string url_decode(std::string_view text);
std::vector<std::pair<std::string, std::string>> parse_query(std::string_view query_string);

// 解析请求头（到空行为止）。成功时 *header_bytes = 头部字节数（含结尾空行），
// 调用方据此继续读取 body；失败返回 false，并给出建议的 HTTP 状态码与原因。
bool parse_http_request(std::string_view raw, HttpRequest* out, size_t* header_bytes,
                        int* error_status, std::string* error);

struct HttpResponse {
    int status = 200;
    std::string reason = "OK";
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    void set_header(std::string name, std::string value);
    std::string to_string() const;

    static HttpResponse json(const json::Value& body, int status = 200);
    static HttpResponse text(std::string body, int status = 200);
};

const char* http_reason_phrase(int status);

// ApiResponse → HTTP 响应（业务 code 保持在 JSON 内）
HttpResponse to_http_response(const ApiResponse& response);

// 极简客户端：分步调用，测试里可以「客户端发送 → 服务端 poll_once → 客户端读取」，
// 不需要线程。`http_roundtrip()` 是一步到位的便捷版本（需服务端在别的线程/进程）。
class HttpClient {
public:
    HttpClient();
    ~HttpClient();
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    Status connect_to(const std::string& host, uint16_t port, int timeout_ms = 3000);
    Status send(std::string_view raw);
    Result<std::string> read_response(int timeout_ms = 5000);
    void close();
    bool connected() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

Result<std::string> http_roundtrip(const std::string& host, uint16_t port, std::string_view raw_request,
                                   int timeout_ms = 5000);

class HttpServer {
public:
    HttpServer(std::string host, uint16_t port, ApiDispatcher& dispatcher);
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // bind + listen；port 传 0 时由系统分配，可用 port() 取回实际端口
    Status start();
    void stop();
    bool running() const;
    uint16_t port() const;
    std::string bound_address() const;

    // 处理至多一个连接：返回 true 表示处理了连接（超时返回 false）
    bool poll_once(int timeout_ms = 1000);
    // 阻塞循环，直到 stop()
    void serve_forever(int poll_timeout_ms = 1000);
    // 空闲回调：serve_forever 每轮循环最多按 interval_ms 触发一次（用于驱动下载队列等后台任务）
    void set_idle_hook(std::function<void()> hook, int interval_ms = 300);

    // 单次 recv 的超时（防止对端半开连接拖住服务端）；默认 5000ms
    void set_io_timeout_ms(int timeout_ms);
    int io_timeout_ms() const;

    // 静态前端根目录（可选的网页 UI）：设置后可用浏览器直接访问 http://host:port/
    // 只暴露该目录下的白名单静态文件，不做子目录遍历。
    void set_static_root(std::string dir);
    std::string static_root() const;

    size_t served_requests() const;
    uint16_t last_status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace radmusic
