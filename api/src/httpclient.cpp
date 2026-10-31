// Rad music API - 网络传输实现（Windows: WinHTTP；其他平台: curl 子进程）
#include "radmusic/httpclient.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <fstream>
#include <functional>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace radmusic {
namespace {

bool starts_with(const std::string& text, const char* prefix) {
    const std::string p(prefix);
    if (text.size() < p.size()) return false;
    for (size_t i = 0; i < p.size(); ++i) {
        char a = text[i];
        char b = p[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

#ifdef _WIN32
// 网络线路质量差的场景（尤其跨国下载）下，DNS/握手可能要二十秒以上才有结果，
// 15s 的超时会直接被掐断成 Win32 12002（ERROR_INTERNET_TIMEOUT）。这里放宽到足够慢链路也能撑住的档位。
constexpr int kResolveTimeoutMs = 30000;
constexpr int kConnectTimeoutMs = 30000;
constexpr int kSendTimeoutMs = 30000;
constexpr int kReceiveTimeoutMs = 60000;
// 连接层面偶发失败（超时/重置/被掐断）自动重试；一旦写过数据或拿到 HTTP 状态码就不再重试，
// 避免把半包数据重复写进文件。
constexpr int kMaxAttempts = 3;
#endif

#ifdef _WIN32

std::wstring to_wide(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::string to_utf8(const std::wstring& text) {
    if (text.empty()) return std::string();
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                         nullptr, nullptr);
    if (size <= 0) return std::string();
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr,
                        nullptr);
    return out;
}

std::string win32_message(DWORD code) {
    std::string out = "Win32 " + std::to_string(code);
    if (code == ERROR_DISK_FULL || code == ERROR_HANDLE_DISK_FULL) return "insufficient space (" + out + ")";
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
        code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    if (length > 0 && buffer != nullptr) {
        std::wstring text(buffer, length);
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n')) text.pop_back();
        out += ": " + to_utf8(text);
        LocalFree(buffer);
    }
    return out;
}

class Handle {
public:
    Handle() = default;
    explicit Handle(HINTERNET handle) : handle_(handle) {}
    ~Handle() {
        if (handle_ != nullptr) WinHttpCloseHandle(handle_);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HINTERNET get() const { return handle_; }
    explicit operator bool() const { return handle_ != nullptr; }

private:
    HINTERNET handle_ = nullptr;
};

#endif  // _WIN32

// 传输核心：把响应体交给 write 回调（返回 false 表示写失败），支持 Range 与分片上限。
// http_status 非空时回写 HTTP 状态码（非 Windows 实现走 curl，回写 0）。
bool http_transfer_once(const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
                       const std::function<bool(const char*, size_t)>& write, long long* received, long long* total,
                       std::string* error, long long max_bytes, int* http_status = nullptr) {
    auto fail = [error](const std::string& message) {
        if (error != nullptr) *error = message;
        return false;
    };

    if (!is_remote_url(url)) return fail("不是 http(s) 下载地址: " + url);

#ifdef _WIN32
    const std::wstring wide_url = to_wide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (wide_url.empty() ||
        !WinHttpCrackUrl(wide_url.c_str(), static_cast<DWORD>(wide_url.size()), 0, &parts)) {
        return fail("无法解析下载地址: " + url);
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring target(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (target.empty()) target = L"/";
    if (parts.dwExtraInfoLength > 0 && parts.lpszExtraInfo != nullptr) {
        target.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }
    const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    const INTERNET_PORT port = parts.nPort == 0 ? (secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT)
                                                : parts.nPort;

    Handle session(WinHttpOpen(L"RadMusic/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) return fail("初始化网络会话失败: " + win32_message(GetLastError()));
    WinHttpSetTimeouts(session.get(), kResolveTimeoutMs, kConnectTimeoutMs, kSendTimeoutMs, kReceiveTimeoutMs);

    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(session.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));

    Handle connection(WinHttpConnect(session.get(), host.c_str(), port, 0));
    if (!connection) return fail("连接下载源失败: " + to_utf8(host) + " " + win32_message(GetLastError()));

    Handle request(WinHttpOpenRequest(connection.get(), L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0));
    if (!request) return fail("创建下载请求失败: " + win32_message(GetLastError()));

    WinHttpAddRequestHeaders(request.get(),
                             L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) RadMusic/1.0",
                             static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    for (const auto& header : headers) {
        const std::wstring line = to_wide(header.first + ": " + header.second);
        if (line.empty()) continue;
        WinHttpAddRequestHeaders(request.get(), line.c_str(), static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD);
    }

    long long start = received != nullptr ? *received : 0;
    if (start < 0) start = 0;
    if (start > 0) {
        wchar_t range[64];
        std::swprintf(range, sizeof(range) / sizeof(range[0]), L"Range: bytes=%lld-", start);
        WinHttpAddRequestHeaders(request.get(), range, static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD);
    }

    if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        return fail("发送下载请求失败: " + win32_message(GetLastError()));
    }
    if (!WinHttpReceiveResponse(request.get(), nullptr)) {
        return fail("下载源无响应: " + win32_message(GetLastError()));
    }

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX)) {
        return fail("无法读取下载响应状态: " + win32_message(GetLastError()));
    }
    if (http_status != nullptr) *http_status = static_cast<int>(status);

    if (status == 416 && start > 0) {   // 请求的范围已完整下载过
        if (total != nullptr) *total = start;
        return true;
    }
    if (status < 200 || status >= 300) {
        return fail("下载源返回 HTTP " + std::to_string(status));
    }
    if (status == 200 && start > 0) start = 0;   // 服务器不支持续传，从头写入

    long long content_length = -1;
    {
        wchar_t buffer[64] = {0};
        DWORD size = sizeof(buffer) - sizeof(wchar_t);
        if (WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, buffer,
                                &size, WINHTTP_NO_HEADER_INDEX)) {
            wchar_t* end = nullptr;
            const long long parsed = std::wcstoll(buffer, &end, 10);
            if (end != buffer && parsed >= 0) content_length = parsed;
        }
    }
    if (total != nullptr) *total = content_length >= 0 ? start + content_length : -1;

    long long done = start;
    long long written = 0;
    bool truncated = false;
    std::vector<char> chunk(64 * 1024);
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available)) {
            return fail("读取下载数据失败: " + win32_message(GetLastError()));
        }
        if (available == 0) break;
        const DWORD want = std::min<DWORD>(available, static_cast<DWORD>(chunk.size()));
        DWORD got = 0;
        if (!WinHttpReadData(request.get(), chunk.data(), want, &got)) {
            return fail("读取下载数据失败: " + win32_message(GetLastError()));
        }
        if (got == 0) break;
        if (!write(chunk.data(), got)) return fail("写入失败（可能磁盘空间不足）");
        done += static_cast<long long>(got);
        written += static_cast<long long>(got);
        if (received != nullptr) *received = done;
        if (max_bytes > 0 && written >= max_bytes) {   // 分片下载：本轮到此为止
            truncated = true;
            break;
        }
    }

    if (!truncated && content_length >= 0 && done - start < content_length) {
        if (received != nullptr) *received = done;
        return fail("下载未完成：已接收 " + std::to_string(done - start) + " / " +
                    std::to_string(content_length) + " 字节");
    }
    if (received != nullptr) *received = done;
    return true;
#else
    // 非 Windows：用系统 curl（argv 直传，不经 shell）
    (void)received;
    (void)total;
    const long long start = 0;
    (void)start;
    std::vector<std::string> args;
    args.push_back("curl");
    args.push_back("-fsSL");
    if (received != nullptr && *received > 0) {
        args.push_back("-C");
        args.push_back("-");
        if (total != nullptr && *total > 0) {
            if (*received >= *total) return true;
        }
    }
    args.push_back(url);

    int fds[2];
    if (pipe(fds) != 0) return fail("创建管道失败");
    const pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp("curl", argv.data());
        _exit(127);
    }
    close(fds[1]);
    if (pid < 0) {
        close(fds[0]);
        return fail("启动 curl 失败");
    }
    long long done = received != nullptr ? *received : 0;
    long long written = 0;
    std::vector<char> chunk(64 * 1024);
    for (;;) {
        const ssize_t got = read(fds[0], chunk.data(), chunk.size());
        if (got < 0) {
            close(fds[0]);
            return fail("读取下载数据失败");
        }
        if (got == 0) break;
        if (!write(chunk.data(), static_cast<size_t>(got))) {
            close(fds[0]);
            return fail("写入失败（可能磁盘空间不足）");
        }
        done += got;
        written += got;
        if (received != nullptr) *received = done;
    }
    close(fds[0]);
    int status_code = 0;
    waitpid(pid, &status_code, 0);
    const bool exited_ok = WIFEXITED(status_code) && WEXITSTATUS(status_code) == 0;
    if (!exited_ok) {
        return fail(written == 0 ? "curl 下载失败（网络不可用或地址无效）" : "下载中断");
    }
    (void)max_bytes;
    if (total != nullptr && *total < 0) *total = done;
    return true;
#endif
}

// 传输核心：统一出口。Windows 下对「连接层失败且尚未写入任何数据」的情况自动重试若干次。
bool http_transfer(const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
                   const std::function<bool(const char*, size_t)>& write, long long* received, long long* total,
                   std::string* error, long long max_bytes, int* http_status = nullptr) {
    const long long start_offset = received != nullptr ? *received : 0;
#ifdef _WIN32
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        bool wrote_data = false;
        int status = 0;
        const bool ok = http_transfer_once(
            url, headers,
            [&write, &wrote_data](const char* data, size_t size) {
                wrote_data = true;
                return write(data, size);
            },
            received, total, error, max_bytes, &status);
        if (ok) {
            if (http_status != nullptr) *http_status = status;
            return true;
        }
        if (http_status != nullptr) *http_status = status;
        const bool server_answered = status > 0;   // 拿到状态码说明链路通了，重试没有意义
        if (wrote_data || server_answered || attempt + 1 == kMaxAttempts) break;
        if (received != nullptr) *received = start_offset;   // 重置进度，下一次仍是同一段 Range
        std::this_thread::sleep_for(std::chrono::milliseconds(700 * (attempt + 1)));
    }
    if (error != nullptr && !error->empty()) *error += "（网络不稳定，已自动重试）";
    return false;
#else
    (void)start_offset;
    return http_transfer_once(url, headers, write, received, total, error, max_bytes, http_status);
#endif
}

// 音频流同源代理的内部实现（见 http_proxy_stream）：用 WinHTTP 转发 GET（透传 Range 等头），
// 取回上游状态码、挑选的响应头与响应体。仅本翻译单元使用。
bool proxy_stream_impl(const std::string& remote_url,
                       const std::vector<std::pair<std::string, std::string>>& req_headers,
                       int* upstream_status,
                       std::vector<std::pair<std::string, std::string>>* out_headers,
                       std::string* out_body,
                       std::string* error) {
    auto fail = [error](const std::string& message) {
        if (error != nullptr) *error = message;
        return false;
    };
    if (!is_remote_url(remote_url)) return fail("不是 http(s) 地址: " + remote_url);

#ifdef _WIN32
    const std::wstring wide_url = to_wide(remote_url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (wide_url.empty() ||
        !WinHttpCrackUrl(wide_url.c_str(), static_cast<DWORD>(wide_url.size()), 0, &parts)) {
        return fail("无法解析地址: " + remote_url);
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring target(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (target.empty()) target = L"/";
    if (parts.dwExtraInfoLength > 0 && parts.lpszExtraInfo != nullptr) {
        target.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }
    const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    const INTERNET_PORT port = parts.nPort == 0
                                   ? (secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT)
                                   : parts.nPort;

    Handle session(WinHttpOpen(L"RadMusic/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) return fail("初始化网络会话失败: " + win32_message(GetLastError()));
    WinHttpSetTimeouts(session.get(), kResolveTimeoutMs, kConnectTimeoutMs, kSendTimeoutMs, kReceiveTimeoutMs);

    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(session.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));

    Handle connection(WinHttpConnect(session.get(), host.c_str(), port, 0));
    if (!connection) return fail("连接上游失败: " + to_utf8(host) + " " + win32_message(GetLastError()));

    Handle request(WinHttpOpenRequest(connection.get(), L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0));
    if (!request) return fail("创建请求失败: " + win32_message(GetLastError()));

    WinHttpAddRequestHeaders(
        request.get(), L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) RadMusic/1.0",
        static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    auto to_lower = [](const std::string& s) {
        std::string o; o.reserve(s.size());
        for (char c : s) o.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c);
        return o;
    };
    for (const auto& header : req_headers) {
        const std::string name_l = to_lower(header.first);
        // 这些头由传输层处理，转发出去反而出错，跳过
        if (name_l == "host" || name_l == "connection" || name_l == "accept-encoding" ||
            name_l == "content-length" || name_l == "transfer-encoding") {
            continue;
        }
        const std::wstring line = to_wide(header.first + ": " + header.second);
        if (line.empty()) continue;
        WinHttpAddRequestHeaders(request.get(), line.c_str(), static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD);
    }

    if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        return fail("发送代理请求失败: " + win32_message(GetLastError()));
    }
    if (!WinHttpReceiveResponse(request.get(), nullptr)) {
        return fail("上游无响应: " + win32_message(GetLastError()));
    }

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX)) {
        return fail("无法读取上游响应状态: " + win32_message(GetLastError()));
    }
    if (upstream_status != nullptr) *upstream_status = static_cast<int>(status);

    // 挑选需透传给浏览器的响应头（音频播放 / seek 必需）
    if (out_headers != nullptr) {
        static const wchar_t* kPick[] = {L"Content-Type",  L"Content-Range",  L"Accept-Ranges",
                                         L"Content-Length", L"Cache-Control",  L"ETag",
                                         L"Last-Modified"};
        for (const wchar_t* name : kPick) {
            wchar_t buffer[1024];
            DWORD size = sizeof(buffer);
            if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_CUSTOM, name, buffer, &size,
                                     WINHTTP_NO_HEADER_INDEX)) {
                continue;  // 该头不存在或读取失败，跳过
            }
            std::wstring value(buffer, size / sizeof(wchar_t));
            while (!value.empty() && value.back() == L'\0') value.pop_back();
            out_headers->emplace_back(to_utf8(name), to_utf8(value));
        }
    }

    // 读取响应体（浏览器按 Range 分段请求，单次体量可控，直接缓冲后回传）
    if (out_body != nullptr) {
        out_body->clear();
        std::vector<char> chunk(64 * 1024);
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request.get(), &available)) break;
            if (available == 0) break;
            const DWORD want = std::min<DWORD>(available, static_cast<DWORD>(chunk.size()));
            DWORD got = 0;
            if (!WinHttpReadData(request.get(), chunk.data(), want, &got)) break;
            if (got == 0) break;
            out_body->append(chunk.data(), got);
        }
    }
    return true;
#else
    (void)req_headers;
    (void)upstream_status;
    (void)out_headers;
    (void)out_body;
    return fail("非 Windows 平台暂不支持音频代理");
#endif
}

}  // namespace

bool is_remote_url(const std::string& url) {
    return starts_with(url, "http://") || starts_with(url, "https://");
}

bool http_proxy_stream(const std::string& remote_url,
                       const std::vector<std::pair<std::string, std::string>>& req_headers,
                       int* upstream_status,
                       std::vector<std::pair<std::string, std::string>>* out_headers,
                       std::string* out_body,
                       std::string* error) {
    return proxy_stream_impl(remote_url, req_headers, upstream_status, out_headers, out_body, error);
}

bool http_fetch_file(const std::string& url, const std::filesystem::path& out_path, long long* received,
                     long long* total, std::string* error, long long max_bytes) {
    long long start = received != nullptr ? *received : 0;
    if (start < 0) start = 0;
    std::ofstream out(out_path, std::ios::binary | (start > 0 ? std::ios::app : std::ios::trunc));
    if (!out) {
        if (error != nullptr) *error = "无法写入临时文件: " + std::string(out_path.u8string().c_str());
        return false;
    }
    bool write_failed = false;
    const bool ok = http_transfer(
        url, {}, [&out, &write_failed](const char* data, size_t size) {
            if (write_failed) return false;
            out.write(data, static_cast<std::streamsize>(size));
            if (!out) {
                write_failed = true;
                return false;
            }
            return true;
        },
        received, total, error, max_bytes);
    out.flush();
    if (ok && !out.good() && error != nullptr) *error = "写入临时文件失败（可能磁盘空间不足）";
    return ok && out.good();
}

bool http_fetch_text(const std::string& url,
                     const std::vector<std::pair<std::string, std::string>>& extra_headers, std::string& out,
                     std::string* error, long long max_bytes) {
    out.clear();
    bool too_large = false;
    const bool ok = http_transfer(
        url, extra_headers,
        [&out, max_bytes, &too_large](const char* data, size_t size) {
            if (max_bytes > 0 && static_cast<long long>(out.size() + size) > max_bytes) {
                too_large = true;
                return false;
            }
            out.append(data, size);
            return true;
        },
        nullptr, nullptr, error, 0);
    if (too_large) {
        if (error != nullptr) *error = "响应体积超过上限";
        out.clear();
        return false;
    }
    if (!ok) out.clear();
    return ok;
}

bool http_fetch_segment(const std::string& url, const std::filesystem::path& out_path, long long start, long long end,
                        const std::function<bool(long long)>& on_progress, std::string* error) {
    if (end < start) {
        if (error != nullptr) *error = "分段范围非法";
        return false;
    }
    if (!is_remote_url(url)) {
        if (error != nullptr) *error = "不是 http(s) 下载地址: " + url;
        return false;
    }
    const long long expected = end - start + 1;

#ifdef _WIN32
    std::fstream out(out_path, std::ios::in | std::ios::out | std::ios::binary);
    if (!out) {
        if (error != nullptr) *error = "无法打开更新包临时文件";
        return false;
    }
    out.seekp(static_cast<std::streamoff>(start));
    if (!out) {
        if (error != nullptr) *error = "无法定位分段写入位置";
        return false;
    }

    long long written = 0;
    bool overflow = false;
    bool aborted = false;
    int status = 0;
    const std::string range = "bytes=" + std::to_string(start) + "-" + std::to_string(end);
    const bool ok = http_transfer(
        url, {{"Range", range}},
        [&out, &written, &overflow, &aborted, expected, &on_progress](const char* data, size_t size) {
            if (written >= expected) {   // 服务器没按请求的范围返回
                overflow = true;
                return false;
            }
            const size_t room = static_cast<size_t>(expected - written);
            const size_t take = size < room ? size : room;
            out.write(data, static_cast<std::streamsize>(take));
            if (!out) return false;
            written += static_cast<long long>(take);
            if (on_progress && !on_progress(written)) {   // 调用方要求中止
                aborted = true;
                return false;
            }
            return true;
        },
        nullptr, nullptr, error, 0, &status);
    out.flush();

    if (aborted) {
        if (error != nullptr) *error = "分段下载已中止";
        return false;
    }
    if (overflow) {
        if (error != nullptr) *error = "下载源未按请求的范围返回数据（可能不支持分段下载）";
        return false;
    }
    if (start > 0 && status == 200) {
        if (error != nullptr) *error = "下载源不支持分段下载（返回 HTTP 200 全量数据）";
        return false;
    }
    if (!ok || !out.good()) {
        if (error != nullptr && error->empty()) *error = "分段写入失败（磁盘空间不足？）";
        return false;
    }
    if (written != expected) {
        if (error != nullptr) {
            *error = "分段下载不完整：" + std::to_string(written) + " / " + std::to_string(expected) + " 字节";
        }
        return false;
    }
    return true;
#else
    // 非 Windows：curl -r start-end，stdout 通过管道按偏移写入
    const int fd = ::open(out_path.c_str(), O_WRONLY, 0644);
    if (fd < 0) {
        if (error != nullptr) *error = "无法打开更新包临时文件";
        return false;
    }
    int fds[2];
    if (pipe(fds) != 0) {
        ::close(fd);
        if (error != nullptr) *error = "创建管道失败";
        return false;
    }
    std::vector<std::string> args = {"curl", "-fsSL", "-r",
                                     std::to_string(start) + "-" + std::to_string(end), url};
    const pid_t pid = fork();
    if (pid == 0) {
        ::close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        ::close(fds[1]);
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp("curl", argv.data());
        _exit(127);
    }
    ::close(fds[1]);
    if (pid < 0) {
        ::close(fds[0]);
        ::close(fd);
        if (error != nullptr) *error = "启动 curl 失败";
        return false;
    }

    long long written = 0;
    bool io_error = false;
    std::vector<char> chunk(64 * 1024);
    for (;;) {
        const ssize_t got = ::read(fds[0], chunk.data(), chunk.size());
        if (got < 0) {
            io_error = true;
            break;
        }
        if (got == 0) break;
        ssize_t done = 0;
        while (done < got) {
            const ssize_t put = ::pwrite(fd, chunk.data() + done, static_cast<size_t>(got - done), start + written + done);
            if (put <= 0) {
                io_error = true;
                break;
            }
            done += put;
        }
        if (io_error) break;
        written += got;
        if (on_progress && !on_progress(written)) {   // 调用方要求中止
            ::close(fds[0]);
            int ignored = 0;
            waitpid(pid, &ignored, 0);
            ::close(fd);
            if (error != nullptr) *error = "分段下载已中止";
            return false;
        }
    }
    ::close(fds[0]);
    int status_code = 0;
    waitpid(pid, &status_code, 0);
    ::close(fd);

    if (io_error) {
        if (error != nullptr) *error = "写入更新包失败（磁盘空间不足？）";
        return false;
    }
    if (!(WIFEXITED(status_code) && WEXITSTATUS(status_code) == 0)) {
        if (error != nullptr) *error = "curl 分段下载失败（该源可能不支持 Range）";
        return false;
    }
    if (written != expected) {
        if (error != nullptr) {
            *error = "分段下载不完整：" + std::to_string(written) + " / " + std::to_string(expected) + " 字节";
        }
        return false;
    }
    return true;
#endif
}

}  // namespace radmusic
