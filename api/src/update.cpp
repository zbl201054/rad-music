// Rad music API - 检查更新实现（GitHub Releases）
#include "radmusic/update.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string_view>

#include "radmusic/archive.hpp"
#include "radmusic/httpclient.hpp"
#include "radmusic/json.hpp"
#include "radmusic/log.hpp"
#include "radmusic/version.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace radmusic {
namespace {

// 每片下载量：4MB 一片，够快又不会长时间占住主循环
constexpr long long kChunkBytes = 4 * 1024 * 1024;

// 下载完成后到自动安装的倒计时（秒）：给用户留出取消或改为手动安装的时间
constexpr long long kAutoApplyDelaySec = 10;

std::string path_to_utf8(const std::filesystem::path& p) {
    const auto u8 = p.u8string();
    return u8.empty() ? std::string() : std::string(reinterpret_cast<const char*>(u8.c_str()));
}

std::string now_iso8601() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char buffer[32] = {0};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &local);
    return buffer;
}

const char* executable_name() {
#if defined(_WIN32)
    return "radmusic_server.exe";
#else
    return "radmusic_server";
#endif
}

bool file_exists(const std::filesystem::path& path) {
    std::error_code code;
    return std::filesystem::is_regular_file(path, code);
}

long long file_size_or_zero(const std::filesystem::path& path) {
    std::error_code code;
    const auto size = std::filesystem::file_size(path, code);
    return code ? 0 : static_cast<long long>(size);
}

// 把参数拼成命令行（含引号转义）；供 Windows 替换脚本拉起新进程复用
std::string build_command_line(const std::vector<std::string>& argv) {
    std::string line;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i > 0) line += ' ';
        const std::string& a = argv[i];
        const bool need_quote = a.find_first_of(" \t\"") != std::string::npos;
        if (!need_quote) {
            line += a;
            continue;
        }
        line += '"';
        for (char ch : a) {
            if (ch == '"') line += "\\\"";
            else line += ch;
        }
        line += '"';
    }
    return line;
}

bool write_text(const std::filesystem::path& path, const std::string& text) {
    std::error_code code;
    std::filesystem::create_directories(path.parent_path(), code);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.flush();
    return out.good();
}

// 带 UTF-8 BOM 的写入：给 .ps1 用。Windows PowerShell 5.1 读无 BOM 的 .ps1 时会按系统 ANSI 解码，
// 脚本里只要出现一个中文，引号就会被拆坏（报「字符串缺少终止符」），整个更新脚本直接跑不起来。
bool write_text_bom(const std::filesystem::path& path, const std::string& text) {
    std::error_code code;
    std::filesystem::create_directories(path.parent_path(), code);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write("\xEF\xBB\xBF", 3);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.flush();
    return out.good();
}

// ---------------------------------------------------------------- 替换脚本的启动与会话
//
// 背景（实测踩过的坑）：apply() 会拉起一个 PowerShell 脚本，等本进程退出后再覆盖 exe 并重启。
// 但如果本进程处在「带 KILL_ON_JOB_CLOSE 的 Job」里（安装器、IDE 终端、某些启动器都会这样），
// 子进程会随父进程一起被杀，脚本刚写下 `start waitPid=…` 就没了 —— 表现为点了安装更新之后
// 服务没了、版本没变、stage 目录还在。
//
// 所以启动分三级：
//   1) CREATE_BREAKAWAY_FROM_JOB：作业允许 breakaway 时直接跳出（最干净）；
//   2) WMI/CIM：让 WmiPrvSE 当父进程创建（任何 job 都管不到它），只需要系统自带的 PowerShell；
//   3) 普通 CreateProcess：最坏情况，靠下面「下次启动接着装」的标记兜底。
constexpr const char* kApplyMarkerName = "apply.pending";
constexpr int kApplyMaxResumeAttempts = 2;   // 自动重试上限，超过就只提示，不反复重启

std::filesystem::path apply_marker_path(const std::filesystem::path& updates_dir) {
    return updates_dir / kApplyMarkerName;
}

// 标记文件：{ version, attempts, at }。存在 = 有一次 apply 没走完
void write_apply_marker(const std::filesystem::path& updates_dir, const std::string& version, int attempts) {
    json::Value marker = json::Value::object();
    marker.set("version", version);
    marker.set("attempts", static_cast<long long>(attempts));
    marker.set("at", static_cast<long long>(std::time(nullptr)));
    write_text(apply_marker_path(updates_dir), marker.dump(2));
}

int read_apply_attempts(const std::filesystem::path& updates_dir, std::string* version = nullptr) {
    std::ifstream in(apply_marker_path(updates_dir), std::ios::binary);
    if (!in) return -1;
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.empty()) return 0;
    json::ParseError parse_error;
    const json::Value marker = json::Value::parse(text, &parse_error);
    if (!marker.is_object()) return 0;
    if (version != nullptr) *version = marker.get_string("version");
    const long long attempts = marker.get_int("attempts", 0);
    return attempts < 0 ? 0 : static_cast<int>(attempts);
}

void clear_apply_marker(const std::filesystem::path& updates_dir) {
    std::error_code code;
    std::filesystem::remove(apply_marker_path(updates_dir), code);
}

// PowerShell 单引号字符串里，字面量单引号要写两遍
std::string ps_single_quoted(std::string_view text) {
    std::string out = "'";
    for (char ch : text) {
        if (ch == '\'') out += "''";
        else out += ch;
    }
    out += "'";
    return out;
}

std::wstring widen_utf8(const std::string& text) {
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return std::wstring();
    std::wstring wide(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

bool spawn_windows(const std::string& command, DWORD flags, unsigned long* pid = nullptr) {
    std::wstring wide = widen_utf8(command);
    if (wide.empty()) return false;
    wide.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, wide.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    if (pid != nullptr) *pid = pi.dwProcessId;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// 让 WMI 供应商（WmiPrvSE.exe）替我们创建进程：调用方的 job 管不到它的子进程
bool spawn_via_cim(const std::string& command) {
    const std::string inner =
        "powershell.exe -NoProfile -NonInteractive -WindowStyle Hidden -Command "
        "\"Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{CommandLine=" +
        ps_single_quoted(command) + "} | Out-Null\"";
    return spawn_windows(inner, CREATE_NO_WINDOW);
}

enum class LaunchPath {
    Breakaway,   // 直接跳出 job
    Wmi,         // 由 WMI 供应商代劳
    Plain,       // 普通父子进程（可能随父进程被清理）
};

const char* launch_path_name(LaunchPath path) {
    switch (path) {
        case LaunchPath::Breakaway: return "breakaway";
        case LaunchPath::Wmi: return "wmi";
        default: return "plain";
    }
}

bool launch_replacer(const std::string& command, LaunchPath* used) {
    // 1) 跳出调用方的 job
    if (spawn_windows(command, CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB)) {
        if (used != nullptr) *used = LaunchPath::Breakaway;
        return true;
    }
    const DWORD first_error = GetLastError();
    // 2) 交给 WMI
    if (spawn_via_cim(command)) {
        if (used != nullptr) *used = LaunchPath::Wmi;
        return true;
    }
    const DWORD cim_error = GetLastError();
    // 3) 兜底：普通方式（下次启动会靠 apply.pending 接着装）
    if (spawn_windows(command, CREATE_NO_WINDOW)) {
        if (used != nullptr) *used = LaunchPath::Plain;
        return true;
    }
    RM_LOG_ERROR("update", "三种启动方式都失败：breakaway=" + std::to_string(first_error) +
                               " wmi=" + std::to_string(cim_error) +
                               " plain=" + std::to_string(GetLastError()));
    return false;
}

// Windows：等旧进程退出 → 备份 exe → 覆盖 → 用原参数拉起 → 自清理
// 全程写 updates/apply_update.log，更新失败时用户可以直接查这个文件
std::string windows_replacer_script(unsigned long wait_pid, bool has_args) {
    std::ostringstream script;
    script << "$ErrorActionPreference = 'SilentlyContinue'\n"
           << "$updates = Split-Path -Parent $MyInvocation.MyCommand.Path\n"
           << "$install = Split-Path -Parent $updates\n"
           << "$stage = Join-Path $updates 'stage'\n"
           << "$exe = Join-Path $install 'radmusic_server.exe'\n"
           << "$marker = Join-Path $updates 'apply.pending'\n"
           << "$log = Join-Path $updates 'apply_update.log'\n"
           << "function Log([string]$m) { \"$((Get-Date).ToString('s')) $m\" | Out-File -FilePath $log "
              "-Append -Encoding UTF8 }\n"
           << "$waitPid = " << wait_pid << "\n"
           << "Log \"start waitPid=$waitPid install=$install\"\n"
           << "$deadline = (Get-Date).AddSeconds(90)\n"
           << "while ((Get-Process -Id $waitPid -ErrorAction SilentlyContinue) -and ((Get-Date) -lt $deadline)) {\n"
           << "  Start-Sleep -Milliseconds 400\n"
           << "}\n"
           // 旧进程还没退：绝不覆盖、也绝不重启第二个 —— 两个实例抢同一个端口比更新失败更糟
           << "if (Get-Process -Id $waitPid -ErrorAction SilentlyContinue) {\n"
           << "  Log \"abort: old process $waitPid still alive after 90s (skip copy, avoid double instance)\"\n"
           << "  exit 1\n"
           << "}\n"
           << "Start-Sleep -Milliseconds 600\n"
           << "if (-not (Test-Path $stage)) { Log 'abort: stage dir missing'; exit 1 }\n"
           << "if (Test-Path $exe) { Copy-Item $exe ($exe + '.bak') -Force; Log 'backup written' }\n"
           // 先单独换 exe：换不上就别动别的文件，留着 stage 下次启动接着装
           << "Copy-Item (Join-Path $stage 'radmusic_server.exe') $exe -Force\n"
           << "if (-not (Test-Path $exe)) { Log 'abort: replace exe failed (file locked?)'; exit 1 }\n"
           << "$len = (Get-Item $exe).Length\n"
           << "$want = (Get-Item (Join-Path $stage 'radmusic_server.exe')).Length\n"
           << "if ($len -ne $want) { Log \"abort: exe size mismatch ($len / $want)\"; exit 1 }\n"
           << "Get-ChildItem -Path $stage -Force | Where-Object { $_.Name -ne 'radmusic_server.exe' } | "
              "ForEach-Object {\n"
           << "  Copy-Item -Path $_.FullName -Destination $install -Recurse -Force\n"
           << "}\n"
           << "Log 'files copied'\n";
    if (has_args) {
        script << "$argfile = Join-Path $updates 'relaunch.args'\n"
               << "$argline = ''\n"
               << "if (Test-Path $argfile) { $argline = (Get-Content -Path $argfile -Raw -Encoding UTF8) }\n"
               << "if (Test-Path $exe) {\n"
               << "  Start-Process -FilePath $exe -ArgumentList $argline -WorkingDirectory $install\n"
               << "  Log \"relaunched args=$argline\"\n"
               << "}\n";
    } else {
        script << "if (Test-Path $exe) {\n"
               << "  Start-Process -FilePath $exe -WorkingDirectory $install\n"
               << "  Log 'relaunched without args'\n"
               << "}\n";
    }
    script << "Start-Sleep -Seconds 2\n"
           << "Remove-Item $stage -Recurse -Force\n"
           << "Remove-Item (Join-Path $updates 'relaunch.args') -Force\n"
           << "Remove-Item $marker -Force\n"
           << "Log 'done'\n"
           << "Remove-Item $MyInvocation.MyCommand.Path -Force\n";
    return script.str();
}

}  // namespace

// ---------------------------------------------------------------- 版本比较

bool parse_version(const char* text, int out[3]) {
    out[0] = 0;
    out[1] = 0;
    out[2] = 0;
    if (text == nullptr) return false;
    const std::string s(text);
    size_t i = 0;
    if (i < s.size() && (s[i] == 'v' || s[i] == 'V')) ++i;
    int count = 0;
    while (count < 3 && i < s.size()) {
        int value = 0;
        bool digit = false;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            value = value * 10 + (s[i] - '0');
            ++i;
            digit = true;
        }
        if (!digit) break;
        out[count++] = value;
        if (i < s.size() && s[i] == '.') {
            ++i;
            continue;
        }
        break;
    }
    return count > 0;
}

int compare_version(const char* a, const char* b) {
    int left[3] = {0, 0, 0};
    int right[3] = {0, 0, 0};
    const bool left_ok = parse_version(a, left);
    const bool right_ok = parse_version(b, right);
    if (!left_ok && !right_ok) return 0;
    if (!left_ok) return -1;
    if (!right_ok) return 1;
    for (int i = 0; i < 3; ++i) {
        if (left[i] != right[i]) return left[i] > right[i] ? 1 : -1;
    }
    return 0;
}

namespace {

std::string lower_copy(std::string_view text) {
    std::string out(text);
    for (char& ch : out) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return out;
}

// semver 的预发布后缀拆词：0.12.0-canary.3 → {"canary", "3"}；没有后缀返回空
std::vector<std::string> version_suffix(std::string_view text) {
    std::vector<std::string> tokens;
    std::string value(text);
    const size_t plus = value.find('+');   // 构建元数据不参与比较
    if (plus != std::string::npos) value.erase(plus);
    const size_t dash = value.find('-');
    if (dash == std::string::npos) return tokens;
    size_t i = dash + 1;
    while (i < value.size()) {
        const size_t dot = value.find('.', i);
        const std::string token = value.substr(i, dot == std::string::npos ? std::string::npos : dot - i);
        if (!token.empty()) tokens.push_back(token);
        if (dot == std::string::npos) break;
        i = dot + 1;
    }
    return tokens;
}

// 纯数字词判数值（溢出按大数处理，够用即可）
bool token_number(const std::string& token, long long* out) {
    if (token.empty()) return false;
    long long value = 0;
    for (char ch : token) {
        if (ch < '0' || ch > '9') return false;
        if (value < 100000000000000LL) value = value * 10 + (ch - '0');
    }
    *out = value;
    return true;
}

// 按字节截断文本，但必须落在 UTF-8 字符边界上：
// 从中间切开一个汉字会让整个响应体变成非法 UTF-8，前端 JSON.parse 直接失败。
std::string truncate_utf8(const std::string& text, size_t max_bytes) {
    if (text.size() <= max_bytes) return text;
    size_t cut = max_bytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
    return text.substr(0, cut) + "…";
}

}  // namespace

int compare_version_full(const char* a, const char* b) {
    const int base = compare_version(a, b);
    if (base != 0) return base;
    const std::vector<std::string> left = version_suffix(a ? a : "");
    const std::vector<std::string> right = version_suffix(b ? b : "");
    if (left.empty() && right.empty()) return 0;
    // 无后缀 = 正式版，比任何预发布都新（semver）
    if (left.empty()) return 1;
    if (right.empty()) return -1;
    const size_t count = std::min(left.size(), right.size());
    for (size_t i = 0; i < count; ++i) {
        long long ln = 0;
        long long rn = 0;
        const bool left_number = token_number(left[i], &ln);
        const bool right_number = token_number(right[i], &rn);
        if (left_number && right_number) {
            if (ln != rn) return ln > rn ? 1 : -1;
            continue;
        }
        // 数字词优先级低于字母词（semver：1.0.0-1 < 1.0.0-alpha）
        if (left_number != right_number) return left_number ? -1 : 1;
        if (left[i] != right[i]) return left[i] > right[i] ? 1 : -1;
    }
    if (left.size() == right.size()) return 0;
    return left.size() > right.size() ? 1 : -1;
}

// ---------------------------------------------------------------- 发布频道

std::string normalize_update_channel(std::string_view id) {
    const std::string value = lower_copy(id);
    if (value == "canary" || value == "nightly" || value == "insider") return "canary";
    if (value == "dev" || value == "develop" || value == "beta" || value == "alpha" || value == "preview" ||
        value == "rc") {
        return "dev";
    }
    return "release";
}

const char* update_channel_label(std::string_view id) {
    const std::string value = normalize_update_channel(id);
    if (value == "canary") return "金丝雀";
    if (value == "dev") return "开发版";
    return "正式版";
}

std::string classify_update_channel(const std::string_view tag, bool prerelease) {
    const std::string value = lower_copy(tag);
    const size_t dash = value.find('-');
    const std::string suffix = dash == std::string::npos ? std::string() : value.substr(dash + 1);
    // 只看 '-' 之后的后缀词：仓库名/产品名里出现 canary 字样不该被判成频道
    for (const std::string& token : version_suffix(suffix.empty() ? value : "-" + suffix)) {
        const std::string word = lower_copy(token);
        std::string letters;
        for (char ch : word) {
            if (ch >= '0' && ch <= '9') break;
            letters += ch;
        }
        if (letters.rfind("canary", 0) == 0) return "canary";
        if (letters.rfind("dev", 0) == 0 || letters.rfind("nightly", 0) == 0 || letters.rfind("alpha", 0) == 0 ||
            letters.rfind("beta", 0) == 0 || letters.rfind("rc", 0) == 0 || letters.rfind("preview", 0) == 0) {
            return "dev";
        }
    }
    // 没有可识别的后缀：以 GitHub 的 prerelease 标记为准
    return prerelease ? "dev" : "release";
}

std::string channel_of_version(std::string_view version) {
    return classify_update_channel(version, false);
}

// ---------------------------------------------------------------- 下载源

std::string normalize_update_source(std::string_view id) {
    const std::string value = lower_copy(id);
    if (value == "gitee" || value == "gitee.com" || value == "cn" || value == "mirror") return "gitee";
    return "github";
}

const char* update_source_label(std::string_view id) {
    return normalize_update_source(id) == "gitee" ? "Gitee" : "GitHub";
}

const char* update_state_id(UpdateState state) {
    switch (state) {
        case UpdateState::Idle: return "idle";
        case UpdateState::Checking: return "checking";
        case UpdateState::UpToDate: return "up_to_date";
        case UpdateState::Available: return "available";
        case UpdateState::Downloading: return "downloading";
        case UpdateState::Downloaded: return "downloaded";
        case UpdateState::Applying: return "applying";
        case UpdateState::Failed: return "failed";
    }
    return "idle";
}

std::vector<UpdateSegment> plan_update_segments(long long total, int threads, long long min_segment) {
    std::vector<UpdateSegment> segments;
    if (total <= 0) return segments;
    int count = threads;
    if (count < 1) count = 1;
    if (count > 16) count = 16;
    if (min_segment < 1) min_segment = 1;
    // 段太小没意义：退化成更少的段（1 段 = 单线程下载）
    const long long max_count = (total + min_segment - 1) / min_segment;
    if (static_cast<long long>(count) > max_count) count = static_cast<int>(max_count);
    if (count < 1) count = 1;

    const long long base = total / count;
    long long start = 0;
    for (int i = 0; i < count; ++i) {
        const long long size = (i == count - 1) ? (total - start) : base;   // 余数并入最后一段
        UpdateSegment segment;
        segment.start = start;
        segment.end = start + size - 1;
        segments.push_back(segment);
        start += size;
    }
    return segments;
}

// ---------------------------------------------------------------- UpdateService

UpdateService::UpdateService(std::filesystem::path updates_dir, std::string current_version, std::string repo,
                             std::string platform_slug)
    : updates_dir_(std::move(updates_dir)),
      current_version_(std::move(current_version)),
      repo_(std::move(repo)),
      slug_(std::move(platform_slug)) {
    // Gitee 镜像仓库默认与 GitHub 同名（构建宏 RADMUSIC_GITEE_REPO 可覆盖）
    gitee_repo_ = update_gitee_repo();
}
std::string UpdateService::detect_platform_slug() {
#if defined(_WIN32)
#if defined(_M_ARM64) || defined(__aarch64__)
    return "win-arm64";
#else
    return "win64";
#endif
#elif defined(__APPLE__)
#if defined(__aarch64__) || defined(__arm64__)
    return "macos-arm64";
#else
    return "macos-x86_64";
#endif
#elif defined(__linux__)
#if defined(__aarch64__)
    return "linux-aarch64";
#else
    return "linux-x86_64";
#endif
#else
    return std::string();
#endif
}

bool UpdateService::busy() const {
    return state_ == UpdateState::Checking || state_ == UpdateState::Downloading;
}

Status UpdateService::begin_check(bool allow_downgrade) {
    if (busy()) return Status(ErrorCode::RequestFailed, "更新任务正在执行，请稍候");
    if (repo_.empty()) return Status(ErrorCode::RequestFailed, "未配置更新仓库");
    error_.clear();
    auto_apply_pending_ = false;   // 重新检查即取消待执行的自动安装
    pending_check_ = true;
    pending_allow_downgrade_ = allow_downgrade;
    state_ = UpdateState::Checking;
    return Status::success();
}

Status UpdateService::begin_download(int threads) {
    if (busy()) return Status(ErrorCode::RequestFailed, "更新任务正在执行，请稍候");
    if (!has_update_) return Status(ErrorCode::RequestFailed, "没有可下载的新版本，请先检查更新");
    if (asset_.url.empty()) {
        return Status(ErrorCode::RequestFailed,
                      "该版本没有当前平台的安装包（" + slug_ + "），请到发布页手动下载");
    }
    if (install_dir_.empty()) return Status(ErrorCode::RequestFailed, "未注入安装目录，无法准备更新");
    if (threads > 0) set_threads(threads);

    const std::string suffix = asset_.name.size() > 7 &&
                                       asset_.name.compare(asset_.name.size() - 7, 7, ".tar.gz") == 0
                                   ? ".tar.gz"
                                   : ".zip";
    archive_path_ = updates_dir_ / ("radmusic-" + (tag_.empty() ? latest_ : tag_) + "-" + slug_ + suffix);

    if (file_exists(archive_path_) && asset_.size > 0 && file_size_or_zero(archive_path_) == asset_.size) {
        received_ = asset_.size;
        total_ = asset_.size;
        state_ = UpdateState::Downloaded;
        error_.clear();
        schedule_auto_apply();   // 包已完好：等同于本次下载完成
        return Status::success();
    }
    error_.clear();

    // 多线程：按 Range 把文件切成若干段并发下载（每段至少 512KB，避免小包被切碎）
    const std::vector<UpdateSegment> segments = plan_update_segments(asset_.size, threads_);
    if (segments.size() > 1) {
        return start_parallel(segments, asset_.size);
    }

    // 单线程：沿用「Range 续传 + step() 分片推进」，可复用已下载的部分
    received_ = file_size_or_zero(part_path());
    total_ = asset_.size;
    pending_download_ = true;
    state_ = UpdateState::Downloading;
    return Status::success();
}

void UpdateService::set_threads(int threads) {
    if (threads < 1) threads = 1;
    if (threads > 16) threads = 16;
    threads_ = threads;
}

void UpdateService::set_auto_apply(bool enabled) {
    auto_apply_ = enabled;
    // 关设置必须立刻取消倒计时：不能用户都关了还擅自重启
    if (!enabled) auto_apply_pending_ = false;
}

void UpdateService::cancel_auto_apply() { auto_apply_pending_ = false; }

void UpdateService::schedule_auto_apply() {
    if (!auto_apply_ || state_ != UpdateState::Downloaded) {
        auto_apply_pending_ = false;
        return;
    }
    auto_apply_pending_ = true;
    auto_apply_at_ = std::chrono::steady_clock::now() + std::chrono::seconds(kAutoApplyDelaySec);
    RM_LOG_INFO("update", "更新包已就绪，" + std::to_string(kAutoApplyDelaySec) +
                              " 秒后自动安装并重启（可在设置页取消）");
}

bool UpdateService::set_channel(std::string channel) {
    const std::string next = normalize_update_channel(channel);
    if (next == channel_) return false;
    channel_ = next;
    // 正在下载 / 已下载待安装 / 正在应用：保留当前任务，别把用户手里的包抽走；
    // 其余情况（空闲 / 已检查 / 失败）直接作废旧结果，等下一次检查。
    const bool keep_task =
        busy() || state_ == UpdateState::Downloaded || state_ == UpdateState::Applying;
    if (!keep_task) reset_check_result();
    return true;
}

void UpdateService::set_repo(std::string repo) {
    if (repo.empty() || repo == repo_) return;
    repo_ = std::move(repo);
    // 换了仓库，旧的发布列表 / 目标版本都不再对应这个仓库
    const bool keep_task =
        busy() || state_ == UpdateState::Downloaded || state_ == UpdateState::Applying;
    if (!keep_task) reset_check_result();
}

void UpdateService::set_gitee_repo(std::string repo) {
    if (repo == gitee_repo_) return;
    gitee_repo_ = std::move(repo);
    // 只在当前就在用 Gitee 源时才作废结果（否则切回去时自然会重新检查）
    if (source_ != "gitee") return;
    const bool keep_task =
        busy() || state_ == UpdateState::Downloaded || state_ == UpdateState::Applying;
    if (!keep_task) reset_check_result();
}

bool UpdateService::set_source(std::string source) {
    const std::string next = normalize_update_source(source);
    if (next == source_) return false;
    source_ = next;
    // 两个源的发布列表不能混用（tag 内容可能不同），切源即作废旧检查结果；
    // 正在下载 / 已下载待安装的包保留，由用户决定装不装。
    const bool keep_task =
        busy() || state_ == UpdateState::Downloaded || state_ == UpdateState::Applying;
    if (!keep_task) reset_check_result();
    return true;
}

std::string UpdateService::releases_url() const {
    const std::string repo = active_repo();
    if (source_ == "gitee") {
        const std::string base =
            gitee_api_base_.empty() ? std::string("https://gitee.com/api/v5") : gitee_api_base_;
        // Gitee 的分页参数是 page/per_page，与 GitHub 同名
        return base + "/repos/" + repo + "/releases?per_page=30&page=1";
    }
    const std::string base = api_base_.empty() ? std::string("https://api.github.com") : api_base_;
    return base + "/repos/" + repo + "/releases?per_page=30";
}

std::string UpdateService::release_page_url(const std::string& tag) const {
    const std::string base = source_ == "gitee" ? "https://gitee.com/" : "https://github.com/";
    const std::string repo = active_repo();
    if (repo.empty()) return std::string();
    if (tag.empty()) return base + repo + "/releases";
    return base + repo + "/releases/tag/" + tag;
}

const ReleaseInfo* UpdateService::find_release(std::string_view tag) const {
    // 容忍 "v0.12.3" / "V0.12.3" / "0.12.3" 三种写法，大小写不敏感
    std::string want = lower_copy(tag);
    while (!want.empty() && (want.front() == ' ' || want.front() == '\t')) want.erase(0, 1);
    while (!want.empty() && (want.back() == ' ' || want.back() == '\t' || want.back() == '\r' ||
                             want.back() == '\n')) {
        want.pop_back();
    }
    if (!want.empty() && want.front() == 'v') want.erase(0, 1);
    if (want.empty()) return nullptr;
    for (const auto& release : releases_) {
        if (lower_copy(release.version) == want) return &release;
        std::string with_v = "v" + lower_copy(release.version);
        if (with_v == want) return &release;
    }
    return nullptr;
}

Status UpdateService::select_version(std::string tag) {
    if (busy()) return Status(ErrorCode::RequestFailed, "更新任务正在执行，请稍候再切换版本");
    if (state_ == UpdateState::Applying) return Status(ErrorCode::RequestFailed, "更新已在进行中");

    // 空 tag = 取消指定，回到「跟随频道头部」
    if (tag.empty() || tag == "-") {
        manual_ = false;
        downgrade_ = false;
        auto_apply_pending_ = false;
        const ReleaseInfo* head = channel_head();
        if (head == nullptr) {
            reset_check_result();
            return Status::success();
        }
        tag_ = head->tag;
        latest_ = head->version;
        notes_ = head->notes;
        published_at_ = head->published_at;
        page_url_ = head->page_url;
        asset_ = head->asset;
        received_ = 0;
        total_ = asset_.size;
        has_update_ = compare_version_full(latest_.c_str(), current_version_.c_str()) > 0;
        error_.clear();
        if (has_update_ && asset_.url.empty()) {
            error_ = "该版本没有当前平台（" + slug_ + "）的安装包，请到发布页手动下载";
        }
        state_ = has_update_ ? UpdateState::Available : UpdateState::UpToDate;
        return Status::success();
    }

    if (releases_.empty()) {
        return Status(ErrorCode::RequestFailed, "还没有发布列表，请先检查更新再选择版本");
    }
    const ReleaseInfo* found = find_release(tag);
    if (found == nullptr) {
        return Status(ErrorCode::FileNotFound, "发布列表里没有版本 " + tag + "（可能刚切换过下载源或频道），请重新检查更新");
    }
    tag_ = found->tag;
    latest_ = found->version;
    notes_ = found->notes;
    published_at_ = found->published_at;
    page_url_ = found->page_url;
    asset_ = found->asset;
    received_ = 0;
    total_ = asset_.size;
    manual_ = true;
    downgrade_ = compare_version_full(latest_.c_str(), current_version_.c_str()) < 0;
    // 用户显式指定版本：即使版本号更旧也允许安装（这就是「回退到旧版本」的路径）
    has_update_ = true;
    error_.clear();
    auto_apply_pending_ = false;   // 换目标版本，旧的倒计时作废
    if (asset_.url.empty()) {
        error_ = "该版本没有当前平台（" + slug_ + "）的安装包，请到发布页手动下载";
    }
    state_ = UpdateState::Available;
    RM_LOG_INFO("update", std::string("已指定安装版本 ") + tag_ +
                              (downgrade_ ? "（降级：低于当前构建）" : "") + "，来源 " +
                              update_source_label(source_));
    return Status::success();
}

void UpdateService::reset_check_result() {
    latest_.clear();
    tag_.clear();
    notes_.clear();
    published_at_.clear();
    page_url_.clear();
    asset_ = UpdateAsset();
    has_update_ = false;
    manual_ = false;
    downgrade_ = false;
    error_.clear();
    checked_at_.clear();
    received_ = 0;
    total_ = 0;
    auto_apply_pending_ = false;
    state_ = UpdateState::Idle;
}

const ReleaseInfo* UpdateService::channel_head() const {
    for (const auto& release : releases_) {
        if (release.channel == channel_) return &release;
    }
    return nullptr;
}

UpdateAsset UpdateService::pick_asset(const json::Value& item) const {
    UpdateAsset asset;
    if (slug_.empty()) return asset;
#if defined(_WIN32)
    const std::string prefer = slug_ + ".zip";
    const std::string fallback = slug_ + ".tar.gz";
#else
    const std::string prefer = slug_ + ".tar.gz";
    const std::string fallback = slug_ + ".zip";
#endif
    // 先找本平台的主归档格式，再退而求其次
    for (const std::string& pattern : {prefer, fallback}) {
        for (const auto& entry : item["assets"].as_array()) {
            const std::string name = entry.get_string("name");
            if (name.size() <= pattern.size()) continue;
            if (name.compare(name.size() - pattern.size(), pattern.size(), pattern) != 0) continue;
            asset.name = name;
            asset.url = entry.get_string("browser_download_url");
            asset.size = entry.get_int("size", 0);
            break;
        }
        if (!asset.url.empty()) break;
    }
    return asset;
}

ReleaseInfo UpdateService::parse_release(const json::Value& item) const {
    ReleaseInfo info;
    info.tag = item.get_string("tag_name");
    info.name = item.get_string("name");
    info.notes = item.get_string("body");
    info.page_url = item.get_string("html_url");
    info.published_at = item.get_string("published_at");
    if (info.published_at.empty()) info.published_at = item.get_string("created_at");
    info.prerelease = item.get_bool("prerelease", false);
    info.draft = item.get_bool("draft", false);
    info.version = info.tag;
    if (!info.version.empty() && (info.version.front() == 'v' || info.version.front() == 'V')) {
        info.version.erase(0, 1);
    }
    info.channel = classify_update_channel(info.tag.empty() ? info.version : info.tag, info.prerelease);
    info.asset = pick_asset(item);
    if (info.page_url.empty()) info.page_url = release_page_url(info.tag);
    return info;
}

Status UpdateService::start_parallel(const std::vector<UpdateSegment>& segments, long long total) {
    std::error_code code;
    std::filesystem::create_directories(updates_dir_, code);

    const std::filesystem::path target = part_path();
    std::filesystem::remove(target, code);
    {
        std::ofstream create(target, std::ios::binary | std::ios::trunc);
        if (!create) {
            return Status(ErrorCode::RequestFailed, "无法创建更新包临时文件（目录不可写或磁盘已满）");
        }
    }
    // 预分配到最终大小：各段直接定位写入，互不踩踏
    std::filesystem::resize_file(target, static_cast<std::uintmax_t>(total), code);
    if (code) {
        std::filesystem::remove(target, code);
        return Status(ErrorCode::RequestFailed, "无法为更新包预留空间（磁盘空间不足？）");
    }

    stop_workers();
    cancel_.store(false);
    parallel_mode_ = true;
    segments_ = static_cast<int>(segments.size());
    parallel_received_.store(0);
    parallel_active_.store(segments_);
    segment_errors_.assign(segments.size(), std::string());
    received_ = 0;
    total_ = total;
    state_ = UpdateState::Downloading;
    error_.clear();

    const std::string url = asset_.url;
    workers_.reserve(segments.size());
    for (size_t index = 0; index < segments.size(); ++index) {
        const UpdateSegment segment = segments[index];
        workers_.emplace_back([this, index, segment, url, target]() {
            long long reported = 0;
            std::string segment_error;
            const bool ok = http_fetch_segment(
                url, target, segment.start, segment.end,
                [this, &reported](long long written) {
                    if (cancel_.load()) return false;
                    if (written <= reported) return true;
                    parallel_received_.fetch_add(written - reported, std::memory_order_relaxed);
                    reported = written;
                    return true;
                },
                &segment_error);
            if (!ok) {
                segment_errors_[index] = segment_error.empty() ? "分段下载失败" : segment_error;
            }
            parallel_active_.fetch_sub(1, std::memory_order_release);
        });
    }
    return Status::success();
}

void UpdateService::finish_parallel() {
    for (auto& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
    workers_.clear();
    parallel_mode_ = false;
    received_ = parallel_received_.load();
    if (asset_.size > 0) total_ = asset_.size;

    std::string failure;
    for (const auto& text : segment_errors_) {
        if (!text.empty()) {
            failure = text;
            break;
        }
    }
    segment_errors_.clear();

    std::error_code code;
    const std::filesystem::path target = part_path();
    if (!failure.empty()) {
        std::filesystem::remove(target, code);
        received_ = 0;
        // 源不支持 Range 是确定性问题（换单线程必然可行）：自动退回串行重下一次，
        // 不让用户对着「请把线程数改成 1」的报错手动折腾。
        const bool unsupported = failure.find("不支持分段下载") != std::string::npos ||
                                 failure.find("未按请求的范围") != std::string::npos;
        if (unsupported) {
            segments_ = 0;
            error_.clear();
            pending_download_ = true;
            state_ = UpdateState::Downloading;
            return;
        }
        state_ = UpdateState::Failed;
        error_ = "下载失败：" + failure;
        return;
    }
    const long long part_size = file_size_or_zero(target);
    if (total_ > 0 && part_size != total_) {
        std::filesystem::remove(target, code);
        received_ = 0;
        state_ = UpdateState::Failed;
        error_ = "更新包大小异常（" + std::to_string(part_size) + " / " + std::to_string(total_) +
                 " 字节），已删除，请重试";
        return;
    }
    std::filesystem::remove(archive_path_, code);
    std::filesystem::rename(target, archive_path_, code);
    if (code) {
        state_ = UpdateState::Failed;
        error_ = "下载完成但无法重命名安装包：" + code.message();
        return;
    }
    received_ = file_size_or_zero(archive_path_);
    total_ = received_;
    state_ = UpdateState::Downloaded;
    error_.clear();
    schedule_auto_apply();
}

void UpdateService::stop_workers() {
    if (workers_.empty()) return;
    cancel_.store(true);   // 让分片回调尽快退出，避免析构时长时间阻塞
    for (auto& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
    workers_.clear();
    cancel_.store(false);
    parallel_mode_ = false;
}

UpdateService::~UpdateService() {
    stop_workers();
}

bool UpdateService::step() {
    // 上次安装没走完（替换脚本被杀 / 断电）：启动后先接着装，最多自动重试 kApplyMaxResumeAttempts 次
    if (!resume_checked_ && request_exit) {
        resume_checked_ = true;
        arm_resume_if_needed();
    }
    if (resume_armed_ && std::chrono::steady_clock::now() >= resume_at_) {
        resume_armed_ = false;
        resume_apply();
        return true;
    }
    // 自动安装倒计时到点：直接走 apply()（失败会留在 failed + error，由界面提示）
    if (auto_apply_pending_ && std::chrono::steady_clock::now() >= auto_apply_at_) {
        auto_apply_pending_ = false;
        RM_LOG_WARN("update", "自动安装倒计时结束，开始应用更新");
        apply();
        return true;
    }
    if (pending_check_) {
        pending_check_ = false;
        run_check();
        return true;
    }
    if (parallel_mode_ && parallel_active_.load(std::memory_order_acquire) == 0) {
        finish_parallel();
        return true;
    }
    if (pending_download_) {
        pending_download_ = false;
        run_download();
        return true;
    }
    return false;
}

void UpdateService::run_check() {
    const bool gitee = source_ == "gitee";
    const std::string source_label = update_source_label(source_);
    if (gitee) {
        if (gitee_api_base_.empty()) gitee_api_base_ = "https://gitee.com/api/v5";
        if (gitee_repo_.empty()) gitee_repo_ = repo_;
    } else if (api_base_.empty()) {
        api_base_ = "https://api.github.com";
    }
    // 一次拉最近 30 条 Release：既能在各频道里挑头部版本，又能给「版本」页列出可选版本
    const std::string url = releases_url();
    std::vector<std::pair<std::string, std::string>> headers;
    if (gitee) {
        headers.push_back({"Accept", "application/json"});
    } else {
        headers.push_back({"Accept", "application/vnd.github+json"});
        headers.push_back({"X-GitHub-Api-Version", "2022-11-28"});
    }
    std::string body;
    std::string error;
    checked_at_ = now_iso8601();
    if (!http_fetch_text(url, headers, body, &error, 8 * 1024 * 1024)) {
        // 网络失败时保留上一次的发布列表，至少版本信息不会突然空白
        state_ = UpdateState::Failed;
        const std::string other = gitee ? "github" : "gitee";
        error_ = "查询 " + source_label + " 失败：" + error + "（可在「下载源」里切换到 " +
                 update_source_label(other) + "，或点「在 " + source_label + " 查看」手动下载）";
        if (error.find("404") != std::string::npos) {
            // 404 多半是仓库名不对或该源上还没有发布：给出可操作的方向，而不是让用户对着英文报错猜
            error_ += gitee ? "。请确认 Gitee 镜像仓库「" + active_repo() +
                                  "」存在且已发布 Release（可在设置里修改镜像仓库名）"
                            : "。请确认仓库「" + active_repo() + "」存在且已发布 Release";
        }
        return;
    }

    json::Value root = json::Value::parse(body);
    if (!root.is_array()) {
        state_ = UpdateState::Failed;
        error_ = root.is_object() && root.contains("message")
                     ? source_label + " 返回错误：" + root.get_string("message")
                     : source_label + " 返回的不是发布列表";
        return;
    }

    std::vector<ReleaseInfo> parsed;
    for (const auto& item : root.as_array()) {
        ReleaseInfo info = parse_release(item);
        if (info.draft || info.tag.empty()) continue;
        parsed.push_back(std::move(info));
    }
    // 版本倒序（含预发布后缀），发布时间作次要键：canary 各条目基础版本号往往相同
    std::stable_sort(parsed.begin(), parsed.end(), [](const ReleaseInfo& left, const ReleaseInfo& right) {
        const int cmp = compare_version_full(left.version.c_str(), right.version.c_str());
        if (cmp != 0) return cmp > 0;
        return left.published_at > right.published_at;
    });
    releases_ = std::move(parsed);

    const ReleaseInfo* head = channel_head();
    if (head == nullptr) {
        // 该频道还没有任何发布：不是错误，界面显示「暂无发布」即可
        latest_.clear();
        tag_.clear();
        notes_.clear();
        published_at_.clear();
        page_url_ = release_page_url(std::string());
        asset_ = UpdateAsset();
        has_update_ = false;
        manual_ = false;
        downgrade_ = false;
        error_.clear();
        received_ = 0;
        total_ = 0;
        state_ = UpdateState::UpToDate;
        return;
    }

    tag_ = head->tag;
    latest_ = head->version;
    notes_ = head->notes;
    page_url_ = head->page_url;
    published_at_ = head->published_at;
    asset_ = head->asset;
    received_ = 0;
    total_ = asset_.size;
    // 重新检查后回到「跟随频道头部」：之前手动指定的版本作废
    manual_ = false;
    downgrade_ = false;

    // 可更新 = 版本更新；或用户显式切到另一个频道（此时允许装该频道头部版本，即使版本号更小）
    const bool allow_downgrade = pending_allow_downgrade_;
    pending_allow_downgrade_ = false;
    const int cmp = compare_version_full(latest_.c_str(), current_version_.c_str());
    const bool channel_switch = channel_of_version(current_version_) != channel_;
    has_update_ = cmp > 0 || (cmp != 0 && channel_switch && allow_downgrade);
    downgrade_ = has_update_ && cmp < 0;
    state_ = has_update_ ? UpdateState::Available : UpdateState::UpToDate;
    error_.clear();
    if (has_update_ && asset_.url.empty()) {
        error_ = "该版本没有当前平台（" + slug_ + "）的安装包，请到发布页手动下载";
    }
}

std::filesystem::path UpdateService::part_path() const {
    std::filesystem::path path = archive_path_;
    path += ".part";
    return path;
}

void UpdateService::run_download() {
    if (asset_.url.empty()) {
        state_ = UpdateState::Failed;
        error_ = "没有可下载的安装包";
        return;
    }
    std::error_code code;
    std::filesystem::create_directories(updates_dir_, code);

    const std::filesystem::path target = part_path();
    long long received = file_size_or_zero(target);
    long long total = asset_.size;
    std::string error;
    const bool ok = http_fetch_file(asset_.url, target, &received, &total, &error, kChunkBytes);
    received_ = received;
    if (total > 0) total_ = total;
    if (!ok) {
        state_ = UpdateState::Failed;
        error_ = "下载失败：" + error;
        return;
    }
    const long long expected = asset_.size > 0 ? asset_.size : total_;
    if (expected > 0 && received < expected) {
        pending_download_ = true;   // 还有下一片，下一轮继续
        state_ = UpdateState::Downloading;
        return;
    }
    // 收到的大小必须与发布信息一致：不一致多半是服务器不支持 Range 导致重复追加，
    // 直接丢弃重下，绝不拿它去覆盖程序文件
    const long long part_size = file_size_or_zero(target);
    if (expected > 0 && part_size != expected) {
        std::filesystem::remove(target, code);
        received_ = 0;
        state_ = UpdateState::Failed;
        error_ = "更新包大小异常（" + std::to_string(part_size) + " / " + std::to_string(expected) +
                 " 字节），已删除，请重新下载";
        return;
    }
    std::filesystem::remove(archive_path_, code);
    std::filesystem::rename(target, archive_path_, code);
    if (code) {
        state_ = UpdateState::Failed;
        error_ = "下载完成但无法重命名安装包：" + code.message();
        return;
    }
    received_ = file_size_or_zero(archive_path_);
    total_ = received_;
    state_ = UpdateState::Downloaded;
    error_.clear();
    schedule_auto_apply();
}

Status UpdateService::prepare_stage(std::filesystem::path* stage_dir) {
    if (!file_exists(archive_path_)) {
        return Status(ErrorCode::FileNotFound, "更新包不存在，请先下载");
    }
    if (install_dir_.empty()) return Status(ErrorCode::RequestFailed, "未注入安装目录");
    if (!file_exists(install_dir_ / executable_name())) {
        return Status(ErrorCode::FileNotFound,
                      "安装目录下找不到 " + std::string(executable_name()) + "：" + path_to_utf8(install_dir_));
    }

    const std::filesystem::path stage = updates_dir_ / "stage";
    std::error_code code;
    std::filesystem::remove_all(stage, code);
    std::filesystem::create_directories(stage, code);

    const archive::ExtractResult extracted = archive::extract_archive(archive_path_, stage);
    if (!extracted.ok()) {
        return Status(ErrorCode::RequestFailed, "解压更新包失败：" + extracted.error);
    }
    if (!file_exists(stage / executable_name())) {
        return Status(ErrorCode::RequestFailed, "更新包里缺少 " + std::string(executable_name()) + "，已中止");
    }
    *stage_dir = stage;
    return Status::success();
}

Status UpdateService::apply() {
    if (state_ == UpdateState::Applying) {
        return Status(ErrorCode::RequestFailed, "更新已在进行中");
    }
    if (state_ != UpdateState::Downloaded) {
        return Status(ErrorCode::RequestFailed, "还没有下载好的更新包，请先检查并下载");
    }
    auto_apply_pending_ = false;   // 手动安装优先，倒计时作废
    std::filesystem::path stage;
    const Status prepared = prepare_stage(&stage);
    if (!prepared.ok()) {
        state_ = UpdateState::Failed;
        error_ = prepared.message;
        return prepared;
    }

#if defined(_WIN32)
    const Status applied = apply_windows(stage);
#else
    const Status applied = apply_posix(stage);
#endif
    if (!applied.ok()) {
        state_ = UpdateState::Failed;
        error_ = applied.message;
        return applied;
    }
    state_ = UpdateState::Applying;
    error_.clear();
    return Status::success();
}

#if defined(_WIN32)
Status UpdateService::apply_windows(const std::filesystem::path& stage) {
    (void)stage;
    // 1. 记录原始启动参数（去掉 argv[0]，脚本自己拼 exe 路径）
    std::vector<std::string> args;
    for (size_t i = 1; i < launch_argv_.size(); ++i) args.push_back(launch_argv_[i]);
    if (!write_text(updates_dir_ / "relaunch.args", args.empty() ? std::string() : build_command_line(args))) {
        return Status(ErrorCode::RequestFailed, "无法写入重启参数文件（目录不可写）");
    }

    // 2. 写替换脚本并启动（等本进程退出后再覆盖 exe/dll）
    const std::filesystem::path script = updates_dir_ / "apply_update.ps1";
    const std::string content =
        windows_replacer_script(static_cast<unsigned long>(GetCurrentProcessId()), !args.empty());
    if (!write_text_bom(script, content)) {
        return Status(ErrorCode::RequestFailed, "无法写入更新脚本（目录不可写）");
    }

    const std::string command = "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass "
                                "-WindowStyle Hidden -File \"" +
                                path_to_utf8(script) + "\"";

    // 3. 留一个「这次安装没走完」的标记：万一脚本被杀（对方在 job 里、断电、被安全软件拦），
    //    下次启动会看到它并自动把剩下的活干完，而不是静默地停在旧版本。
    const std::string target_version = tag_.empty() ? latest_ : tag_;
    write_apply_marker(updates_dir_, target_version, apply_attempt_);

    LaunchPath path = LaunchPath::Plain;
    if (!launch_replacer(command, &path)) {
        // 脚本没起来：标记留着，下次启动重试；这里只报错，不退出进程
        RM_LOG_ERROR("update", "启动替换脚本失败（" + command + "）");
        error_ = "启动更新脚本失败（无法覆盖运行中的程序），下次启动会自动重试";
        state_ = UpdateState::Failed;
        return Status(ErrorCode::RequestFailed, error_);
    }
    RM_LOG_WARN("update", std::string("已启动替换脚本（方式=") + launch_path_name(path) +
                              (apply_attempt_ > 0 ? "，续装第 " + std::to_string(apply_attempt_) + " 次" : "") +
                              "），等待进程退出后覆盖并重启");

    if (!request_exit) {
        RM_LOG_ERROR("update", "宿主没有注入 request_exit，替换脚本会等 90 秒后放弃");
    } else {
        request_exit();
    }
    return Status::success();
}
#else
Status UpdateService::apply_posix(const std::filesystem::path& stage) {
    std::error_code code;
    std::filesystem::recursive_directory_iterator it(stage, code);
    if (code) return Status(ErrorCode::RequestFailed, "无法遍历解压目录：" + code.message());
    for (const auto& entry : it) {
        const std::filesystem::path relative = std::filesystem::relative(entry.path(), stage, code);
        if (code) continue;
        const std::filesystem::path target = install_dir_ / relative;
        if (entry.is_directory()) {
            std::filesystem::create_directories(target, code);
            continue;
        }
        std::filesystem::create_directories(target.parent_path(), code);
        std::filesystem::copy_file(entry.path(), target,
                                   std::filesystem::copy_options::overwrite_existing, code);
        if (code) return Status(ErrorCode::RequestFailed, "覆盖文件失败：" + path_to_utf8(relative));
    }
    const std::filesystem::path exe = install_dir_ / executable_name();
    ::chmod(exe.c_str(), 0755);
    if (!restart_handler) {
        return Status(ErrorCode::RequestFailed, "已覆盖文件，但宿主未注入重启回调，请手动重启");
    }
    const Status restarted = restart_handler();
    if (!restarted.ok()) return restarted;
    return Status::success();
}
#endif

// ---------------------------------------------------------------- 续装：上次安装没走完
//
// apply() 会在 <updates>/apply.pending 留一个标记，脚本干完活才删。启动时如果标记还在、
// stage 也还在，说明上一次覆盖没做成（脚本被 job 带走、断电、或被安全软件拦下）。
// 这时旧进程还占着 exe，只能「再拉起一次脚本 + 自己退出」——所以放到启动后几秒，
// 且最多自动重试 kApplyMaxResumeAttempts 次，避免陷入「一启动就退出」的死循环。

void UpdateService::arm_resume_if_needed() {
#if defined(_WIN32)
    std::string version;
    const int attempts = read_apply_attempts(updates_dir_, &version);
    if (attempts < 0) return;   // 没有未完成的安装
    std::error_code ec;
    const std::filesystem::path stage = updates_dir_ / "stage";
    if (!std::filesystem::is_directory(stage, ec)) {
        // 活儿其实干完了，只是没来得及删标记（例如删到一半被中断）
        clear_apply_marker(updates_dir_);
        return;
    }
    apply_pending_ = true;
    apply_attempts_ = attempts;
    if (attempts >= kApplyMaxResumeAttempts) {
        apply_hint_ = "上次更新没有装完，已自动重试 " + std::to_string(attempts) +
                      " 次仍未成功。可以重新「检查更新 → 下载 → 安装」，或手动把 updates 里 stage "
                      "目录内的文件覆盖到安装目录。";
        RM_LOG_ERROR("update", "放弃自动续装（已重试 " + std::to_string(attempts) + " 次）");
        return;
    }
    apply_attempt_ = attempts + 1;
    resume_armed_ = true;
    resume_at_ = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    apply_hint_ = "检测到上次更新没装完（" + (version.empty() ? std::string("未知版本") : version) +
                  "），几秒后会自动继续安装（第 " + std::to_string(apply_attempt_) + " 次尝试）";
    RM_LOG_WARN("update", apply_hint_);
#endif
}

void UpdateService::resume_apply() {
#if defined(_WIN32)
    std::error_code ec;
    const std::filesystem::path stage = updates_dir_ / "stage";
    if (!std::filesystem::is_directory(stage, ec)) {
        clear_apply_marker(updates_dir_);
        apply_pending_ = false;
        return;
    }
    RM_LOG_WARN("update", "继续安装上次未完成的更新（第 " + std::to_string(apply_attempt_) + " 次尝试）");
    state_ = UpdateState::Applying;
    error_.clear();
    const Status applied = apply_windows(stage);   // 成功会 request_exit，进程随即退出
    if (!applied.ok()) {
        state_ = UpdateState::Failed;
        error_ = applied.message;
        apply_hint_ = applied.message;
        RM_LOG_ERROR("update", "续装失败：" + applied.message);
    }
#endif
}

json::Value UpdateService::status() const {
    json::Value data = json::Value::object();
    data.set("current", current_version_);
    data.set("latest", latest_);
    data.set("tag", tag_);
    data.set("has_update", has_update_);
    data.set("state", update_state_id(state_));
    data.set("platform", slug_);
    data.set("repo", repo_);
    data.set("notes", notes_);
    data.set("published_at", published_at_);
    data.set("page_url", page_url_.empty() ? release_page_url(tag_) : page_url_);
    data.set("error", error_);
    data.set("checked_at", checked_at_);
    data.set("updates_dir", path_to_utf8(updates_dir_));
    data.set("install_dir", path_to_utf8(install_dir_));
    data.set("busy", busy());
    data.set("can_download", has_update_ && !asset_.url.empty() && !busy());
    data.set("can_apply", state_ == UpdateState::Downloaded);
    data.set("is_dev_version", current_version_.find("dev") != std::string::npos);
    // 上次安装的收尾情况：pending_apply=true 表示有一次更新没装完（界面据此提示）
    data.set("pending_apply", apply_pending_);
    data.set("apply_attempts", static_cast<long long>(apply_attempts_));
    data.set("apply_hint", apply_hint_);
    // 下载后自动安装：开关状态 + 倒计时剩余秒数（仅倒计时进行中出现）
    data.set("auto_apply", auto_apply_);
    if (auto_apply_pending_) {
        const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
                                   auto_apply_at_ - std::chrono::steady_clock::now())
                                   .count();
        data.set("auto_apply_in", remaining < 1 ? 1 : remaining);   // 到点前至少显示 1 秒
    }

    // 频道：当前选择的频道、当前构建的频道、每个频道的最新发布
    const std::string build_channel = channel_of_version(current_version_);
    data.set("channel", channel_);
    data.set("channel_label", update_channel_label(channel_));
    data.set("build_channel", build_channel);
    data.set("build_channel_label", update_channel_label(build_channel));
    data.set("has_release", !tag_.empty());
    // 下载源：当前选择 + 可选列表（前端「下载源」下拉）
    data.set("source", source_);
    data.set("source_label", update_source_label(source_));
    data.set("source_repo", active_repo());
    data.set("gitee_repo", gitee_repo_.empty() ? repo_ : gitee_repo_);
    // 构建期的默认仓库：设置页「恢复默认」用（宏可被发行流水线覆盖）
    data.set("repo_default", std::string(update_repo()));
    data.set("gitee_repo_default", std::string(update_gitee_repo()));
    json::Value sources = json::Value::array();
    for (int i = 0; i < kUpdateSourceCount; ++i) {
        const std::string id = kUpdateSources[i];
        const std::string repo_for = id == "gitee" ? (gitee_repo_.empty() ? repo_ : gitee_repo_) : repo_;
        json::Value entry = json::Value::object();
        entry.set("id", id);
        entry.set("label", update_source_label(id));
        entry.set("selected", id == source_);
        entry.set("repo", repo_for);
        sources.push_back(std::move(entry));
    }
    data.set("sources", sources);
    // 是否由用户显式指定了版本（选择性更新 / 降级回滚），以及该版本是降级还是重装当前版本
    data.set("manual", manual_);
    data.set("downgrade", !tag_.empty() && compare_version_full(latest_.c_str(), current_version_.c_str()) < 0);
    data.set("same_version", !tag_.empty() && compare_version_full(latest_.c_str(), current_version_.c_str()) == 0);
    // 该频道头部版本比当前构建还旧（常见于本地开发版跑在正式版频道上）：
    // 不提示更新，界面据此说明「低于当前构建」而不是误报有新版本。
    data.set("behind", !tag_.empty() && compare_version_full(latest_.c_str(), current_version_.c_str()) < 0);
    data.set("selected_tag", tag_);

    json::Value channels = json::Value::array();
    for (int i = 0; i < kUpdateChannelCount; ++i) {
        const std::string id = kUpdateChannels[i];
        const ReleaseInfo* head = nullptr;
        int count = 0;
        for (const auto& release : releases_) {
            if (release.channel != id) continue;
            ++count;
            if (head == nullptr) head = &release;
        }
        json::Value entry = json::Value::object();
        entry.set("id", id);
        entry.set("label", update_channel_label(id));
        entry.set("available", head != nullptr);
        entry.set("selected", id == channel_);
        entry.set("is_current", head != nullptr && !head->version.empty() && head->version == current_version_);
        entry.set("tag", head != nullptr ? head->tag : std::string());
        entry.set("version", head != nullptr ? head->version : std::string());
        entry.set("published_at", head != nullptr ? head->published_at : std::string());
        entry.set("page_url", head != nullptr ? head->page_url : std::string());
        entry.set("count", static_cast<long long>(count));
        channels.push_back(std::move(entry));
    }
    data.set("channels", channels);

    // 当前频道的发布历史（版本页「版本信息」列表，「安装此版本」的可选项）：说明截断，避免状态响应过大
    json::Value list = json::Value::array();
    for (const auto& release : releases_) {
        if (release.channel != channel_) continue;
        if (list.size() >= 20) break;
        std::string notes = truncate_utf8(release.notes, 800);
        json::Value item = json::Value::object();
        item.set("tag", release.tag);
        item.set("version", release.version);
        item.set("name", release.name);
        item.set("channel", release.channel);
        item.set("channel_label", update_channel_label(release.channel));
        item.set("published_at", release.published_at);
        item.set("page_url", release.page_url);
        item.set("prerelease", release.prerelease);
        item.set("notes", notes);
        item.set("asset_name", release.asset.name);
        item.set("asset_size", release.asset.size);
        const bool is_current = !release.version.empty() && release.version == current_version_;
        item.set("is_current", is_current);
        // 与当前构建的关系：前端据此提示「这是降级 / 重装」并给按钮换文案
        const int cmp = release.version.empty() ? 0
                                                 : compare_version_full(release.version.c_str(),
                                                                        current_version_.c_str());
        item.set("relation", cmp > 0 ? "newer" : (cmp < 0 ? "older" : "same"));
        // 只有「用户显式指定」的那条才算已选定（频道头部默认目标不标，避免误以为手动选了）
        item.set("selected", manual_ && !release.tag.empty() && release.tag == tag_);
        item.set("has_asset", !release.asset.url.empty());
        list.push_back(std::move(item));
    }
    data.set("releases", list);

    json::Value asset = json::Value::object();
    asset.set("name", asset_.name);
    asset.set("url", asset_.url);
    asset.set("size", asset_.size);
    data.set("asset", asset);

    json::Value download = json::Value::object();
    // 并行下载时进度由 worker 累加到原子量，串行时由 run_download 写 received_
    download.set("received", parallel_mode_ ? parallel_received_.load() : received_);
    download.set("total", total_ > 0 ? total_ : asset_.size);
    download.set("segments", segments_ > 0 ? segments_ : 1);
    download.set("path", path_to_utf8(archive_path_));
    data.set("download", download);
    data.set("threads", threads_);
    data.set("parallel", parallel_mode_);
    return data;
}

}  // namespace radmusic
