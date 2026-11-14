// Rad Music 桌面端 —— Win32 + WebView2 独立窗口壳
//
// 它是「不依赖浏览器」的桌面入口：自带一个原生窗口，内嵌 WebView2 渲染现有的 web/ 界面，
// 并在启动时拉起同目录的 radmusic_server.exe（服务端仍是最初的 C++17 程序，架构不变）。
//
// 行为：
//   1. 单实例：重复双击时把已有窗口激活到前台，不再开第二个进程。
//   2. 若 127.0.0.1:8765 已有服务在跑（例如用户自己用 bat 起的），直接复用，退出时不杀它。
//   3. 自己拉起的服务端在关窗时通过 POST /music/shutdown 优雅退出，超时再 TerminateProcess。
//   4. 服务端启动失败时，把它的输出（已重定向到 <root>/logs/server-stdout.log）贴到弹窗里。
//   5. WebView2 初始化失败时翻译错误码；用户数据目录被占用会自动换备用目录重试。
//
// 构建：见同目录 build_desktop.sh
//   g++ -std=c++17 -O2 -static -municode -mwindows main.cpp app.res -o radmusic_desktop.exe \
//       -I<WebView2 头目录> -lws2_32 -lole32 -loleaut32 -luuid -lshell32 -lshlwapi -lgdi32 -luser32

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdio>
#include <string>
#include <vector>

#include "WebView2.h"

namespace {

constexpr wchar_t kWindowClass[] = L"RadMusicDesktopWindow";
constexpr wchar_t kWindowTitle[] = L"Rad Music";
constexpr int kDefaultPort = 8765;
constexpr int kMinWidth = 940;
constexpr int kMinHeight = 620;

HINSTANCE g_instance = nullptr;
HWND g_hwnd = nullptr;
ICoreWebView2Controller* g_controller = nullptr;
ICoreWebView2* g_webview = nullptr;

PROCESS_INFORMATION g_server{};
bool g_server_owned = false;      // 服务端是否由本进程拉起（决定关窗时是否关闭它）
int g_port = kDefaultPort;
bool g_closing = false;

// 排查用日志：%LOCALAPPDATA%\RadMusic\desktop.log（GUI 程序没有控制台，只能落文件）
std::wstring g_log_path;

// exe 所在目录（不依赖后面定义的 exe_dir）
std::wstring self_dir() {
    wchar_t buf[MAX_PATH];
    const DWORD got = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (got == 0 || got >= MAX_PATH) return std::wstring();
    std::wstring path(buf, got);
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

// 目录是否可写（第三方安全软件的“沙箱”常只放行程序自身目录）
bool can_write_dir(const std::wstring& dir) {
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring probe = dir + L"\\.write-test";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

// 日志：优先程序目录（安全软件沙箱一般放行），其次 %LOCALAPPDATA%
void log_line(const std::wstring& text) {
    if (g_log_path.empty()) {
        const std::wstring dir = self_dir();
        if (!dir.empty()) {
            const std::wstring candidate = dir + L"\\desktop.log";
            HANDLE probe = CreateFileW(candidate.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (probe != INVALID_HANDLE_VALUE) {
                CloseHandle(probe);
                g_log_path = candidate;
            }
        }
        if (g_log_path.empty()) {
            PWSTR local_app = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local_app)) &&
                local_app != nullptr) {
                g_log_path = std::wstring(local_app) + L"\\RadMusic\\desktop.log";
                CoTaskMemFree(local_app);
            }
        }
        if (g_log_path.empty()) return;
    }
    HANDLE file = CreateFileW(g_log_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t stamp[64];
    swprintf(stamp, 64, L"[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
    std::wstring line = std::wstring(stamp) + text + L"\r\n";
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), nullptr, 0,
                                          nullptr, nullptr);
    if (bytes > 0) {
        std::string utf8(static_cast<size_t>(bytes), '\0');
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), utf8.data(), bytes, nullptr,
                            nullptr);
        DWORD written = 0;
        WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }
    CloseHandle(file);
}

// ---------------------------------------------------------------- 字符串 / 路径工具

std::wstring widen(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::wstring exe_dir() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD got = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (got == 0) return std::wstring();
        if (got < buf.size()) {
            buf.resize(got);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    const size_t slash = buf.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : buf.substr(0, slash);
}

bool path_exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool is_directory(const std::wstring& path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring join(const std::wstring& dir, const wchar_t* name) {
    if (dir.empty()) return name;
    if (dir.back() == L'\\' || dir.back() == L'/') return dir + name;
    return dir + L"\\" + name;
}

// 相对路径 → 绝对路径（切换工作目录前必须先钉住，否则会解析到新目录）
std::wstring to_absolute(const std::wstring& path) {
    if (path.empty()) return path;
    const DWORD need = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (need == 0) return path;
    std::wstring buffer(need, L'\0');
    const DWORD got = GetFullPathNameW(path.c_str(), need, buffer.data(), nullptr);
    if (got == 0 || got > need) return path;
    buffer.resize(got);
    return buffer;
}

// %LOCALAPPDATA%（拿不到时返回空）
std::wstring local_app_dir() {
    PWSTR raw = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)) && raw != nullptr) {
        out = raw;
    }
    if (raw != nullptr) CoTaskMemFree(raw);
    return out;
}

// ---------------------------------------------------------------- Winsock

bool ensure_winsock() {
    static bool ready = []() {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ready;
}

// 端口是否已经有人监听（判断服务是否就绪 / 是否已有实例）
bool port_alive(int port, int timeout_ms) {
    if (!ensure_winsock()) return false;
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    u_long nonblocking = 1;
    ioctlsocket(s, FIONBIO, &nonblocking);
    const int rc = ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    bool ok = false;
    if (rc == 0) {
        ok = true;
    } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set write_set;
        fd_set error_set;
        FD_ZERO(&write_set);
        FD_ZERO(&error_set);
        FD_SET(s, &write_set);
        FD_SET(s, &error_set);
        timeval tv{};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        const int ready = select(0, nullptr, &write_set, &error_set, &tv);
        ok = ready > 0 && !FD_ISSET(s, &error_set);
    }
    closesocket(s);
    return ok;
}

// 请求服务端优雅退出：POST /music/shutdown
// 服务端要求 body 带 {"confirm":"SHUTDOWN"}（防误触），空请求会被拒（code 1011）。
// 返回 true 表示请求已送达（不代表服务端已经退完）。
bool request_server_shutdown(int port) {
    if (!ensure_winsock()) {
        log_line(L"关闭流程：Winsock 不可用");
        return false;
    }
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        log_line(L"关闭流程：创建 socket 失败 " + std::to_wstring(WSAGetLastError()));
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    timeval tv{};
    tv.tv_sec = 2;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));

    bool delivered = false;
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        log_line(L"关闭流程：连接 127.0.0.1:" + std::to_wstring(port) + L" 失败 " +
                 std::to_wstring(WSAGetLastError()));
    } else {
        const std::string body = "{\"confirm\":\"SHUTDOWN\"}";
        std::string request = "POST /music/shutdown HTTP/1.1\r\n";
        request += "Host: 127.0.0.1\r\n";
        request += "Content-Type: application/json\r\n";
        request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        request += "Connection: close\r\n\r\n";
        request += body;
        const int sent = ::send(s, request.data(), static_cast<int>(request.size()), 0);
        if (sent <= 0) {
            log_line(L"关闭流程：发送请求失败 " + std::to_wstring(WSAGetLastError()));
        } else {
            delivered = true;
            char buf[1024];
            const int got = ::recv(s, buf, sizeof(buf), 0);
            if (got > 0) {
                std::string reply(buf, static_cast<size_t>(got));
                const size_t pos = reply.find("\r\n\r\n");
                const std::string payload = pos == std::string::npos ? reply : reply.substr(pos + 4);
                log_line(L"关闭流程：服务端回应 " + widen(payload.substr(0, 160)));
            }
        }
    }
    closesocket(s);
    return delivered;
}

// ---------------------------------------------------------------- 服务端进程

// 拼命令行：参数里的空格用引号包住
std::wstring build_command_line(const std::wstring& exe, const std::vector<std::wstring>& args) {
    std::wstring line = L"\"" + exe + L"\"";
    for (const auto& a : args) {
        line += L" ";
        if (a.find_first_of(L" \t") == std::wstring::npos) {
            line += a;
        } else {
            line += L"\"" + a + L"\"";
        }
    }
    return line;
}

struct LaunchPlan {
    std::wstring server_exe;
    std::wstring root;
    std::wstring web;
};

std::wstring pick_server_exe(const std::wstring& dir) {
    const wchar_t* candidates[] = {
        L"radmusic_server.exe",
        L"api\\build\\radmusic_server.exe",
        L"bin\\radmusic_server.exe",
    };
    for (const wchar_t* name : candidates) {
        const std::wstring full = join(dir, name);
        if (path_exists(full)) return full;
    }
    return std::wstring();
}

// 读文件末尾若干字符（启动失败时把服务端最后的输出贴给用户）
std::wstring tail_of_file(const std::wstring& path, size_t bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::wstring();
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    const long long want = static_cast<long long>(bytes);
    const long long from = size.QuadPart > want ? size.QuadPart - want : 0;
    LARGE_INTEGER pos{};
    pos.QuadPart = from;
    SetFilePointerEx(file, pos, nullptr, FILE_BEGIN);
    std::string data(static_cast<size_t>(size.QuadPart - from), '\0');
    DWORD read = 0;
    ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr);
    CloseHandle(file);
    data.resize(read);
    if (data.empty()) return std::wstring();
    const int wide = MultiByteToWideChar(CP_UTF8, 0, data.data(), static_cast<int>(data.size()), nullptr, 0);
    if (wide <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(wide), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, data.data(), static_cast<int>(data.size()), out.data(), wide);
    return out;
}

// 服务端 stdout/stderr 落到哪个文件（启动失败时读它）
std::wstring server_output_log(const LaunchPlan& plan) {
    std::wstring base = plan.root;
    if (base.empty()) base = join(exe_dir(), L"radmusic-data");
    const std::wstring logs = join(base, L"logs");
    CreateDirectoryW(logs.c_str(), nullptr);
    return join(logs, L"server-stdout.log");
}

bool spawn_server(const LaunchPlan& plan, std::wstring* error) {
    std::vector<std::wstring> args;
    if (!plan.root.empty()) {
        args.push_back(L"--root");
        args.push_back(plan.root);
    }
    if (!plan.web.empty()) {
        args.push_back(L"--web");
        args.push_back(plan.web);
    }
    args.push_back(L"--port");
    args.push_back(std::to_wstring(g_port));

    const std::wstring line = build_command_line(plan.server_exe, args);
    std::vector<wchar_t> mutable_line(line.begin(), line.end());
    mutable_line.push_back(L'\0');

    // 把服务端输出接到文件：GUI 程序没有控制台，不接的话「启动失败」完全看不到原因
    const std::wstring out_path = server_output_log(plan);
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE out = CreateFileW(out_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (out != INVALID_HANDLE_VALUE) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdOutput = out;
        si.hStdError = out;
        si.hStdInput = INVALID_HANDLE_VALUE;
    }
    PROCESS_INFORMATION pi{};

    std::wstring workdir = plan.server_exe;
    const size_t slash = workdir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) workdir.resize(slash);

    const BOOL ok = CreateProcessW(nullptr, mutable_line.data(), nullptr, nullptr,
                                   out != INVALID_HANDLE_VALUE ? TRUE : FALSE, CREATE_NO_WINDOW, nullptr,
                                   workdir.empty() ? nullptr : workdir.c_str(), &si, &pi);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (!ok) {
        const DWORD code = GetLastError();
        if (error != nullptr) {
            *error = L"无法启动服务端：\n" + plan.server_exe + L"\n错误码 " + std::to_wstring(code);
        }
        return false;
    }
    CloseHandle(pi.hThread);
    g_server = pi;
    g_server_owned = true;
    return true;
}

// 自己拉起的服务端是否已退出（退出了就别再傻等端口）
bool server_exited(DWORD* code) {
    if (!g_server_owned || g_server.hProcess == nullptr) return false;
    if (WaitForSingleObject(g_server.hProcess, 0) != WAIT_OBJECT_0) return false;
    DWORD exit_code = 0;
    GetExitCodeProcess(g_server.hProcess, &exit_code);
    if (code != nullptr) *code = exit_code;
    return true;
}

// ---------------------------------------------------------------- WebView2

struct EnvHandler : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
    ULONG ref = 1;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr) return E_POINTER;
        // 只有一个接口的简单回调对象：任何 IID 都返回自身（vtable 布局一致，运行时只调 Invoke）
        (void)riid;
        *ppv = this;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --ref;
        if (left == 0) delete this;
        return left;
    }

    HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Environment* env) override;
};

struct ControllerHandler : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
    ULONG ref = 1;
    // 创建失败要重试，重试需要原环境；换目录重建环境时由 EnvHandler 重新给
    ICoreWebView2Environment* env = nullptr;
    int attempt = 0;

    ControllerHandler() = default;
    ControllerHandler(ICoreWebView2Environment* environment, int tries) : env(environment), attempt(tries) {
        if (env != nullptr) env->AddRef();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr) return E_POINTER;
        (void)riid;
        *ppv = this;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --ref;
        if (left == 0) {
            if (env != nullptr) env->Release();
            delete this;
        }
        return left;
    }

    HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Controller* controller) override;
};

// 导航完成回调：页面打不开时给个可读提示，而不是一片白
struct NavHandler : public ICoreWebView2NavigationCompletedEventHandler {
    ULONG ref = 1;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr) return E_POINTER;
        (void)riid;
        *ppv = this;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --ref;
        if (left == 0) delete this;
        return left;
    }

    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* sender,
                                     ICoreWebView2NavigationCompletedEventArgs* args) override {
        BOOL success = TRUE;
        if (args != nullptr) args->get_IsSuccess(&success);
        if (success) return S_OK;
        COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
        if (args != nullptr) args->get_WebErrorStatus(&status);
        log_line(L"页面加载失败，状态 " + std::to_wstring(static_cast<int>(status)));
        if (sender != nullptr) {
            const std::wstring html =
                L"<!doctype html><html><body style='margin:0;height:100vh;background:#0F0F12;color:#c9c6d6;"
                L"font-family:system-ui,-apple-system,sans-serif;display:flex;align-items:center;"
                L"justify-content:center'><div style='text-align:center;padding:32px'>"
                L"<h3 style='font-weight:600;margin:0 0 12px'>界面加载失败</h3>"
                L"<p style='color:#8b87a0;font-size:13px;line-height:1.8'>错误状态：" +
                std::to_wstring(static_cast<int>(status)) +
                L"<br>确认服务端在 127.0.0.1:" + std::to_wstring(g_port) + L" 上运行，然后重新打开窗口。</p>"
                L"</div></body></html>";
            sender->NavigateToString(html.c_str());
        }
        return S_OK;
    }
};

// WebView2 环境创建：用户数据目录可能被「上一个没退干净的实例」占用（ERROR_BUSY 0x800700AA），
// 目录状态也可能损坏；失败时换一个全新的备用目录重试，而不是把整个应用关掉。
using PFN_CreateEnv = HRESULT(STDAPICALLTYPE*)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*,
                                              ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);

PFN_CreateEnv g_create_env = nullptr;
std::wstring g_user_data_base;
int g_env_attempt = 0;
constexpr int kMaxEnvAttempts = 3;

std::wstring hresult_text(HRESULT hr) {
    wchar_t buffer[32];
    swprintf(buffer, 32, L"0x%08X", static_cast<unsigned int>(hr));
    return buffer;
}

// 用户数据目录被占用 / 无权限时的错误码（重试或换目录能救回来）
bool is_dir_conflict(HRESULT hr) {
    return hr == HRESULT_FROM_WIN32(ERROR_BUSY) || hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) ||
           hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION);
}

std::wstring explain_env_error(HRESULT hr) {
    if (hr == HRESULT_FROM_WIN32(ERROR_BUSY) || hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION)) {
        return L"用户数据目录正被另一个 Rad Music 窗口占用（上一个窗口可能还没完全退出）";
    }
    if (hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)) return L"没有权限写入用户数据目录";
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) return L"WebView2 运行时文件缺失";
    return L"原因未知";
}

// 日志文件路径（弹窗里提示用户去哪看）
std::wstring log_path_hint() {
    return g_log_path.empty() ? std::wstring(L"(尚未创建)") : g_log_path;
}

void resize_webview() {
    if (g_controller == nullptr || g_hwnd == nullptr) return;
    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    g_controller->put_Bounds(rc);
}

void show_message(const std::wstring& text, UINT icon) {
    MessageBoxW(g_hwnd, text.c_str(), kWindowTitle, MB_OK | icon);
}

bool start_environment(const std::wstring& dir, int attempt) {
    if (g_create_env == nullptr) return false;
    g_env_attempt = attempt;
    const HRESULT hr = g_create_env(nullptr, dir.c_str(), nullptr, new EnvHandler());
    if (FAILED(hr)) {
        log_line(L"CreateCoreWebView2EnvironmentWithOptions 立即失败 " + hresult_text(hr));
        return false;
    }
    return true;
}

HRESULT ControllerHandler::Invoke(HRESULT result, ICoreWebView2Controller* controller) {
    if (FAILED(result) || controller == nullptr) {
        log_line(L"WebView2 控制器创建失败 " + hresult_text(result) + L"（" + explain_env_error(result) +
                 L"），attempt=" + std::to_wstring(attempt));

        // 同一环境内短重试：上一次的浏览器进程可能还在退出（0x800700AA），
        // 或运行时装完/更新后状态还没稳定（0x8000FFFF E_UNEXPECTED）。
        // 注意：**不能**换目录重建环境 —— WebView2 在同一个 UI 线程重复创建环境会进入
        // 未定义状态，反而把「重开一次就好」的情况变成必失败。
        if (attempt < 2 && env != nullptr) {
            Sleep(1000);
            log_line(L"重试创建控制器，第 " + std::to_wstring(attempt + 1) + L" 次…");
            env->CreateCoreWebView2Controller(g_hwnd, new ControllerHandler(env, attempt + 1));
            return S_OK;
        }

        std::wstring text = L"WebView2 初始化失败（" + hresult_text(result) + L"）：" + explain_env_error(result);
        if (attempt > 0) text += L"\n\n已自动重试 " + std::to_wstring(attempt) + L" 次，仍然失败。";
        text += L"\n\n请按顺序试：\n"
                L"· 关掉本窗口，等 3 秒再重新打开（最常见，浏览器进程需要时间退出）\n"
                L"· 任务管理器里结束残留的 radmusic_desktop.exe / msedgewebview2.exe\n"
                L"· 仍不行就重启电脑\n"
                L"· 若提示缺少运行时，安装：\n"
                L"  https://developer.microsoft.com/microsoft-edge/webview2/\n\n"
                L"详细日志：" + log_path_hint();
        show_message(text, MB_ICONERROR);
        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
        return S_OK;
    }

    g_controller = controller;
    g_controller->AddRef();
    g_controller->put_IsVisible(TRUE);
    resize_webview();

    if (FAILED(g_controller->get_CoreWebView2(&g_webview)) || g_webview == nullptr) {
        log_line(L"get_CoreWebView2 失败");
        show_message(L"WebView2 内核创建失败。", MB_ICONERROR);
        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
        return S_OK;
    }
    g_webview->AddRef();

    ICoreWebView2Settings* settings = nullptr;
    if (SUCCEEDED(g_webview->get_Settings(&settings)) && settings != nullptr) {
        settings->put_IsStatusBarEnabled(FALSE);
        settings->put_AreDefaultContextMenusEnabled(FALSE);   // 生产态去掉「查看源代码 / 刷新」菜单
        settings->put_AreDevToolsEnabled(TRUE);                // 保留 F12 便于排障
        settings->put_IsZoomControlEnabled(FALSE);
        settings->Release();
    }

    EventRegistrationToken token{};
    g_webview->add_NavigationCompleted(new NavHandler(), &token);

    const std::wstring url = L"http://127.0.0.1:" + std::to_wstring(g_port) + L"/";
    g_webview->Navigate(url.c_str());
    log_line(L"已导航到 " + url);
    return S_OK;
}

HRESULT EnvHandler::Invoke(HRESULT result, ICoreWebView2Environment* env) {
    if (FAILED(result) || env == nullptr) {
        log_line(L"WebView2 环境创建失败 " + hresult_text(result) + L"（" + explain_env_error(result) + L"）");
        show_message(L"WebView2 环境初始化失败（" + hresult_text(result) + L"）：" + explain_env_error(result) +
                         L"\n\n请关掉本窗口，等 3 秒后重新打开；\n"
                         L"或结束任务管理器里残留的 radmusic_desktop.exe / msedgewebview2.exe。\n\n"
                         L"详细日志：" + log_path_hint(),
                     MB_ICONERROR);
        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
        return S_OK;
    }
    env->CreateCoreWebView2Controller(g_hwnd, new ControllerHandler(env, 0));
    return S_OK;
}

bool init_webview() {
    // 数据目录优先放程序旁边：360 之类的「沙箱运行」会拦 %LOCALAPPDATA% 写入，
    // 但通常允许程序写自身目录 —— 否则 WebView2 会报 0x800700AA（目录被占用）。
    std::wstring data_dir;
    const std::wstring beside_exe = join(exe_dir(), L"WebView2Data");
    if (can_write_dir(beside_exe)) {
        data_dir = beside_exe;
    } else {
        const std::wstring app_data = local_app_dir();
        data_dir = app_data.empty() ? beside_exe : join(app_data, L"RadMusic\\WebView2");
    }

    // WebView2Loader 走动态加载：即使系统缺少运行时也能给出可读提示，而不是「缺少 dll」弹窗。
    HMODULE loader = LoadLibraryW(L"WebView2Loader.dll");
    if (loader == nullptr) {
        loader = LoadLibraryW(L"C:\\Windows\\System32\\WebView2Loader.dll");
    }
    if (loader == nullptr) {
        log_line(L"缺少 WebView2Loader.dll");
        show_message(L"缺少 WebView2Loader.dll。\n请把它与 radmusic_desktop.exe 放在同一目录。", MB_ICONERROR);
        return false;
    }

    using PFN_GetVersion = HRESULT(STDAPICALLTYPE*)(PCWSTR, LPWSTR*);
    auto get_version =
        reinterpret_cast<PFN_GetVersion>(GetProcAddress(loader, "GetAvailableCoreWebView2BrowserVersionString"));
    auto create_env = reinterpret_cast<PFN_CreateEnv>(
        GetProcAddress(loader, "CreateCoreWebView2EnvironmentWithOptions"));
    if (create_env == nullptr) {
        log_line(L"WebView2Loader.dll 版本不匹配");
        show_message(L"WebView2Loader.dll 版本不匹配（缺少 CreateCoreWebView2EnvironmentWithOptions）。", MB_ICONERROR);
        return false;
    }

    LPWSTR version = nullptr;
    if (get_version != nullptr && SUCCEEDED(get_version(nullptr, &version)) && version != nullptr) {
        log_line(L"WebView2 运行时版本 " + std::wstring(version));
        CoTaskMemFree(version);
    } else {
        log_line(L"未检测到 WebView2 运行时");
        show_message(L"未检测到 Microsoft Edge WebView2 运行时。\n\n"
                     L"请先安装（系统自带 Edge 一般已安装）：\n"
                     L"https://developer.microsoft.com/microsoft-edge/webview2/",
                     MB_ICONERROR);
        return false;
    }

    log_line(L"WebView2 数据目录: " + data_dir);
    g_create_env = create_env;
    g_user_data_base = data_dir;
    return start_environment(data_dir, 0);
}

// ---------------------------------------------------------------- 窗口

void shutdown_everything() {
    if (g_webview != nullptr) {
        g_webview->Release();
        g_webview = nullptr;
    }
    if (g_controller != nullptr) {
        g_controller->Close();
        g_controller->Release();
        g_controller = nullptr;
        // 给 WebView2 浏览器进程一点时间退出并释放 user data 目录，
        // 否则「关掉立刻又打开」会撞上 ERROR_BUSY
        Sleep(400);
    }
    if (g_server_owned && g_server.hProcess != nullptr) {
        log_line(L"关闭流程：请服务端退出…");
        const bool delivered = request_server_shutdown(g_port);
        // 送达就给它 1.5 秒写完 settings / 索引；没送达直接强杀。
        // 不长等：关窗后进程还赖着，用户会以为「关不掉」。
        const DWORD wait_ms = delivered ? 1500 : 800;
        if (WaitForSingleObject(g_server.hProcess, wait_ms) == WAIT_TIMEOUT) {
            log_line(L"关闭流程：等待超时，强制结束服务端");
            TerminateProcess(g_server.hProcess, 0);
            WaitForSingleObject(g_server.hProcess, 1000);
        }
        log_line(L"关闭流程：服务端已退出");
        CloseHandle(g_server.hProcess);
        g_server.hProcess = nullptr;
        g_server_owned = false;
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_CREATE:
            return 0;
        case WM_SIZE:
            resize_webview();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            info->ptMinTrackSize.x = kMinWidth;
            info->ptMinTrackSize.y = kMinHeight;
            return 0;
        }
        case WM_CLOSE:
            // 先隐藏窗口：关闭服务端要等它优雅退出（最多几秒），
            // 不能让用户盯着一个「关不掉」的窗口。
            ShowWindow(hwnd, SW_HIDE);
            if (!g_closing) {
                g_closing = true;
                shutdown_everything();
            }
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// 单实例：已有窗口就激活它
bool activate_existing_instance() {
    SetLastError(0);
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"RadMusicDesktop_SingleInstance");
    const DWORD err = GetLastError();
    if (mutex == nullptr) return false;
    if (err == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(kWindowClass, nullptr);
        if (existing != nullptr) {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        CloseHandle(mutex);
        return true;
    }
    // 故意不关闭 mutex：进程存活期间保持占用
    return false;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_cmd) {
    // 临时诊断：确认进程真的进到了这里（绝对路径，不依赖任何 API）
    {
        HANDLE bf = CreateFileW(L"C:\\Windows\\Temp\\radmusic-boot.log", FILE_APPEND_DATA,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
        if (bf != INVALID_HANDLE_VALUE) {
            const char msg[] = "enter wWinMain\r\n";
            DWORD written = 0;
            WriteFile(bf, msg, sizeof(msg) - 1, &written, nullptr);
            CloseHandle(bf);
        }
    }
    g_instance = instance;
    log_line(L"=== 启动 radmusic_desktop ===");

    if (activate_existing_instance()) {
        log_line(L"已有实例在运行，激活后退出");
        return 0;
    }

    // 高 DPI：失败也无妨（老系统没有这个 API）
    using PFN_SetDpi = BOOL(WINAPI*)(HANDLE);
    if (auto set_dpi = reinterpret_cast<PFN_SetDpi>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"))) {
        set_dpi(reinterpret_cast<HANDLE>(-4));   // PER_MONITOR_AWARE_V2
    }

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        log_line(L"CoInitializeEx 失败");
        return 1;
    }

    // 命令行：--root <目录> --web <目录> --port <端口> --server <exe>
    LaunchPlan plan;
    std::wstring explicit_server;
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv != nullptr) {
            for (int i = 1; i + 1 < argc; ++i) {
                const std::wstring flag = argv[i];
                if (flag == L"--root") plan.root = argv[++i];
                else if (flag == L"--web") plan.web = argv[++i];
                else if (flag == L"--port") g_port = _wtoi(argv[++i]);
                else if (flag == L"--server") explicit_server = argv[++i];
            }
            LocalFree(argv);
        }
    }

    const std::wstring dir = exe_dir();
    if (plan.root.empty()) {
        const std::wstring candidate = join(dir, L"radmusic-data");
        if (is_directory(candidate)) plan.root = candidate;
    }
    if (plan.web.empty()) {
        const std::wstring candidate = join(dir, L"web");
        if (is_directory(candidate)) plan.web = candidate;
    }

    // 把工作目录切到 %LOCALAPPDATA%\RadMusic。
    // 原因：WebView2 的渲染 / GPU 子进程会加载系统输入法（搜狗、QQ 拼音等），
    // 这些 DLL 习惯往「当前工作目录」写 debug.log —— 不切走的话发行目录里会凭空多出
    // 一份和本程序毫无关系的输入法日志。切走前先把命令行里的相对路径钉成绝对路径。
    plan.root = to_absolute(plan.root);
    plan.web = to_absolute(plan.web);
    explicit_server = to_absolute(explicit_server);
    {
        const std::wstring app_data = local_app_dir();
        if (!app_data.empty()) {
            const std::wstring work = join(app_data, L"RadMusic");
            CreateDirectoryW(work.c_str(), nullptr);
            if (SetCurrentDirectoryW(work.c_str())) {
                log_line(L"工作目录已切到 " + work);
            }
        }
    }

    // 已有服务在跑就复用（比如用户自己开了 start_radmusic.bat）
    log_line(L"exe 目录: " + dir + L"；目标端口 " + std::to_wstring(g_port));
    const bool already_running = port_alive(g_port, 400);
    bool spawned = false;
    if (already_running) {
        log_line(L"端口已有服务在跑，直接复用（退出时不关闭它）");
    } else {
        plan.server_exe = explicit_server.empty() ? pick_server_exe(dir) : explicit_server;
        if (plan.server_exe.empty()) {
            log_line(L"找不到 radmusic_server.exe");
            MessageBoxW(nullptr,
                        L"找不到 radmusic_server.exe。\n\n请把它和 radmusic_desktop.exe 放在同一目录，"
                        L"或用 --server <路径> 指定。",
                        kWindowTitle, MB_OK | MB_ICONERROR);
            CoUninitialize();
            return 1;
        }
        std::wstring error;
        spawned = spawn_server(plan, &error);
        if (!spawned) {
            // 可能刚好有别的实例在起（用户同时点了 bat 和这个 exe）：再探一次端口
            log_line(L"spawn_server 失败: " + error + L"；重探端口…");
            bool appeared = false;
            for (int i = 0; i < 30; ++i) {
                if (port_alive(g_port, 150)) {
                    appeared = true;
                    break;
                }
                Sleep(100);
            }
            if (!appeared) {
                MessageBoxW(nullptr, error.c_str(), kWindowTitle, MB_OK | MB_ICONERROR);
                CoUninitialize();
                return 1;
            }
            log_line(L"端口上有别的实例，改为复用");
        }
    }

    if (spawned) {
        log_line(L"已拉起服务端，等待端口就绪…");
        // 等端口就绪（首次扫库最多约 20 秒）；期间子进程一旦退出就立刻报错，不干等
        bool ready = false;
        DWORD exit_code = 0;
        bool died = false;
        for (int i = 0; i < 200; ++i) {
            if (port_alive(g_port, 120)) {
                ready = true;
                break;
            }
            if (server_exited(&exit_code)) {
                died = true;
                break;
            }
            Sleep(100);
        }
        if (!ready) {
            const std::wstring tail = tail_of_file(server_output_log(plan), 1500);
            const std::wstring detail = tail.empty() ? std::wstring(L"（服务端没有任何输出）") : tail;
            std::wstring text;
            if (died) {
                log_line(L"服务端启动后立即退出，exit=" + std::to_wstring(exit_code));
                text = L"服务端启动失败（退出码 " + std::to_wstring(exit_code) + L"）。\n\n服务端输出：\n" + detail;
            } else {
                log_line(L"服务端启动超时");
                text = L"服务端 20 秒内没有监听端口。\n\n服务端输出：\n" + detail;
            }
            MessageBoxW(nullptr, text.c_str(), kWindowTitle, MB_OK | MB_ICONERROR);
            shutdown_everything();
            CoUninitialize();
            return 1;
        }
        log_line(L"端口已就绪");
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(18, 18, 20));
    wc.lpszClassName = kWindowClass;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
    wc.hIconSm = wc.hIcon;
    if (RegisterClassExW(&wc) == 0) {
        CoUninitialize();
        return 1;
    }

    g_hwnd = CreateWindowExW(0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 1280, 840, nullptr, nullptr, instance, nullptr);
    if (g_hwnd == nullptr) {
        CoUninitialize();
        return 1;
    }

    if (!init_webview()) {
        log_line(L"init_webview 失败");
        DestroyWindow(g_hwnd);
        CoUninitialize();
        return 1;
    }
    log_line(L"WebView2 初始化请求已发出");

    ShowWindow(g_hwnd, show_cmd == 0 ? SW_SHOWDEFAULT : show_cmd);
    UpdateWindow(g_hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    shutdown_everything();
    CoUninitialize();
    return 0;
}
