// Rad music API - HTTP 服务端可执行文件（把各模块接线起来并提供真实 HTTP 接口）
//
// 用法：
//   radmusic_server [--root <曲库根目录>] [--host 127.0.0.1] [--port 8765]
//                   [--web <网页目录>] [--demo] [--io-timeout 5000]
//
// --web 指定网页 UI 目录后，直接用浏览器访问 http://host:port/ 即可打开界面
//       （不指定时会自动探测相对路径 web / ../web / ../../web）；
// --demo 会注册一个内置的演示播放源插件（无需网络，返回合成曲目），便于手工联调。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <unistd.h>   // fork / execv（自动重启）
#ifdef __APPLE__
#include <mach-o/dyld.h>   // _NSGetExecutablePath
#endif
#endif

#include "radmusic/api.hpp"
#include "radmusic/cache.hpp"
#include "radmusic/collections.hpp"
#include "radmusic/download.hpp"
#include "radmusic/engine.hpp"
#include "radmusic/groupkey.hpp"
#include "radmusic/http.hpp"
#include "radmusic/identify.hpp"
#include "radmusic/import.hpp"
#include "radmusic/log.hpp"
#include "radmusic/lyrics.hpp"
#include "radmusic/merge.hpp"
#include "radmusic/migrate.hpp"
#include "radmusic/model.hpp"
#include "radmusic/plugin.hpp"
#include "radmusic/remote_source.hpp"
#include "radmusic/settings.hpp"
#include "radmusic/storage.hpp"
#include "radmusic/update.hpp"
#include "radmusic/version.hpp"

namespace {

using namespace radmusic;

struct Options {
    std::string root = "radmusic-data";
    bool root_explicit = false;         // 是否通过 --root 显式指定（缺省时读 data_root.txt 配置）
    std::string host = "127.0.0.1";
    uint16_t port = 8765;
    int io_timeout_ms = 5000;
    bool demo = false;
    std::string web;  // 网页 UI 目录（空表示未指定，稍后自动探测）
    // v0.10 运行日志（Readme 1.14）
    std::string log_level = "info";
    std::string log_file;   // 空 = <root>/logs/radmusic.log
    bool log_console = true;
    bool log_memory = true;
    std::vector<std::string> plugin_dirs;   // 额外的外部插件目录（--plugin-dir，可重复）
    bool plugin_autostart = true;           // 自动拉起声明了 command 的插件进程
};

// 重启状态：被请求时把 true，poll 循环检测到就拉一份新进程再退出
std::atomic<bool> restart_requested{false};
// 关闭状态：被请求时置 true，poll 循环检测到就直接退出（不拉新进程）
std::atomic<bool> shutdown_requested{false};
std::string self_executable_path;   // 在 main() 里 GetModuleFileNameW / readlink
std::vector<std::string> self_argv;  // 保存启动参数（含可执行文件绝对路径）

// 内置默认数据根（与 start_radmusic.bat / start_server.py 的兜底值保持一致）
const std::string kDefaultDataRoot = "G:\\Rad music\\api\\radmusic-data";

// 数据根持久化：<exe 目录>/data_root.txt 存一行「当前选定的数据根」。
// 迁移完成后由服务端写入，启动脚本与（未显式传 --root 的）服务端启动时读取，实现换位置后持久生效。
std::filesystem::path data_root_config_path() {
    std::error_code ec;
    std::filesystem::path dir = self_executable_path.empty()
                                    ? std::filesystem::current_path(ec)
                                    : std::filesystem::path(self_executable_path).parent_path();
    if (dir.empty()) dir = std::filesystem::current_path(ec);
    return dir / "data_root.txt";
}

std::string read_data_root_config() {
    std::error_code ec;
    const std::filesystem::path cfg = data_root_config_path();
    if (!std::filesystem::exists(cfg, ec) || ec) return std::string();
    std::ifstream in(cfg, std::ios::binary);
    if (!in) return std::string();
    std::string line;
    std::getline(in, line);
    while (!line.empty() &&
           (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t')) {
        line.pop_back();
    }
    return line;
}

void write_data_root_config(const std::string& root) {
    const std::filesystem::path cfg = data_root_config_path();
    std::ofstream out(cfg, std::ios::binary | std::ios::trunc);
    if (out) out << root << "\n";
}

// 自动探测网页 UI 目录：取第一个含 index.html 的相对路径
std::string detect_web_root() {
    const char* candidates[] = {"web", "../web", "../../web"};
    for (const char* candidate : candidates) {
        std::error_code ec;
        const auto dir = std::filesystem::path(candidate);
        if (std::filesystem::exists(dir / "index.html", ec) && !ec) {
            std::error_code abs_ec;
            const auto absolute = std::filesystem::absolute(dir, abs_ec);
            return abs_ec ? dir.u8string() : absolute.u8string();
        }
    }
    return std::string();
}

Options parse_args(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "缺少 %s 的值\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--root") {
            options.root = next("--root");
            options.root_explicit = true;
        } else if (arg == "--host") {
            options.host = next("--host");
        } else if (arg == "--port") {
            options.port = static_cast<uint16_t>(std::atoi(next("--port").c_str()));
        } else if (arg == "--io-timeout") {
            options.io_timeout_ms = std::atoi(next("--io-timeout").c_str());
        } else if (arg == "--web") {
            options.web = next("--web");
        } else if (arg == "--demo") {
            options.demo = true;
        } else if (arg == "--log-level") {
            options.log_level = next("--log-level");
        } else if (arg == "--log-file") {
            options.log_file = next("--log-file");
        } else if (arg == "--no-log-console") {
            options.log_console = false;
        } else if (arg == "--plugin-dir") {
            options.plugin_dirs.push_back(next("--plugin-dir"));
        } else if (arg == "--no-plugin-autostart") {
            options.plugin_autostart = false;
        } else if (arg == "--help" || arg == "-h") {
            std::printf("用法: radmusic_server [--root <dir>] [--host IP] [--port N] [--web <dir>] [--demo]\n"
                        "                      [--plugin-dir <dir>] [--no-plugin-autostart]\n"
                        "                      [--io-timeout ms]\n"
                        "                      [--log-level trace|debug|info|warn|error|off]\n"
                        "                      [--log-file <path>] [--no-log-console]\n");
            std::exit(0);
        } else {
            std::fprintf(stderr, "未知参数: %s（--help 查看用法）\n", arg.c_str());
            std::exit(2);
        }
    }
    return options;
}

// 取当前可执行文件绝对路径（用于重启）
std::string current_executable_path(const char* argv0) {
#if defined(_WIN32)
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD got = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (got == 0) break;
        if (got < buf.size()) {
            buf.resize(got);
            break;
        }
        // 缓冲区不够，扩大重试
        buf.resize(buf.size() * 2);
    }
    if (buf.empty()) return argv0 ? argv0 : "";
    const int size = WideCharToMultiByte(CP_UTF8, 0, buf.c_str(), static_cast<int>(buf.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return argv0 ? argv0 : "";
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf.c_str(), static_cast<int>(buf.size()), out.data(), size, nullptr, nullptr);
    return out;
#elif defined(__APPLE__)
    // macOS：_NSGetExecutablePath（见 <mach-o/dyld.h>）
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size);
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return argv0 ? argv0 : "";
    std::error_code ec;
    auto abs = std::filesystem::weakly_canonical(std::filesystem::path(buf.data()), ec);
    return ec ? std::string(buf.data()) : abs.u8string();
#else
    // Linux：/proc/self/exe
    std::error_code ec;
    auto abs = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) return abs.u8string();
    return argv0 ? argv0 : "";
#endif
}

// 把保存的 argv 拼成命令行；包含引号转义，方便 CreateProcess 走 cmd 行解析
std::string build_command_line(const std::vector<std::string>& argv) {
    std::string line;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i > 0) line += ' ';
        const std::string& a = argv[i];
        const bool need_quote = a.find_first_of(" \t\"") != std::string::npos;
        if (!need_quote) {
            line += a;
        } else {
            line += '"';
            for (char ch : a) {
                if (ch == '"') line += "\\\"";
                else line += ch;
            }
            line += '"';
        }
    }
    return line;
}

// 异步拉起一份新的 radmusic_server 后退出当前进程；
// new_root 非空时用新数据根重启（迁移后切换位置用），否则沿用 saved_argv 里的参数；
// 若新进程拉起失败，保持旧进程继续运行（status 错误由回调返回）。
// 通过 Shell 打开 exe（等效于用户双击）：由 explorer 在独立会话中拉起，
// 彻底脱离当前进程可能存在的 Job（BREAKAWAY 被拒时 CreateProcess 拉起的新进程仍会被回收）。
static Status trigger_restart_shell(const std::vector<std::string>& argv) {
    if (argv.empty()) {
        return Status(ErrorCode::RequestFailed, "没有可执行文件路径，无法自动重启");
    }
    const std::string& exe_path = argv.front();
    std::string params;
    for (size_t i = 1; i < argv.size(); ++i) {
        if (!params.empty()) params += " ";
        if (argv[i].find(' ') != std::string::npos)
            params += "\"" + argv[i] + "\"";
        else
            params += argv[i];
    }
    std::wstring wexe(exe_path.begin(), exe_path.end());
    if (wexe.find(L' ') != std::wstring::npos && wexe[0] != L'"')
        wexe = L"\"" + wexe + L"\"";
    std::wstring wparams(params.begin(), params.end());
    HINSTANCE r = ShellExecuteW(nullptr, L"open", wexe.c_str(),
                                wparams.empty() ? nullptr : wparams.c_str(),
                                nullptr, SW_HIDE);
    if (reinterpret_cast<INT_PTR>(r) > 32) {
        restart_requested.store(true);
        return Status::success();
    }
    return Status(ErrorCode::RequestFailed, "Shell 拉起新进程失败（ShellExecuteW）");
}

Status trigger_restart(const std::vector<std::string>& saved_argv, const std::string& new_root = std::string()) {
    std::vector<std::string> argv = saved_argv;
    if (!new_root.empty()) {
        bool replaced = false;
        for (size_t i = 0; i + 1 < argv.size(); ++i) {
            if (argv[i] == "--root") {
                argv[i + 1] = new_root;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            argv.push_back("--root");
            argv.push_back(new_root);
        }
    }
    if (argv.empty()) {
        return Status(ErrorCode::RequestFailed, "没有可执行文件路径，无法自动重启");
    }
    const std::string line = build_command_line(argv);
#if defined(_WIN32)
    std::wstring wline(line.begin(), line.end());
    const DWORD base_flags = CREATE_NO_WINDOW | DETACHED_PROCESS;
    PROCESS_INFORMATION pi{};
    // 先尝试脱离父进程所在的 Job：被安装器 / 守护脚本 / 终端任务托管时，
    // 不脱离的话父进程一退出，新进程会被一起回收（表现为「点了重启就再也没起来」）。
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    BOOL ok = CreateProcessW(nullptr, wline.data(), nullptr, nullptr, FALSE,
                             base_flags | CREATE_BREAKAWAY_FROM_JOB, nullptr, nullptr, &si, &pi);
    if (!ok) {
        const DWORD le = GetLastError();
        if (le == ERROR_ACCESS_DENIED) {
            // 父进程在 Job 中且不允许脱离：普通 CreateProcess 拉起的新进程仍会被同一 Job
            // 回收，直接改用 Shell（= 双击 exe）让 explorer 在独立会话拉起，彻底脱离 Job。
            return trigger_restart_shell(argv);
        }
        // 其他错误（如路径问题）退回普通方式
        STARTUPINFOW si2{};
        si2.cb = sizeof(si2);
        PROCESS_INFORMATION pi2{};
        ok = CreateProcessW(nullptr, wline.data(), nullptr, nullptr, FALSE,
                            base_flags, nullptr, nullptr, &si2, &pi2);
        pi = pi2;
        if (!ok) {
            // 仍失败：最后再试 Shell 拉起
            return trigger_restart_shell(argv);
        }
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    restart_requested.store(true);
    return Status::success();
#elif defined(__APPLE__) || defined(__linux__)
    pid_t pid = fork();
    if (pid == 0) {
        // 子进程：立刻替换为新一份 radmusic_server；
        // argv[0] 是 exe 路径，剩下原样传过去
        std::vector<char*> args;
        for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        execv(argv.front().c_str(), args.data());
        std::exit(127);   // exec 失败
    }
    if (pid < 0) {
        return Status(ErrorCode::RequestFailed, "fork 失败");
    }
    restart_requested.store(true);
    return Status::success();
#else
    (void)line;
    return Status(ErrorCode::RequestFailed, "当前平台未实现自动重启");
#endif
}

// 关闭服务：置退出标志，poll 循环检测到后退出当前进程（不拉起新进程）。
// 与 trigger_restart 的区别：不 fork/CreateProcess，纯退出。
Status trigger_shutdown() {
    shutdown_requested.store(true);
    return Status::success();
}

// 内置演示播放源：无网络依赖，用于手工验证搜索/折叠/音质/歌词链路
class DemoSource : public ISourcePlugin {
public:
    explicit DemoSource(std::string platform) : platform_(std::move(platform)) {
        manifest_.id = platform_ + ".source";
        manifest_.name = platform_ + " 演示源";
        manifest_.type = PluginType::Source;
        manifest_.platforms = {platform_};
        manifest_.capabilities = {"search", "get_song", "get_url", "get_qualities", "get_lyric"};
        manifest_.limits.max_concurrency = 2;
        manifest_.limits.qps = 5;
        manifest_.permissions = {"network"};

        QualityLevelSpec plain;
        plain.id = "exhigh";
        plain.label = "极高";
        plain.spec.level = QualityLevel::High;
        plain.spec.codec = "mp3";
        plain.spec.lossless = false;
        plain.spec.bitrate = 320;
        plain.spec.quality_id = "exhigh";

        QualityLevelSpec lossless;
        lossless.id = "lossless";
        lossless.label = "无损";
        lossless.spec.level = QualityLevel::Lossless;
        lossless.spec.codec = "flac";
        lossless.spec.lossless = true;
        lossless.spec.bitrate = 1411;
        lossless.spec.sample_rate = 44100;
        lossless.spec.bit_depth = 16;
        lossless.spec.channels = 2;
        lossless.spec.quality_id = "lossless";

        manifest_.quality_levels = {plain, lossless};
    }

    const PluginManifest& manifest() const override { return manifest_; }
    std::vector<std::string> platforms() const override { return manifest_.platforms; }
    bool supports(std::string_view) const override { return true; }
    std::vector<std::string> ops() const override { return manifest_.capabilities; }
    std::vector<QualityLevelSpec> quality_levels() const override { return manifest_.quality_levels; }

    Result<BaseInfo> resolve(std::string_view item_id) override {
        BaseInfo base;
        base.platform = platform_;
        base.version = "1.0.0";
        base.item_id = std::string(item_id);
        base.sha = std::string(64, 'd');
        base.location = platform_ + ":" + std::string(item_id);
        base.name = "Demo Song";
        base.author = "Demo Artist";
        base.duration_seconds = 200;
        base.quality = manifest_.quality_levels[1].spec;
        return Result<BaseInfo>::success(base);
    }

    Result<SearchItem> search(std::string_view keyword, const json::Value&) override {
        SearchItem item;
        if (keyword.empty()) return Result<SearchItem>::success(item);
        item.group_key = group_key("Demo Song", "Demo Artist", 200);
        item.title = "Demo Song";
        item.artists = {"Demo Artist"};
        item.duration_seconds = 200;

        SourceEntry entry;
        entry.platform = platform_;
        entry.item_id = platform_ + ":demo-1";
        entry.available = true;
        entry.quality = manifest_.quality_levels[1].spec;
        entry.qualities = {manifest_.quality_levels[0].spec, manifest_.quality_levels[1].spec};
        entry.base = resolve(entry.item_id).value();
        item.sources = {entry};
        return Result<SearchItem>::success(item);
    }

    Result<std::string> open(std::string_view) override {
        return Result<std::string>::failure(ErrorCode::FileNotFound, "演示源不提供真实音频流");
    }

    Result<json::Value> request(std::string_view op, const json::Value& params) override {
        if (op == "get_lyric") {
            json::Value value = json::Value::object();
            value.set("lrc", std::string("[00:01.00]demo line one\n[00:06.00]demo line two\n"));
            return Result<json::Value>::success(value);
        }
        if (op == "get_qualities") {
            json::Value levels = json::Value::array();
            for (const auto& spec : manifest_.quality_levels) levels.push_back(spec.to_json());
            return Result<json::Value>::success(levels);
        }
        if (op == "get_url") {
            json::Value value = json::Value::object();
            value.set("url", std::string());
            value.set("quality", manifest_.quality_levels[1].spec.to_json());
            value.set("note", std::string("演示源没有真实地址，请安装真实播放源插件"));
            return Result<json::Value>::success(value);
        }
        if (op == "get_song") {
            const std::string id = params.is_object() ? params.get_string("id", "demo-1") : "demo-1";
            return Result<json::Value>::success(resolve(id).value().to_json());
        }
        return Result<json::Value>::failure(ErrorCode::UnsupportedCapability, "演示源未实现该 op");
    }

    Result<std::vector<Quality>> qualities(std::string_view) override {
        std::vector<Quality> out;
        for (const auto& spec : manifest_.quality_levels) out.push_back(spec.spec);
        return Result<std::vector<Quality>>::success(out);
    }

private:
    std::string platform_;
    PluginManifest manifest_;
};

void print_banner(const Options& options, const LibraryPaths& paths, const PluginRegistry& plugins,
                  const std::string& address, const std::string& web_root, const std::string& log_file) {
    std::printf("Rad music API 服务已启动\n");
    std::printf("  监听地址 : http://%s\n", address.c_str());
    if (!web_root.empty()) {
        std::printf("  网页界面 : http://%s/  （浏览器打开即可）\n", address.c_str());
        std::printf("  网页目录 : %s\n", web_root.c_str());
    } else {
        std::printf("  网页界面 : 未启用（用 --web <目录> 指定含 index.html 的网页目录）\n");
    }
    std::printf("  曲库根   : %s\n", paths.root.u8string().c_str());
    std::printf("  日志级别 : %s\n", options.log_level.c_str());
    if (!log_file.empty()) std::printf("  日志文件 : %s\n", log_file.c_str());
    std::printf("  插件     : %zu 个\n", plugins.manifests().size());
    for (const auto& manifest : plugins.manifests()) {
        std::printf("             - %s (%s, %s)\n", manifest.id.c_str(), plugin_type_id(manifest.type),
                    manifest.platforms.empty() ? "-" : manifest.platforms.front().c_str());
    }
    if (options.demo) std::printf("  演示源   : 已启用（--demo）\n");
    std::printf("\n常用接口：\n");
    std::printf("  GET  /music/status\n");
    std::printf("  POST /music/search        {\"keyword\":\"demo\"}\n");
    std::printf("  POST /music/request       {\"source\":\"demo\",\"op\":\"get_lyric\",\"params\":{}}\n");
    std::printf("  GET  /music/quality\n");
    std::printf("  GET  /music/cache\n");
    std::printf("  GET  /music/download\n");
    std::printf("  POST /music/import/scan   {\"paths\":[\"<目录>\"]}\n");
    std::printf("  GET  /music/eq\n");
    std::printf("  GET  /music/playlist      POST /music/playlist {\"name\":\"我的收藏\"}\n");
    std::printf("  GET  /music/album         POST /music/album {\"name\":\"专辑\",\"artist\":\"歌手\"}\n");
    std::printf("  GET  /music/mv            POST /music/mv {\"sha\":\"<64hex>\",\"title\":\"MV\"}\n");
    std::printf("  GET  /music/logs?lines=50\n");
    std::printf("\n按 Ctrl+C 结束。\n\n");
    std::fflush(stdout);
}

// 启动时加载声明式插件清单：<root>/plugins/<id>/plugin.json（设置页「添加插件」写入的位置）
void load_declarative_plugins(const LibraryPaths& paths, PluginRegistry& plugins, const SettingsStore& settings) {
    std::error_code ec;
    const std::filesystem::path dir = paths.plugins();
    if (!std::filesystem::exists(dir, ec) || ec) return;
    int loaded = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_directory()) continue;
        const std::filesystem::path manifest_file = entry.path() / "plugin.json";
        if (!std::filesystem::exists(manifest_file, ec) || ec) continue;
        auto text = read_text_file(manifest_file);
        if (!text.ok()) {
            RM_LOG_WARN("plugins", "读取 " + manifest_file.u8string() + " 失败");
            continue;
        }
        json::Value parsed = json::Value::parse(text.value());
        if (parsed.is_null()) {
            RM_LOG_WARN("plugins", manifest_file.u8string() + " 不是合法 JSON");
            continue;
        }
        PluginManifest manifest = PluginManifest::from_json(parsed);
        auto validated = manifest.validate();
        if (!validated.ok()) {
            RM_LOG_WARN("plugins", "插件清单校验失败 " + manifest.id + ": " + validated.message);
            continue;
        }
        auto added = plugins.add_manifest(manifest);
        if (!added.ok()) {
            RM_LOG_WARN("plugins", "插件注册失败 " + manifest.id + ": " + added.message);
            continue;
        }
        if (!settings.plugin_enabled(manifest.id, true)) plugins.set_enabled(manifest.id, false);
        ++loaded;
    }
    if (loaded > 0) RM_LOG_INFO("plugins", "已加载声明式插件 " + std::to_string(loaded) + " 个");
}

// 外部插件目录：<root>/plugins + --plugin-dir + 常见相对目录（源码仓库里的 plugins/）
std::vector<std::filesystem::path> plugin_search_dirs(const Options& options, const LibraryPaths& paths) {
    std::vector<std::filesystem::path> dirs;
    dirs.push_back(paths.plugins());
    for (const std::string& dir : options.plugin_dirs) dirs.emplace_back(dir);
    const char* candidates[] = {"plugins", "../plugins", "../../plugins"};
    for (const char* candidate : candidates) dirs.emplace_back(candidate);
    return dirs;
}

// 外部进程播放源（Readme 5.5）：plugin.json 里 transport=http 的插件，由宿主用 HTTP JSON-RPC 驱动。
// 若声明了 command 且尚未运行，会尝试自动拉起插件进程（--no-plugin-autostart 可关闭）。
int load_remote_plugins(const Options& options, const LibraryPaths& paths, PluginRegistry& plugins,
                        const SettingsStore& settings) {
    int loaded = 0;
    for (const std::filesystem::path& dir : plugin_search_dirs(options, paths)) {
        std::error_code ec;
        if (!std::filesystem::exists(dir, ec) || ec) continue;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (ec) break;
            if (!entry.is_directory()) continue;
            const std::filesystem::path manifest_file = entry.path() / "plugin.json";
            if (!std::filesystem::exists(manifest_file, ec) || ec) continue;
            auto text = read_text_file(manifest_file);
            if (!text.ok()) continue;
            json::Value parsed = json::Value::parse(text.value());
            if (parsed.is_null()) continue;
            PluginManifest manifest = PluginManifest::from_json(parsed);
            // 功能插件（type=function，无 transport）：作为清单注册，可供宿主提供面板 / 设置
            if (manifest.type == PluginType::Function) {
                auto validated = manifest.validate();
                if (!validated.ok()) {
                    RM_LOG_WARN("plugins", "功能插件清单校验失败 " + manifest.id + ": " + validated.message);
                    continue;
                }
                if (plugins.has_implementation(manifest.id)) continue;  // 更高优先级目录已加载
                plugins.add_manifest(manifest);
                if (!settings.plugin_enabled(manifest.id, true)) plugins.set_enabled(manifest.id, false);
                ++loaded;
                continue;
            }
            if (manifest.transport != "http") continue;
            if (manifest.endpoint.empty()) {
                RM_LOG_WARN("plugins", "外部插件 " + manifest.id + " 缺少 endpoint");
                continue;
            }
            // 同一插件 id 已被前面的目录加载（更高优先级）则跳过
            if (plugins.has_implementation(manifest.id)) continue;
            auto validated = manifest.validate();
            if (!validated.ok()) {
                RM_LOG_WARN("plugins", "外部插件清单校验失败 " + manifest.id + ": " + validated.message);
                continue;
            }

            RemoteSourceOptions remote;
            remote.id = manifest.id;
            remote.name = manifest.name;
            remote.version = manifest.version;
            remote.platforms = manifest.platforms;
            remote.capabilities = manifest.capabilities;
            remote.quality_levels = manifest.quality_levels;
            remote.endpoint = manifest.endpoint;
            remote.settings_schema = manifest.settings_schema;

            const bool already_up = remote_source_ping(remote.endpoint, 600);
            bool started = false;
            if (!already_up && options.plugin_autostart && !manifest.command.empty()) {
                const std::string cwd = entry.path().u8string();
                started = spawn_plugin_process(manifest.command, cwd);
                for (int attempt = 0; started && attempt < 50; ++attempt) {
                    if (remote_source_ping(remote.endpoint, 500)) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
            }
            if (!remote_source_ping(remote.endpoint, 800)) {
                RM_LOG_WARN("plugins", "外部插件 " + manifest.id + " 未就绪（" + manifest.endpoint +
                                           "），已注册但请求会失败；请手动启动插件进程");
            } else {
                RM_LOG_INFO("plugins", "外部播放源 " + manifest.id + " 已就绪（" + manifest.endpoint + "）");
            }

            auto plugin = make_remote_source(std::move(remote));
            if (!plugin) {
                RM_LOG_WARN("plugins", "外部插件 " + manifest.id + " 没有声明 platforms");
                continue;
            }
            plugins.add_source(plugin);
            if (!settings.plugin_enabled(manifest.id, true)) plugins.set_enabled(manifest.id, false);
            ++loaded;
        }
    }
    if (loaded > 0) RM_LOG_INFO("plugins", "已加载外部播放源插件 " + std::to_string(loaded) + " 个");
    return loaded;
}

}  // namespace

int main(int argc, char** argv) {
    // 记录启动参数 → 重启时复用（不会丢 --root / --port / --plugin-dir 等）
    self_argv.clear();
    self_argv.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) self_argv.emplace_back(argv[i]);
    if (!self_argv.empty()) {
        self_executable_path = current_executable_path(self_argv.front().c_str());
        if (!self_executable_path.empty() && self_argv.front() != self_executable_path) {
            self_argv[0] = self_executable_path;
        }
    }

    Options options = parse_args(argc, argv);
    // 未显式传 --root 时，读取持久化的数据根配置（迁移后切换位置用）；没有则用默认
    if (!options.root_explicit) {
        const std::string configured = read_data_root_config();
        if (!configured.empty()) options.root = configured;
    }

    LibraryPaths paths(options.root);
    const Status layout = paths.ensure_layout();
    if (!layout.ok()) {
        std::fprintf(stderr, "初始化曲库目录失败: %s\n", layout.message.c_str());
        return 1;
    }

    // 设置页持久化（<root>/settings.json）：主题 / 音质偏好 / 视频源 / 插件启用状态
    SettingsStore settings_store(paths);
    settings_store.load();

    PluginRegistry plugins;
    if (options.demo) {
        plugins.add_source(std::make_shared<DemoSource>("demo"));
    }
    load_declarative_plugins(paths, plugins, settings_store);
    load_remote_plugins(options, paths, plugins, settings_store);

    CoreEngine engine(paths, plugins);

    // 在线源接线（1.3 规则 5 / 1.4 / 1.5）：
    //   rebuild_base → 插件 get_song 取回元数据并落 base.json
    //   open_online  → 插件 get_url 取真实播放地址（支持所选音质）
    //   lyric_provider → 插件 get_lyric 取歌词
    engine.rebuild_base = [&plugins](std::string_view key) -> Result<BaseInfo> {
        const std::string k(key);
        const std::string prefix = "online:";
        if (k.rfind(prefix, 0) != 0) {
            return Result<BaseInfo>::failure(ErrorCode::BaseRebuildFailed, "无法重建在线元数据: " + k);
        }
        const size_t colon = k.find(':', prefix.size());
        if (colon == std::string::npos) {
            return Result<BaseInfo>::failure(ErrorCode::InvalidMetadata, "在线曲目键格式错误: " + k);
        }
        const std::string platform = k.substr(prefix.size(), colon - prefix.size());
        const std::string item_id = k.substr(colon + 1);
        if (platform.empty() || item_id.empty()) {
            return Result<BaseInfo>::failure(ErrorCode::InvalidMetadata, "在线曲目键缺少 platform/id: " + k);
        }
        auto plugin = plugins.source_for_platform(platform);
        if (!plugin) {
            return Result<BaseInfo>::failure(ErrorCode::PlatformError, "无可用插件适配 platform: " + platform);
        }
        Status capable = plugins.check_capability(platform, "get_song");
        if (!capable.ok()) return Result<BaseInfo>(capable);

        json::Value params = json::Value::object();
        params.set("id", item_id);
        auto result = plugin->request("get_song", params);
        if (!result.ok()) return Result<BaseInfo>(result.status());
        json::Value song = result.value();
        if (song.is_object() && song.find("song") && song.get("song").is_object()) song = song.get("song");
        if (!song.is_object()) {
            return Result<BaseInfo>::failure(ErrorCode::PlatformError, "插件 get_song 返回格式不正确");
        }
        BaseInfo base = BaseInfo::from_json(song);
        if (base.platform.empty()) base.platform = platform;
        if (base.item_id.empty()) base.item_id = item_id;
        // 引擎只在 location 是 http(s) 时才走 open_online；在线源的初始 base 往往没有地址，
        // 这里放一个占位 URL，真正地址由 get_url 在 open_online 里取回。
        if (base.location.rfind("http://", 0) != 0 && base.location.rfind("https://", 0) != 0) {
            base.location = "http://plugin.invalid/" + platform + "/" + item_id;
        }
        return Result<BaseInfo>::success(base);
    };

    engine.open_online = [&plugins](const BaseInfo& base) -> Result<std::string> {
        auto plugin = plugins.source_for_platform(base.platform);
        if (!plugin) {
            return Result<std::string>::failure(ErrorCode::PlatformError, "无可用插件适配 platform: " + base.platform);
        }
        Status capable = plugins.check_capability(base.platform, "get_url");
        if (!capable.ok()) return Result<std::string>(capable);
        json::Value params = json::Value::object();
        params.set("id", base.item_id.empty() ? base.sha : base.item_id);
        if (!base.quality.quality_id.empty()) params.set("quality", base.quality.quality_id);
        auto result = plugin->request("get_url", params);
        if (!result.ok()) return Result<std::string>(result.status());
        std::string url;
        if (result.value().is_string()) {
            url = result.value().as_string();
        } else if (result.value().is_object()) {
            url = result.value().get_string("url", result.value().get_string("location"));
        }
        if (url.empty()) {
            return Result<std::string>::failure(ErrorCode::PlatformError,
                                                "插件未能提供播放地址（platform=" + base.platform + "）");
        }
        return Result<std::string>::success(url);
    };

    engine.lyric_provider = [&plugins](const BaseInfo& base) -> Result<Lyric> {
        auto plugin = plugins.source_for_platform(base.platform);
        if (!plugin) return Result<Lyric>::failure(ErrorCode::LyricUnavailable, "无可用插件");
        json::Value params = json::Value::object();
        params.set("id", base.item_id.empty() ? base.sha : base.item_id);
        auto result = plugin->request("get_lyric", params);
        if (!result.ok()) return Result<Lyric>(result.status());
        const json::Value& value = result.value();
        if (value.is_string()) return parse_lrc(value.as_string());
        if (value.is_object()) {
            // 优先用结构化 lines（带翻译 tr / 罗马音 roma），其次退回纯 LRC 文本
            if (value.find("lines") && value.get("lines").is_array()) {
                return Result<Lyric>::success(Lyric::from_json(value));
            }
            if (value.find("lyric") && value.get("lyric").is_string()) return parse_lrc(value.get("lyric").as_string());
            if (value.find("lrc") && value.get("lrc").is_string()) return parse_lrc(value.get("lrc").as_string());
        }
        return Result<Lyric>::failure(ErrorCode::LyricUnavailable, "插件未返回可用歌词");
    };
    MetaCache meta(paths);
    LibraryIndex index(paths);
    index.load();

    // 自定义合并（曲库里被识别错误拆开的曲目归一到同一首歌）
    MergeStore merges(paths);
    if (auto loaded = merges.load(); !loaded.ok()) {
        std::fprintf(stderr, "合并记录加载失败: %s\n", loaded.message.c_str());
    }
    meta.set_merge_store(&merges);

    // v0.10+：本地曲目联网识别（导入后匹配在线版本与海报）
    IdentifyService identify(paths, meta, index, plugins);

    CacheManager cache(paths, meta, index);
    DownloadManager downloads(paths, meta, index, plugins);
    downloads.load();   // 恢复上次未完成的任务（重启后置为 paused，可 resume 续传）
    ImportManager importer(paths, meta, index);
    // v0.10：歌单 / 专辑 / MV
    PlaylistStore playlists(paths);
    AlbumStore albums(paths);
    MvStore mvs(paths);
    playlists.ensure_layout();
    albums.ensure_layout();
    mvs.ensure_layout();

    // v0.10：运行日志（1.14）——默认 info 级、写 <root>/logs/radmusic.log，并保留内存缓冲
    const std::string log_file =
        options.log_file.empty() ? (paths.logs() / "radmusic.log").u8string() : options.log_file;
    LogConfig log_config;
    log_config.level = log_level_from_id(options.log_level);
    log_config.console = options.log_console;
    log_config.memory = options.log_memory;
    log_config.file_path = log_file;
    const Status logging = log_configure(log_config);
    if (!logging.ok()) {
        std::fprintf(stderr, "日志初始化失败: %s\n", logging.message.c_str());
    }
    RM_LOG_INFO("server", "服务启动中：root=" + paths.root.u8string() + " port=" + std::to_string(options.port));

    ApiDispatcher dispatcher(paths, plugins, engine, meta, index);
    dispatcher.set_cache(&cache);
    dispatcher.set_downloads(&downloads);
    dispatcher.set_importer(&importer);
    dispatcher.set_identifier(&identify);
    dispatcher.set_playlists(&playlists);
    dispatcher.set_albums(&albums);
    dispatcher.set_mvs(&mvs);
    dispatcher.set_settings(&settings_store);
    dispatcher.set_merge(&merges);
    dispatcher.set_source_priority({"radish", "netease", "demo"});
    {
        std::vector<std::filesystem::path> plugin_dirs;
        for (const std::string& d : options.plugin_dirs) plugin_dirs.emplace_back(d);
        dispatcher.set_plugin_dirs(std::move(plugin_dirs));
    }
    // 检查更新：下载包与替换脚本都放在可执行文件同级的 updates/ 下
    // （Windows 的替换脚本要在本进程退出后覆盖安装目录，路径必须跟着程序走，而不是数据根）
    const std::filesystem::path install_dir = self_executable_path.empty()
                                                  ? std::filesystem::path()
                                                  : std::filesystem::path(self_executable_path).parent_path();
    UpdateService updater(install_dir.empty() ? paths.root / "updates" : install_dir / "updates",
                          version_string(), update_repo(), UpdateService::detect_platform_slug());
    updater.set_install_dir(install_dir);
    updater.set_launch_argv(self_argv);
    updater.set_threads(settings_store.values().update_threads);   // 设置页「下载线程数」
    updater.restart_handler = []() { return trigger_restart(self_argv); };
    updater.request_exit = []() { restart_requested.store(true); };
    dispatcher.set_update(&updater);
    // 数据迁移：复制完成后写 data_root.txt 并用新数据根自重启（旧数据保留，不删）
    DataMigrator migrator(paths);
    migrator.on_done = [](const MigrationStatus& ms) {
        if (ms.state == MigrateState::Done) {
            write_data_root_config(ms.dest);
            RM_LOG_WARN("data", "数据已迁移到 " + ms.dest + "（" + std::to_string(ms.files.size()) +
                                    " 个文件，跳过 " + std::to_string(ms.failed_files.size()) +
                                    " 个），写入 data_root.txt 并用新数据根重启…");
            trigger_restart(self_argv, ms.dest);
        } else if (ms.state == MigrateState::Failed) {
            RM_LOG_ERROR("data", "数据迁移失败: " + ms.error + "（保留原数据目录，未切换）");
        } else if (ms.state == MigrateState::Canceled) {
            RM_LOG_WARN("data", "数据迁移已取消（保留原数据目录，未切换）");
        }
    };
    dispatcher.set_migrator(&migrator);
    dispatcher.set_data_location(data_root_config_path().u8string(), kDefaultDataRoot);
    dispatcher.set_restart_callback([](const std::string& new_root) {
        return trigger_restart(self_argv, new_root);
    });
    dispatcher.set_shutdown_callback([]() {
        return trigger_shutdown();
    });

    HttpServer server(options.host, options.port, dispatcher);
    server.set_io_timeout_ms(options.io_timeout_ms);
    const std::string web_root = options.web.empty() ? detect_web_root() : options.web;
    if (!web_root.empty()) server.set_static_root(web_root);

    // 下载队列驱动（单线程模型）：主循环每次空闲时推进一个排队任务，
    // 否则任务只会停在 queued 永不执行（前端表现为“下载无法进行”）。
    downloads.on_done = [&downloads](const DownloadTask&) { downloads.save(); };
    downloads.on_failed = [&downloads](const DownloadTask&) { downloads.save(); };
    server.set_idle_hook(
        [&downloads, &migrator, &updater, &server]() {
            if (downloads.step()) downloads.save();
            if (migrator.active()) migrator.step();   // 数据迁移分片推进（复制并保留旧数据）
            updater.step();                            // 检查更新 / 分片下载更新包
            if (restart_requested.load()) {
                RM_LOG_WARN("server", "收到重启请求，正在退出当前进程...");
                server.stop();
            }
            if (shutdown_requested.load()) {
                RM_LOG_WARN("server", "收到关闭请求，正在退出当前进程...");
                server.stop();
            }
        },
        300);

    const Status started = server.start();
    if (!started.ok()) {
        std::fprintf(stderr, "启动失败: %s\n", started.message.c_str());
        return 1;
    }

    print_banner(options, paths, plugins, server.bound_address(), web_root, log_file);
    server.serve_forever(500);
    // 收到重启请求时 sleep 一下，确保新进程已起 / 端口已被它接住
    if (restart_requested.load()) std::this_thread::sleep_for(std::chrono::milliseconds(800));
    std::printf("\n已停止（共处理 %zu 个请求）\n", server.served_requests());
    return 0;
}
