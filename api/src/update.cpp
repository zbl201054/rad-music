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

// Windows：等旧进程退出 → 备份 exe → 覆盖 → 用原参数拉起 → 自清理
// 全程写 updates/apply_update.log，更新失败时用户可以直接查这个文件
std::string windows_replacer_script(unsigned long wait_pid, bool has_args) {
    std::ostringstream script;
    script << "$ErrorActionPreference = 'SilentlyContinue'\n"
           << "$updates = Split-Path -Parent $MyInvocation.MyCommand.Path\n"
           << "$install = Split-Path -Parent $updates\n"
           << "$stage = Join-Path $updates 'stage'\n"
           << "$exe = Join-Path $install 'radmusic_server.exe'\n"
           << "$log = Join-Path $updates 'apply_update.log'\n"
           << "function Log([string]$m) { \"$((Get-Date).ToString('s')) $m\" | Out-File -FilePath $log "
              "-Append -Encoding UTF8 }\n"
           << "$waitPid = " << wait_pid << "\n"
           << "Log \"start waitPid=$waitPid install=$install\"\n"
           << "$deadline = (Get-Date).AddSeconds(90)\n"
           << "while ((Get-Process -Id $waitPid -ErrorAction SilentlyContinue) -and ((Get-Date) -lt $deadline)) {\n"
           << "  Start-Sleep -Milliseconds 400\n"
           << "}\n"
           << "Start-Sleep -Milliseconds 600\n"
           << "if ((Test-Path $exe) -and (Test-Path $stage)) {\n"
           << "  Copy-Item $exe ($exe + '.bak') -Force\n"
           << "  Log 'backup written'\n"
           << "}\n"
           << "if (Test-Path $stage) {\n"
           << "  Get-ChildItem -Path $stage -Force | ForEach-Object {\n"
           << "    Copy-Item -Path $_.FullName -Destination $install -Recurse -Force\n"
           << "  }\n"
           << "  Log 'files copied'\n"
           << "}\n";
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
      slug_(std::move(platform_slug)) {}

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
    pending_check_ = true;
    pending_allow_downgrade_ = allow_downgrade;
    state_ = UpdateState::Checking;
    return Status::success();
}

Status UpdateService::begin_download(int threads) {
    if (busy()) return Status(ErrorCode::RequestFailed, "更新任务正在执行，请稍候");
    if (!has_update_) return Status(ErrorCode::RequestFailed, "没有可下载的新版本，请先检查更新");
    if (asset_.url.empty()) {
        return Status(ErrorCode::RequestFailed, "该版本没有当前平台的安装包（" + slug_ +
                                                    "），请到 GitHub 页面手动下载");
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

void UpdateService::reset_check_result() {
    latest_.clear();
    tag_.clear();
    notes_.clear();
    published_at_.clear();
    page_url_.clear();
    asset_ = UpdateAsset();
    has_update_ = false;
    error_.clear();
    checked_at_.clear();
    received_ = 0;
    total_ = 0;
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
    if (api_base_.empty()) api_base_ = "https://api.github.com";
    // 一次拉最近 30 条 Release：既能在各频道里挑头部版本，又能给「版本」页列出发布历史
    const std::string url = api_base_ + "/repos/" + repo_ + "/releases?per_page=30";
    const std::vector<std::pair<std::string, std::string>> headers = {
        {"Accept", "application/vnd.github+json"},
        {"X-GitHub-Api-Version", "2022-11-28"},
    };
    std::string body;
    std::string error;
    checked_at_ = now_iso8601();
    if (!http_fetch_text(url, headers, body, &error, 8 * 1024 * 1024)) {
        // 网络失败时保留上一次的发布列表，至少版本信息不会突然空白
        state_ = UpdateState::Failed;
        error_ = "查询 GitHub 失败：" + error +
                 "（网络无法访问 GitHub 时，可点「在 GitHub 查看」手动下载）";
        return;
    }

    json::Value root = json::Value::parse(body);
    if (!root.is_array()) {
        state_ = UpdateState::Failed;
        error_ = root.is_object() && root.contains("message")
                     ? "GitHub 返回错误：" + root.get_string("message")
                     : "GitHub 返回的不是发布列表";
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
        page_url_ = "https://github.com/" + repo_ + "/releases";
        asset_ = UpdateAsset();
        has_update_ = false;
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

    // 可更新 = 版本更新；或用户显式切到另一个频道（此时允许装该频道头部版本，即使版本号更小）
    const bool allow_downgrade = pending_allow_downgrade_;
    pending_allow_downgrade_ = false;
    const int cmp = compare_version_full(latest_.c_str(), current_version_.c_str());
    const bool channel_switch = channel_of_version(current_version_) != channel_;
    has_update_ = cmp > 0 || (cmp != 0 && channel_switch && allow_downgrade);
    state_ = has_update_ ? UpdateState::Available : UpdateState::UpToDate;
    error_.clear();
    if (has_update_ && asset_.url.empty()) {
        error_ = "该版本没有当前平台（" + slug_ + "）的安装包，请到 GitHub 页面手动下载";
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
    if (!write_text(script, content)) {
        return Status(ErrorCode::RequestFailed, "无法写入更新脚本（目录不可写）");
    }

    const std::string command = "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass "
                                "-WindowStyle Hidden -File \"" +
                                path_to_utf8(script) + "\"";
    const int wide_size =
        MultiByteToWideChar(CP_UTF8, 0, command.data(), static_cast<int>(command.size()), nullptr, 0);
    if (wide_size <= 0) return Status(ErrorCode::RequestFailed, "无法构造更新脚本命令");
    std::wstring wide(static_cast<size_t>(wide_size) + 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, command.data(), static_cast<int>(command.size()), wide.data(), wide_size);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // 注意：CREATE_NO_WINDOW 与 DETACHED_PROCESS 不能混用——后者会让 PowerShell 没有控制台而立刻退出，
    // 表现为「脚本没跑、文件没替换、进程也没重启」。这里用 CREATE_NO_WINDOW（无窗口但保留控制台）。
    DWORD flags = CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB;
    BOOL ok = CreateProcessW(nullptr, wide.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr, &si, &pi);
    if (!ok) {
        flags = CREATE_NO_WINDOW;   // 作业不允许 breakaway 时退回普通方式
        ok = CreateProcessW(nullptr, wide.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr, &si, &pi);
    }
    if (!ok) {
        RM_LOG_ERROR("update", "启动替换脚本失败: Win32 " + std::to_string(GetLastError()) + "（" + command + "）");
        return Status(ErrorCode::RequestFailed, "启动更新脚本失败（无法覆盖运行中的程序）");
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    RM_LOG_WARN("update", "已启动替换脚本（pid " + std::to_string(pi.dwProcessId) + "），等待进程退出后覆盖并重启");

    if (request_exit) request_exit();
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
    data.set("page_url", page_url_.empty() && !repo_.empty() ? "https://github.com/" + repo_ + "/releases"
                                                             : page_url_);
    data.set("error", error_);
    data.set("checked_at", checked_at_);
    data.set("updates_dir", path_to_utf8(updates_dir_));
    data.set("install_dir", path_to_utf8(install_dir_));
    data.set("busy", busy());
    data.set("can_download", has_update_ && !asset_.url.empty() && !busy());
    data.set("can_apply", state_ == UpdateState::Downloaded);
    data.set("is_dev_version", current_version_.find("dev") != std::string::npos);

    // 频道：当前选择的频道、当前构建的频道、每个频道的最新发布
    const std::string build_channel = channel_of_version(current_version_);
    data.set("channel", channel_);
    data.set("channel_label", update_channel_label(channel_));
    data.set("build_channel", build_channel);
    data.set("build_channel_label", update_channel_label(build_channel));
    data.set("has_release", !tag_.empty());
    // 该频道头部版本比当前构建还旧（常见于本地开发版跑在正式版频道上）：
    // 不提示更新，界面据此说明「低于当前构建」而不是误报有新版本。
    data.set("behind", !tag_.empty() && compare_version_full(latest_.c_str(), current_version_.c_str()) < 0);

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

    // 当前频道的发布历史（版本页「版本信息」列表）：说明截断，避免状态响应过大
    json::Value list = json::Value::array();
    for (const auto& release : releases_) {
        if (release.channel != channel_) continue;
        if (list.size() >= 12) break;
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
        item.set("is_current", !release.version.empty() && release.version == current_version_);
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
