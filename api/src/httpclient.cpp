// Rad music API - 网络传输实现（WinHTTP）
#include "radmusic/httpclient.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <fstream>
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

}  // namespace

bool is_remote_url(const std::string& url) {
    return starts_with(url, "http://") || starts_with(url, "https://");
}

bool http_fetch_file(const std::string& url, const std::filesystem::path& out_path, long long* received,
                     long long* total, std::string* error) {
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
    WinHttpSetTimeouts(session.get(), 15000, 15000, 15000, 30000);

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

    std::ofstream out(out_path, std::ios::binary | (start > 0 ? std::ios::app : std::ios::trunc));
    if (!out) return fail("无法写入临时文件: " + out_path.u8string());

    long long done = start;
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
        out.write(chunk.data(), static_cast<std::streamsize>(got));
        if (!out) return fail("写入临时文件失败（可能磁盘空间不足）: " + out_path.u8string());
        done += static_cast<long long>(got);
        if (received != nullptr) *received = done;
    }
    out.flush();
    if (!out.good()) return fail("写入临时文件失败（可能磁盘空间不足）: " + out_path.u8string());

    if (content_length >= 0 && done - start < content_length) {
        if (received != nullptr) *received = done;
        return fail("下载未完成：已接收 " + std::to_string(done - start) + " / " +
                    std::to_string(content_length) + " 字节");
    }
    if (received != nullptr) *received = done;
    return true;
#else
    (void)out_path;
    (void)received;
    (void)total;
    return fail("当前平台未提供网络下载实现: " + url);
#endif
}

}  // namespace radmusic
