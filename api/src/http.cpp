#include "radmusic/http.hpp"
#include "radmusic/httpclient.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>

#include "radmusic/log.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

namespace radmusic {
namespace {

#ifdef _WIN32
struct WinsockGuard {
    WinsockGuard() {
        WSADATA data;
        ok = (WSAStartup(MAKEWORD(2, 2), &data) == 0);
    }
    ~WinsockGuard() {
        if (ok) WSACleanup();
    }
    bool ok = false;
};

bool ensure_winsock() {
    static WinsockGuard guard;
    return guard.ok;
}

void close_socket(socket_t s) {
    if (s != kInvalidSocket) closesocket(s);
}

int last_socket_error() { return WSAGetLastError(); }
#else
bool ensure_winsock() { return true; }
void close_socket(socket_t s) {
    if (s != kInvalidSocket) ::close(s);
}
int last_socket_error() { return errno; }
#endif

std::string lower_ascii(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text) out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}

std::string trim(std::string_view text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) ++begin;
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) --end;
    return std::string(text.substr(begin, end - begin));
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

// ---------------------------------------------------------------- 解析

std::string url_decode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '+') {
            out.push_back(' ');
            continue;
        }
        if (text[i] == '%' && i + 2 < text.size()) {
            const int hi = hex_value(text[i + 1]);
            const int lo = hex_value(text[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(text[i]);
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> parse_query(std::string_view query_string) {
    std::vector<std::pair<std::string, std::string>> out;
    size_t i = 0;
    while (i < query_string.size()) {
        const size_t amp = query_string.find('&', i);
        const std::string_view pair =
            query_string.substr(i, amp == std::string_view::npos ? std::string_view::npos : amp - i);
        if (!pair.empty()) {
            const size_t eq = pair.find('=');
            if (eq == std::string_view::npos) {
                out.emplace_back(url_decode(pair), std::string());
            } else {
                out.emplace_back(url_decode(pair.substr(0, eq)), url_decode(pair.substr(eq + 1)));
            }
        }
        if (amp == std::string_view::npos) break;
        i = amp + 1;
    }
    return out;
}

std::string HttpRequest::header(std::string_view name, std::string def) const {
    const std::string want = lower_ascii(name);
    for (const auto& kv : headers) {
        if (lower_ascii(kv.first) == want) return kv.second;
    }
    return def;
}

bool HttpRequest::has_header(std::string_view name) const {
    const std::string want = lower_ascii(name);
    for (const auto& kv : headers) {
        if (lower_ascii(kv.first) == want) return true;
    }
    return false;
}

std::string HttpRequest::query_value(std::string_view name, std::string def) const {
    for (const auto& kv : query) {
        if (kv.first == name) return kv.second;
    }
    return def;
}

bool parse_http_request(std::string_view raw, HttpRequest* out, size_t* header_bytes,
                        int* error_status, std::string* error) {
    auto fail = [&](int status, const std::string& message) {
        if (error_status) *error_status = status;
        if (error) *error = message;
        return false;
    };
    if (out == nullptr) return fail(500, "内部错误：out 为空");

    HttpRequest request;
    const size_t header_end = raw.find("\r\n\r\n");
    if (header_end == std::string_view::npos) return fail(400, "请求头不完整（缺少 CRLFCRLF）");

    const std::string_view head = raw.substr(0, header_end);
    size_t line_start = 0;
    bool first_line = true;

    while (line_start <= head.size()) {
        const size_t line_end = head.find("\r\n", line_start);
        const std::string_view line =
            head.substr(line_start, line_end == std::string_view::npos ? std::string_view::npos : line_end - line_start);

        if (first_line) {
            first_line = false;
            const size_t sp1 = line.find(' ');
            const size_t sp2 = (sp1 == std::string_view::npos) ? std::string_view::npos : line.find(' ', sp1 + 1);
            if (sp1 == std::string_view::npos || sp2 == std::string_view::npos) {
                return fail(400, "请求行非法");
            }
            request.method = std::string(line.substr(0, sp1));
            request.target = std::string(line.substr(sp1 + 1, sp2 - sp1 - 1));
            request.version = trim(line.substr(sp2 + 1));
            if (request.version.rfind("HTTP/", 0) != 0) return fail(400, "HTTP 版本非法");
            if (request.target.empty()) return fail(400, "request-target 为空");

            const size_t qmark = request.target.find('?');
            if (qmark == std::string::npos) {
                request.path = url_decode(request.target);
            } else {
                request.path = url_decode(request.target.substr(0, qmark));
                request.query = parse_query(std::string_view(request.target).substr(qmark + 1));
            }
            if (request.path.empty() || request.path[0] != '/') request.path = "/" + request.path;
        } else if (!line.empty()) {
            const size_t colon = line.find(':');
            if (colon == std::string_view::npos) return fail(400, "请求头行缺少冒号");
            std::string name = trim(line.substr(0, colon));
            if (name.empty()) return fail(400, "请求头名为空");
            request.headers.emplace_back(std::move(name), trim(line.substr(colon + 1)));
        }

        if (line_end == std::string_view::npos) break;
        line_start = line_end + 2;
    }

    if (request.method.empty()) return fail(400, "缺少请求方法");

    // Content-Length（不支持 chunked）
    if (request.has_header("transfer-encoding")) {
        const std::string te = lower_ascii(request.header("transfer-encoding"));
        if (te.find("chunked") != std::string::npos) {
            return fail(411, "不支持 chunked 传输编码，请使用 Content-Length");
        }
    }
    const std::string length_header = request.header("content-length");
    if (!length_header.empty()) {
        size_t value = 0;
        for (char c : length_header) {
            if (c < '0' || c > '9') return fail(400, "Content-Length 非法");
            value = value * 10 + static_cast<size_t>(c - '0');
        }
        request.content_length = value;
    }

    const std::string connection = lower_ascii(request.header("connection"));
    request.keep_alive = (request.version == "HTTP/1.1") ? (connection != "close") : (connection == "keep-alive");

    const bool has_body = request.content_length > 0;
    if (has_body) {
        const size_t body_start = header_end + 4;
        if (raw.size() < body_start + request.content_length) {
            return fail(400, "请求体不完整（Content-Length 与实际字节数不符）");
        }
        request.body = std::string(raw.substr(body_start, request.content_length));
    }

    if (header_bytes) *header_bytes = header_end + 4;
    if (error_status) *error_status = 200;
    if (error) error->clear();
    *out = std::move(request);
    return true;
}

// ---------------------------------------------------------------- 响应

const char* http_reason_phrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 206: return "Partial Content";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 411: return "Length Required";
        case 413: return "Payload Too Large";
        case 415: return "Unsupported Media Type";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        default: return "OK";
    }
}

void HttpResponse::set_header(std::string name, std::string value) {
    for (auto& kv : headers) {
        if (lower_ascii(kv.first) == lower_ascii(name)) {
            kv.second = std::move(value);
            return;
        }
    }
    headers.emplace_back(std::move(name), std::move(value));
}

std::string HttpResponse::to_string() const {
    std::string out;
    out.reserve(body.size() + 256);
    out += "HTTP/1.1 ";
    out += std::to_string(status);
    out += ' ';
    out += reason.empty() ? http_reason_phrase(status) : reason;
    out += "\r\n";

    bool has_content_type = false;
    bool has_content_length = false;
    bool has_connection = false;
    for (const auto& kv : headers) {
        const std::string name = lower_ascii(kv.first);
        if (name == "content-type") has_content_type = true;
        if (name == "content-length") has_content_length = true;
        if (name == "connection") has_connection = true;
        out += kv.first;
        out += ": ";
        out += kv.second;
        out += "\r\n";
    }
    // CORS：允许浏览器跨域调用（网页前端 + OPTIONS 预检）
    out += "Access-Control-Allow-Origin: *\r\n";
    out += "Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS\r\n";
    out += "Access-Control-Allow-Headers: Content-Type\r\n";
    if (!has_content_type) out += "Content-Type: application/json; charset=utf-8\r\n";
    if (!has_content_length) {
        out += "Content-Length: ";
        out += std::to_string(body.size());
        out += "\r\n";
    }
    if (!has_connection) out += "Connection: close\r\n";
    out += "\r\n";
    out += body;
    return out;
}

HttpResponse HttpResponse::json(const json::Value& body_value, int status_value) {
    HttpResponse response;
    response.status = status_value;
    response.reason = http_reason_phrase(status_value);
    response.set_header("Content-Type", "application/json; charset=utf-8");
    response.body = body_value.dump(-1);
    return response;
}

HttpResponse HttpResponse::text(std::string body_value, int status_value) {
    HttpResponse response;
    response.status = status_value;
    response.reason = http_reason_phrase(status_value);
    response.set_header("Content-Type", "text/plain; charset=utf-8");
    response.body = std::move(body_value);
    return response;
}

HttpResponse to_http_response(const ApiResponse& response) {
    return HttpResponse::json(response.to_json(), 200);
}

namespace {

// 依据扩展名推断音频 MIME
std::string audio_mime_for(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    const std::string ext = (dot == std::string::npos) ? std::string() : lower_ascii(path.substr(dot));
    if (ext == ".mp3") return "audio/mpeg";
    if (ext == ".flac") return "audio/flac";
    if (ext == ".wav") return "audio/wav";
    if (ext == ".m4a") return "audio/mp4";
    if (ext == ".aac") return "audio/aac";
    if (ext == ".ogg" || ext == ".opus") return "audio/ogg";
    if (ext == ".ape") return "audio/x-ape";
    if (ext == ".wma") return "audio/x-ms-wma";
    if (ext == ".aif" || ext == ".aiff") return "audio/aiff";
    return "application/octet-stream";
}

// 发送本地音频文件；支持 Range（<audio> 拖动进度需要 206 Partial Content）
HttpResponse make_audio_response(const std::string& file_path, const std::string& range_header) {
    // file_path 为 UTF-8 字节：转成 path 后所有文件操作都走 Windows 宽字符 API，
    // 否则中文等非 ASCII 文件名会因窄编码 fopen 失败而返回 404。
    const std::filesystem::path file = std::filesystem::path(file_path);
    std::error_code ec;
    const auto total = std::filesystem::file_size(file, ec);
    if (ec) {
        return HttpResponse::json(
            ApiResponse::failure(ErrorCode::FileNotFound, "音频文件不可读").to_json(), 404);
    }

    size_t start = 0;
    size_t end = (total == 0) ? 0 : static_cast<size_t>(total) - 1;
    bool partial = false;

    if (!range_header.empty() && range_header.rfind("bytes=", 0) == 0 && total > 0) {
        std::string spec = range_header.substr(6);
        if (const size_t comma = spec.find(','); comma != std::string::npos) spec = spec.substr(0, comma);
        const size_t dash = spec.find('-');
        if (dash != std::string::npos) {
            const std::string first = spec.substr(0, dash);
            const std::string last = spec.substr(dash + 1);
            const auto digits = [](const std::string& s, size_t* out) {
                if (s.empty()) return false;
                size_t value = 0;
                for (char c : s) {
                    if (c < '0' || c > '9') return false;
                    value = value * 10 + static_cast<size_t>(c - '0');
                }
                *out = value;
                return true;
            };
            size_t s = 0;
            size_t e = static_cast<size_t>(total) - 1;
            if (first.empty()) {
                size_t n = 0;
                if (digits(last, &n) && n > 0) {
                    start = (n >= static_cast<size_t>(total)) ? 0 : static_cast<size_t>(total) - n;
                    end = static_cast<size_t>(total) - 1;
                    partial = true;
                }
            } else if (digits(first, &s) && s < static_cast<size_t>(total)) {
                if (last.empty() || !digits(last, &e)) e = static_cast<size_t>(total) - 1;
                if (e >= static_cast<size_t>(total)) e = static_cast<size_t>(total) - 1;
                if (s <= e) {
                    start = s;
                    end = e;
                    partial = true;
                }
            }
        }
    }

    const size_t length = (total == 0) ? 0 : (end - start + 1);
    HttpResponse response;
    response.status = partial ? 206 : 200;
    response.reason = http_reason_phrase(response.status);
    response.set_header("Content-Type", audio_mime_for(file_path));
    response.set_header("Accept-Ranges", "bytes");
    if (partial) {
        response.set_header("Content-Range", "bytes " + std::to_string(start) + "-" +
                                                  std::to_string(end) + "/" + std::to_string(total));
    }

    if (length > 0) {
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            return HttpResponse::json(
                ApiResponse::failure(ErrorCode::FileNotFound, "音频文件不可读").to_json(), 404);
        }
        in.seekg(static_cast<std::streamoff>(start), std::ios::beg);
        response.body.resize(length);
        in.read(&response.body[0], static_cast<std::streamsize>(length));
        response.body.resize(static_cast<size_t>(in.gcount()));
    }
    return response;
}

// 发送本地封面文件（covers/<hash>.<ext>）；整文件发送，带短缓存
HttpResponse make_cover_response(const std::string& file_path) {
    const std::filesystem::path file = std::filesystem::path(file_path);
    std::error_code ec;
    const auto total = std::filesystem::file_size(file, ec);
    if (ec) {
        return HttpResponse::json(
            ApiResponse::failure(ErrorCode::FileNotFound, "封面文件不可读").to_json(), 404);
    }
    HttpResponse response;
    response.status = 200;
    response.reason = http_reason_phrase(200);
    const size_t dot = file_path.find_last_of('.');
    const std::string ext =
        (dot == std::string::npos) ? std::string() : lower_ascii(file_path.substr(dot));
    if (ext == ".png") response.set_header("Content-Type", "image/png");
    else if (ext == ".webp") response.set_header("Content-Type", "image/webp");
    else if (ext == ".gif") response.set_header("Content-Type", "image/gif");
    else if (ext == ".bmp") response.set_header("Content-Type", "image/bmp");
    else response.set_header("Content-Type", "image/jpeg");
    response.set_header("Cache-Control", "public, max-age=86400");
    response.body.resize(static_cast<size_t>(total));
    if (total > 0) {
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            return HttpResponse::json(
                ApiResponse::failure(ErrorCode::FileNotFound, "封面文件不可读").to_json(), 404);
        }
        in.read(&response.body[0], static_cast<std::streamsize>(total));
        response.body.resize(static_cast<size_t>(in.gcount()));
    }
    return response;
}

// 发送导出产物（exports/<name>）作为附件下载；文件名用 RFC 5987 的 filename* 支持中文
HttpResponse make_export_download_response(const std::string& file_path) {
    const std::filesystem::path file = std::filesystem::path(file_path);
    std::error_code ec;
    const auto total = std::filesystem::file_size(file, ec);
    if (ec) {
        return HttpResponse::json(
            ApiResponse::failure(ErrorCode::FileNotFound, "导出文件不可读").to_json(), 404);
    }
    const std::string name = file.filename().u8string();
    std::string ascii;
    std::string encoded;
    static const char* kHex = "0123456789ABCDEF";
    for (unsigned char c : name) {
        if (c < 0x80 && c != '"' && c != '\\' && c >= 0x20) {
            ascii.push_back(static_cast<char>(c));
            encoded.push_back(static_cast<char>(c));
        } else {
            ascii.push_back('_');
            encoded.push_back('%');
            encoded.push_back(kHex[(c >> 4) & 0xF]);
            encoded.push_back(kHex[c & 0xF]);
        }
    }
    HttpResponse response;
    response.status = 200;
    response.reason = http_reason_phrase(200);
    const size_t dot = name.find_last_of('.');
    const std::string ext = (dot == std::string::npos) ? std::string() : lower_ascii(name.substr(dot));
    response.set_header("Content-Type", ext == ".rdl" ? "application/json; charset=utf-8"
                                                      : "application/octet-stream");
    response.set_header("Content-Disposition",
                        "attachment; filename=\"" + ascii + "\"; filename*=UTF-8''" + encoded);
    response.body.resize(static_cast<size_t>(total));
    if (total > 0) {
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            return HttpResponse::json(
                ApiResponse::failure(ErrorCode::FileNotFound, "导出文件不可读").to_json(), 404);
        }
        in.read(&response.body[0], static_cast<std::streamsize>(total));
        response.body.resize(static_cast<size_t>(in.gcount()));
    }
    return response;
}

// 静态页面白名单：只放行这些前端资源扩展名
bool static_extension_allowed(const std::string& ext) {
    static const char* kAllowed[] = {".html", ".css",  ".js",   ".mjs", ".svg", ".png",
                                     ".jpg",  ".jpeg", ".webp", ".ico", ".json", ".map",
                                     ".webmanifest", ".woff", ".woff2", ".ttf"};
    for (const char* item : kAllowed) {
        if (ext == item) return true;
    }
    return false;
}

std::string static_mime_for(const std::string& ext) {
    if (ext == ".html") return "text/html; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".js" || ext == ".mjs") return "text/javascript; charset=utf-8";
    if (ext == ".json" || ext == ".map" || ext == ".webmanifest")
        return "application/json; charset=utf-8";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".webp") return "image/webp";
    if (ext == ".ico") return "image/x-icon";
    if (ext == ".woff") return "font/woff";
    if (ext == ".woff2") return "font/woff2";
    if (ext == ".ttf") return "font/ttf";
    return "application/octet-stream";
}

// 请求体（含上传）的硬上限；超过则直接拒绝，避免把超大 body 读进内存
const size_t kMaxRequestBodyBytes = 512u * 1024u * 1024u;

// 从已读到的头部块里取 Content-Length（未声明或非数字返回 0）。
// 用于在正式解析之前就知道还需要收多少字节的 body。
size_t declared_content_length(const std::string& raw, size_t header_end) {
    const std::string lower = lower_ascii(raw.substr(0, header_end));
    const size_t pos = lower.find("content-length:");
    if (pos == std::string::npos) return 0;
    size_t i = pos + std::strlen("content-length:");
    while (i < lower.size() && (lower[i] == ' ' || lower[i] == '\t')) ++i;
    size_t value = 0;
    while (i < lower.size() && lower[i] >= '0' && lower[i] <= '9') {
        value = value * 10 + static_cast<size_t>(lower[i] - '0');
        ++i;
    }
    return value;
}

// 尝试把请求当作网页 UI 静态资源返回。
// 只服务根目录一级的文件（不下钻子目录），因此 /music/... 会安全地落到 API 路由；
// 未命中或不允许的路径返回 false，由调用方继续按 API 处理。
bool try_static_response(const std::string& root, const std::string& raw_path, HttpResponse* out) {
    if (root.empty() || out == nullptr) return false;

    std::string path = url_decode(raw_path);
    if (path.empty() || path[0] != '/') return false;
    if (path.find('\\') != std::string::npos) return false;
    if (path.size() > 1 && path.find('/', 1) != std::string::npos) return false;  // 仅一级
    if (path.size() > 1 && path[1] == '.') return false;                          // 隐藏文件
    if (path == "/") path = "/index.html";

    const size_t dot = path.find_last_of('.');
    const std::string ext =
        (dot == std::string::npos) ? std::string() : lower_ascii(path.substr(dot));
    if (!static_extension_allowed(ext)) return false;

    const std::filesystem::path file = std::filesystem::path(root) / path.substr(1);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec) || ec) {
        // 缺 favicon 时静默返回 204，避免浏览器控制台反复报错
        if (path == "/favicon.ico") {
            *out = HttpResponse();
            out->status = 204;
            out->reason = http_reason_phrase(204);
            return true;
        }
        return false;
    }

    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    *out = HttpResponse::text(std::move(body), 200);
    out->set_header("Content-Type", static_mime_for(ext));
    out->set_header("Cache-Control", "no-cache");
    return true;
}

}  // namespace

// ---------------------------------------------------------------- 客户端

struct HttpClient::Impl {
    socket_t socket = kInvalidSocket;
};

HttpClient::HttpClient() : impl_(new Impl()) { ensure_winsock(); }

HttpClient::~HttpClient() { close(); }

Status HttpClient::connect_to(const std::string& host, uint16_t port, int timeout_ms) {
    if (!ensure_winsock()) return Status(ErrorCode::RequestFailed, "Winsock 初始化失败");
    close();

    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* result = nullptr;
    const std::string port_text = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_text.c_str(), &hints, &result) != 0 || result == nullptr) {
        return Status(ErrorCode::RequestFailed, "无法解析主机: " + host);
    }

    Status status = Status(ErrorCode::RequestFailed, "无法连接: " + host + ":" + port_text);
    for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
        const socket_t s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == kInvalidSocket) continue;
        if (::connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) {
            impl_->socket = s;
            status = Status::success();
            break;
        }
        close_socket(s);
    }
    freeaddrinfo(result);

    if (!status.ok()) return status;
    (void)timeout_ms;   // 阻塞式连接；超时由调用方的 select 兜底
    return Status::success();
}

Status HttpClient::send(std::string_view raw) {
    if (!connected()) return Status(ErrorCode::RequestFailed, "未连接");
    size_t sent = 0;
    while (sent < raw.size()) {
        const int n = ::send(impl_->socket, raw.data() + sent, static_cast<int>(raw.size() - sent), 0);
        if (n <= 0) return Status(ErrorCode::RequestFailed, "发送失败（错误码 " + std::to_string(last_socket_error()) + "）");
        sent += static_cast<size_t>(n);
    }
    return Status::success();
}

Result<std::string> HttpClient::read_response(int timeout_ms) {
    if (!connected()) return Result<std::string>::failure(ErrorCode::RequestFailed, "未连接");

    std::string raw;
    char buffer[4096];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    size_t expected_total = 0;

    for (;;) {
        // 头部读完则算出总长度（Content-Length）
        if (expected_total == 0) {
            const size_t header_end = raw.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                size_t content_length = 0;
                const std::string lower = lower_ascii(raw.substr(0, header_end));
                const size_t pos = lower.find("content-length:");
                if (pos != std::string::npos) {
                    size_t i = pos + 15;
                    while (i < lower.size() && (lower[i] == ' ' || lower[i] == '\t')) ++i;
                    while (i < lower.size() && lower[i] >= '0' && lower[i] <= '9') {
                        content_length = content_length * 10 + static_cast<size_t>(lower[i] - '0');
                        ++i;
                    }
                }
                expected_total = header_end + 4 + content_length;
            }
        }
        if (expected_total > 0 && raw.size() >= expected_total) {
            return Result<std::string>::success(raw.substr(0, expected_total));
        }

        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(impl_->socket, &read_set);
        timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100 * 1000;
        const int ready = ::select(static_cast<int>(impl_->socket) + 1, &read_set, nullptr, nullptr, &tv);
        if (ready > 0) {
            const int n = ::recv(impl_->socket, buffer, sizeof(buffer), 0);
            if (n == 0) {
                if (!raw.empty()) return Result<std::string>::success(raw);
                return Result<std::string>::failure(ErrorCode::RequestFailed, "连接被关闭且没有响应");
            }
            if (n < 0) return Result<std::string>::failure(ErrorCode::RequestFailed, "读取失败");
            raw.append(buffer, static_cast<size_t>(n));
            continue;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            if (!raw.empty()) return Result<std::string>::success(raw);
            return Result<std::string>::failure(ErrorCode::RequestFailed, "读取响应超时");
        }
    }
}

void HttpClient::close() {
    if (impl_ && impl_->socket != kInvalidSocket) {
        close_socket(impl_->socket);
        impl_->socket = kInvalidSocket;
    }
}

bool HttpClient::connected() const { return impl_ && impl_->socket != kInvalidSocket; }

Result<std::string> http_roundtrip(const std::string& host, uint16_t port, std::string_view raw_request,
                                   int timeout_ms) {
    HttpClient client;
    Status status = client.connect_to(host, port, timeout_ms);
    if (!status.ok()) return Result<std::string>(status);
    status = client.send(raw_request);
    if (!status.ok()) return Result<std::string>(status);
    return client.read_response(timeout_ms);
}

// ---------------------------------------------------------------- 服务端

struct HttpServer::Impl {
    std::string host;
    uint16_t port = 0;
    uint16_t bound_port = 0;
    ApiDispatcher* dispatcher = nullptr;
    socket_t listen_socket = kInvalidSocket;
    std::atomic<bool> stopped{false};
    size_t served = 0;
    uint16_t last_status = 0;
    int io_timeout_ms = 5000;
    std::string static_root;    // 非空时提供网页 UI（仅白名单静态文件）
    std::function<void()> idle_hook;                       // 后台任务驱动（如下载队列）
    int idle_interval_ms = 300;
    std::chrono::steady_clock::time_point idle_last{};
};

// 带超时的 recv：>0 收到字节；0 对端关闭；-1 超时；-2 错误
int recv_with_timeout(socket_t socket, char* buffer, size_t size, int timeout_ms) {
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(socket, &read_set);
    timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    const int ready = ::select(static_cast<int>(socket) + 1, &read_set, nullptr, nullptr, &tv);
    if (ready == 0) return -1;
    if (ready < 0) return -2;
    const int n = ::recv(socket, buffer, static_cast<int>(size), 0);
    if (n == 0) return 0;
    if (n < 0) return -2;
    return n;
}

HttpServer::HttpServer(std::string host, uint16_t port, ApiDispatcher& dispatcher) : impl_(new Impl()) {
    impl_->host = std::move(host);
    impl_->port = port;
    impl_->dispatcher = &dispatcher;
    ensure_winsock();
}

HttpServer::~HttpServer() {
    stop();
    if (impl_ && impl_->listen_socket != kInvalidSocket) {
        close_socket(impl_->listen_socket);
        impl_->listen_socket = kInvalidSocket;
    }
}

Status HttpServer::start() {
    if (!ensure_winsock()) return Status(ErrorCode::RequestFailed, "Winsock 初始化失败");
    if (impl_->listen_socket != kInvalidSocket) return Status::success();

    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    addrinfo* result = nullptr;
    const std::string port_text = std::to_string(impl_->port);
    const char* node = impl_->host.empty() || impl_->host == "0.0.0.0" ? nullptr : impl_->host.c_str();
    if (getaddrinfo(node, port_text.c_str(), &hints, &result) != 0 || result == nullptr) {
        return Status(ErrorCode::RequestFailed, "无法解析监听地址: " + impl_->host);
    }

    socket_t s = kInvalidSocket;
#ifdef _WIN32
    const int kAddrInUse = WSAEADDRINUSE;
#else
    const int kAddrInUse = EADDRINUSE;
#endif
    bool addr_in_use = false;
    auto attempt_bind = [&]() -> bool {
        for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
            s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (s == kInvalidSocket) continue;
            int yes = 1;
            setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
            if (::bind(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0 && ::listen(s, 16) == 0) {
                return true;
            }
            addr_in_use = (last_socket_error() == kAddrInUse);
            close_socket(s);
            s = kInvalidSocket;
        }
        return false;
    };

    bool bound = attempt_bind();
    // 自重启（含迁移后切换数据根）时，旧进程可能尚未释放端口：仅当错误为「地址占用」时
    // 有限重试（最多 ~5s），避免切换瞬间新进程绑定失败、导致无人监听。
    for (int retry = 0; !bound && addr_in_use && retry < 25; ++retry) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        bound = attempt_bind();
    }
    freeaddrinfo(result);

    if (!bound) {
        return Status(ErrorCode::RequestFailed,
                      "监听失败: " + impl_->host + ":" + port_text + "（错误码 " + std::to_string(last_socket_error()) + "）");
    }

    // 取回实际端口（port=0 时由系统分配）
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
#ifdef _WIN32
    int addr_len = sizeof(addr);
#else
    socklen_t addr_len = sizeof(addr);
#endif
    if (getsockname(s, reinterpret_cast<sockaddr*>(&addr), &addr_len) == 0) {
        impl_->bound_port = ntohs(addr.sin_port);
    } else {
        impl_->bound_port = impl_->port;
    }

    impl_->listen_socket = s;
    impl_->stopped = false;
    return Status::success();
}

void HttpServer::stop() {
    if (impl_) impl_->stopped = true;
}

bool HttpServer::running() const {
    return impl_ && impl_->listen_socket != kInvalidSocket && !impl_->stopped.load();
}

uint16_t HttpServer::port() const { return impl_ ? impl_->bound_port : 0; }

std::string HttpServer::bound_address() const {
    if (!impl_) return std::string();
    return impl_->host + ":" + std::to_string(impl_->bound_port);
}

size_t HttpServer::served_requests() const { return impl_ ? impl_->served : 0; }

uint16_t HttpServer::last_status() const { return impl_ ? impl_->last_status : 0; }

void HttpServer::set_io_timeout_ms(int timeout_ms) {
    if (impl_) impl_->io_timeout_ms = timeout_ms <= 0 ? 5000 : timeout_ms;
}

int HttpServer::io_timeout_ms() const { return impl_ ? impl_->io_timeout_ms : 5000; }

void HttpServer::set_static_root(std::string dir) {
    if (impl_) impl_->static_root = std::move(dir);
}

std::string HttpServer::static_root() const { return impl_ ? impl_->static_root : std::string(); }

bool HttpServer::poll_once(int timeout_ms) {
    if (!impl_ || impl_->listen_socket == kInvalidSocket) return false;

    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(impl_->listen_socket, &read_set);
    timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    const int ready = ::select(static_cast<int>(impl_->listen_socket) + 1, &read_set, nullptr, nullptr, &tv);
    if (ready <= 0) return false;

    const socket_t client = ::accept(impl_->listen_socket, nullptr, nullptr);
    if (client == kInvalidSocket) return false;

    auto send_response = [&](const HttpResponse& response) {
        const std::string raw = response.to_string();
        size_t sent = 0;
        while (sent < raw.size()) {
            const int n = ::send(client, raw.data() + sent, static_cast<int>(raw.size() - sent), 0);
            if (n <= 0) break;
            sent += static_cast<size_t>(n);
        }
        impl_->last_status = static_cast<uint16_t>(response.status);
    };

    auto read_more = [&](std::string& buffer) -> int {
        char chunk[64 * 1024];  // 一次尽量多收：上传音频时减少 recv 次数
        const int n = recv_with_timeout(client, chunk, sizeof(chunk), impl_->io_timeout_ms);
        if (n > 0) buffer.append(chunk, static_cast<size_t>(n));
        return n;
    };

    // 1) 读请求头，并按 Content-Length 把 body 一起读满（限制 64 KiB 头；超时视为请求不完整）。
    //    必须读满再解析：body 会跨多次 recv 到达（上传文件时必然如此），
    //    否则 parse_http_request 会以「请求体不完整」为由拒掉整个请求。
    std::string buffer;
    bool header_timeout = false;
    size_t expected_total = 0;
    for (;;) {
        const size_t header_end = buffer.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            // 头还没收完：只对头部本身设上限（body 会远大于它）
            if (buffer.size() > 64 * 1024) {
                send_response(HttpResponse::text("请求头过大", 413));
                close_socket(client);
                ++impl_->served;
                return true;
            }
        } else {
            if (header_end + 4 > 64 * 1024) {
                send_response(HttpResponse::text("请求头过大", 413));
                close_socket(client);
                ++impl_->served;
                return true;
            }
            if (expected_total == 0) {
                const size_t body_length = declared_content_length(buffer, header_end);
                if (body_length > kMaxRequestBodyBytes) {
                    send_response(HttpResponse::text("请求体过大", 413));
                    close_socket(client);
                    ++impl_->served;
                    return true;
                }
                expected_total = header_end + 4 + body_length;
            }
            if (buffer.size() >= expected_total) break;
        }
        const int n = read_more(buffer);
        if (n < 0) {
            header_timeout = true;
            break;
        }
        if (n == 0) break;
    }

    if (header_timeout) {
        send_response(HttpResponse::text("请求头读取超时或不完整", 408));
    } else if (buffer.find("\r\n\r\n") != std::string::npos) {
        HttpRequest request;
        size_t header_bytes = 0;
        int error_status = 400;
        std::string error;
        if (!parse_http_request(buffer, &request, &header_bytes, &error_status, &error)) {
            send_response(HttpResponse::json(ApiResponse::failure(ErrorCode::RequestFailed, error).to_json(),
                                             error_status));
        } else {
            // OPTIONS 预检：浏览器 CORS 预检请求，直接返回空响应（含 CORS 头）
            if (request.method == "OPTIONS") {
                HttpResponse preflight;
                preflight.status = 204;
                preflight.reason = "No Content";
                send_response(preflight);
                close_socket(client);
                ++impl_->served;
                return true;
            }
            // 静态网页 UI（设置 static_root 后生效）：/ → index.html、/app.js、/styles.css…
            if (request.method == "GET") {
                HttpResponse static_response;
                if (try_static_response(impl_->static_root, request.path, &static_response)) {
                    send_response(static_response);
                    close_socket(client);
                    ++impl_->served;
                    return true;
                }
            }
            // GET /music/audio/{sha}：发送本地音频文件（支持 Range，供 <audio> 播放/拖动进度）
            if (request.method == "GET" && request.path.rfind("/music/audio/", 0) == 0) {
                const std::string sha = request.path.substr(std::strlen("/music/audio/"));
                auto resolved = impl_->dispatcher->audio_path_for(sha);
                if (!resolved.ok()) {
                    send_response(HttpResponse::json(ApiResponse::failure(resolved.status().code,
                                                                          resolved.status().message)
                                                         .to_json(),
                                                     404));
                } else {
                    send_response(make_audio_response(resolved.value(), request.header("range")));
                }
                close_socket(client);
                ++impl_->served;
                return true;
            }
            // GET /music/share/download?name=<file>：导出产物（.rdm 等）以附件下载
            if (request.method == "GET" && request.path == "/music/share/download") {
                const std::string name = request.query_value("name");
                auto resolved = impl_->dispatcher->export_file_for(name);
                if (!resolved.ok()) {
                    send_response(HttpResponse::json(ApiResponse::failure(resolved.status().code,
                                                                          resolved.status().message)
                                                         .to_json(),
                                                     404));
                } else {
                    send_response(make_export_download_response(resolved.value()));
                }
                close_socket(client);
                ++impl_->served;
                return true;
            }
            // GET /music/stream?u=<encoded remote url>：跨域音频流同源代理。把跨域 CDN 地址经本机
            // 服务端中转成同源地址，使 Web Audio 能安全接管音频图（EQ 可用），并透传 Range 实现 seek。
            if (request.method == "GET" && request.path == "/music/stream") {
                auto refused = [&](const std::string& msg) {
                    send_response(HttpResponse::json(
                        ApiResponse::failure(ErrorCode::InvalidMetadata, msg).to_json(), 400));
                    close_socket(client);
                    ++impl_->served;
                    return true;
                };
                const std::string remote = request.query_value("u");
                if (remote.empty()) return refused("缺少远程音频地址参数 u");
                if (!is_remote_url(remote)) return refused("仅支持 http(s) 远程地址");
                const std::string rhost = [&]() {
                    size_t s = remote.find("://");
                    if (s == std::string::npos) return std::string();
                    s += 3;
                    size_t e = remote.find('/', s);
                    if (e == std::string::npos) e = remote.size();
                    std::string h = remote.substr(s, e - s);
                    const size_t c = h.rfind(':');
                    if (c != std::string::npos) h = h.substr(0, c);
                    return h;
                }();
                auto blocked = [](const std::string& host) {
                    if (host.empty()) return true;
                    const std::string h = lower_ascii(host);
                    if (h == "localhost" || h == "::1" || h == "[::1]" || h.rfind("127.", 0) == 0 ||
                        h.rfind("10.", 0) == 0 || h.rfind("192.168.", 0) == 0 ||
                        h.rfind("169.254.", 0) == 0 || h.rfind("0.", 0) == 0) {
                        return true;
                    }
                    if (h.rfind("172.", 0) == 0) {
                        const std::string sub = h.substr(4);
                        const size_t d = sub.find('.');
                        if (d != std::string::npos) {
                            bool ok = true;
                            int v = 0;
                            for (char c : sub.substr(0, d)) {
                                if (c < '0' || c > '9') { ok = false; break; }
                                v = v * 10 + (c - '0');
                            }
                            if (ok && v >= 16 && v <= 31) return true;
                        }
                    }
                    return false;
                };
                if (blocked(rhost)) return refused("不允许代理到本地/内网地址");
                int upstream = 0;
                std::vector<std::pair<std::string, std::string>> hdrs;
                std::string body;
                std::string err;
                const std::vector<std::pair<std::string, std::string>> req_hdrs = {
                    {"Range", request.header("range")}};
                if (http_proxy_stream(remote, req_hdrs, &upstream, &hdrs, &body, &err)) {
                    HttpResponse resp;
                    resp.status = upstream > 0 ? upstream : 200;
                    resp.reason = http_reason_phrase(resp.status);
                    for (auto& h : hdrs) resp.set_header(h.first, h.second);
                    resp.set_header("Accept-Ranges", "bytes");
                    resp.body = std::move(body);
                    send_response(resp);
                } else {
                    send_response(HttpResponse::json(
                        ApiResponse::failure(ErrorCode::RequestFailed, "音频代理失败: " + err).to_json(), 502));
                }
                close_socket(client);
                ++impl_->served;
                return true;
            }

            // GET /music/cover/{covers/<hash>.<ext>}：发送本地封面（曲库条目的海报）
            if (request.method == "GET" && request.path.rfind("/music/cover/", 0) == 0) {
                const std::string name = request.path.substr(std::strlen("/music/cover/"));
                auto resolved = impl_->dispatcher->cover_path_for(name);
                if (!resolved.ok()) {
                    send_response(HttpResponse::json(ApiResponse::failure(resolved.status().code,
                                                                          resolved.status().message)
                                                         .to_json(),
                                                     404));
                } else {
                    send_response(make_cover_response(resolved.value()));
                }
                close_socket(client);
                ++impl_->served;
                return true;
            }
            // GET /music/plugins/{id}/panel：发送功能插件的面板 HTML（ui.panel），宿主以 text/html 提供
            if (request.method == "GET" && request.path.rfind("/music/plugins/", 0) == 0) {
                const std::string tail = request.path.substr(std::strlen("/music/plugins/"));  // "<id>/panel"
                const std::string marker = "/panel";
                if (tail.size() > marker.size() &&
                    tail.compare(tail.size() - marker.size(), marker.size(), marker) == 0) {
                    const std::string id = tail.substr(0, tail.size() - marker.size());
                    auto content = impl_->dispatcher->plugin_panel_content(id);
                    if (content.ok()) {
                        HttpResponse panel_resp;
                        panel_resp.status = 200;
                        panel_resp.reason = "OK";
                        panel_resp.body = content.value();
                        panel_resp.set_header("Content-Type", "text/html; charset=utf-8");
                        panel_resp.set_header("Access-Control-Allow-Origin", "*");
                        send_response(panel_resp);
                    } else {
                        send_response(HttpResponse::json(
                            ApiResponse::failure(content.status().code, content.status().message).to_json(), 404));
                    }
                    close_socket(client);
                    ++impl_->served;
                    return true;
                }
            }
            // 2) body 已随第 1 步的 Content-Length 一起读满；此处只做路由级体积上限校验
            //    （默认 8 MiB；上传导入放宽到硬上限）
            const auto request_started = std::chrono::steady_clock::now();
            // 原始字节上传（音频 / .rdl 歌单 / .rdm 音频包）：跳过 JSON 解析，直接交给导入分流
            const bool is_share_import = (request.method == "POST" && request.path == "/music/share/import");
            const bool is_upload =
                (request.method == "POST" && request.path == "/music/import/upload") || is_share_import;
            const size_t max_body = is_upload ? kMaxRequestBodyBytes : (8u * 1024u * 1024u);
            if (request.content_length > max_body) {
                send_response(HttpResponse::text("请求体过大", 413));
            } else {
                request.body = buffer.substr(header_bytes, request.content_length);

                // POST /music/import/upload：原始字节上传 → 暂存 → 导入（不按 JSON 解析）
                // POST /music/share/import：原始字节 → .rdl 歌单 / .rdm 音频包 / 普通音频分流
                if (is_upload) {
                    const std::string filename = request.query_value("filename", "upload.bin");
                    const ApiResponse uploaded =
                        is_share_import ? impl_->dispatcher->share_import(filename, request.body)
                                        : impl_->dispatcher->import_upload(filename, request.body);
                    send_response(to_http_response(uploaded));
                    close_socket(client);
                    ++impl_->served;
                    return true;
                }

                // 3) 转成 ApiRequest 并派发（path_segments 由 ApiDispatcher::handle 内部补齐）
                ApiRequest api_request;
                api_request.method = request.method;
                api_request.path = request.path;
                api_request.query = request.query;

                if (!request.body.empty()) {
                    json::ParseError parse_error;
                    json::Value parsed = json::Value::parse(request.body, &parse_error);
                    if (!parsed.is_object() && !parsed.is_array()) {
                        send_response(HttpResponse::json(
                            ApiResponse::failure(ErrorCode::InvalidMetadata,
                                                 "请求体不是合法 JSON: " + parse_error.message)
                                .to_json(),
                            400));
                        close_socket(client);
                        return true;
                    }
                    api_request.body = parsed;
                }

                const ApiResponse api_response = impl_->dispatcher->handle(api_request);
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now() - request_started)
                                         .count();
                if (api_response.ok()) {
                    RM_LOG_INFO("http", request.method + " " + request.path + " -> code=0 (" +
                                            std::to_string(elapsed) + " ms)");
                } else {
                    RM_LOG_WARN("http", request.method + " " + request.path + " -> code=" +
                                            std::to_string(static_cast<int>(api_response.code)) + " (" +
                                            std::to_string(elapsed) + " ms): " + api_response.message);
                }
                send_response(to_http_response(api_response));
            }
        }
    }

    close_socket(client);
    ++impl_->served;
    return true;
}

void HttpServer::serve_forever(int poll_timeout_ms) {
    while (running()) {
        poll_once(poll_timeout_ms);
        // 后台任务驱动：主线程空闲时推进下载队列，避免任务永远停在 queued
        if (impl_->idle_hook) {
            const auto now = std::chrono::steady_clock::now();
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - impl_->idle_last).count();
            if (elapsed >= impl_->idle_interval_ms) {
                impl_->idle_last = now;
                impl_->idle_hook();
            }
        }
    }
}

void HttpServer::set_idle_hook(std::function<void()> hook, int interval_ms) {
    impl_->idle_hook = std::move(hook);
    impl_->idle_interval_ms = interval_ms > 0 ? interval_ms : 300;
    impl_->idle_last = std::chrono::steady_clock::now();
}

}  // namespace radmusic
