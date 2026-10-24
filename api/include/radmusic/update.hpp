// Rad music API - 检查更新（GitHub Releases）
//
// 流程：检查 → 下载 → 应用（解压覆盖 + 重启）
//
// 「下载后自动安装」：设置里打开 update_auto_apply 时，下载完成会安排一次
// kAutoApplyDelaySec 秒的倒计时，到点自动走 apply()（解压覆盖 + 重启）；
// 倒计时期间可取消（cancel_auto_apply）或立刻安装（apply）。倒计时由 step() 驱动，
// 关设置 / 手动安装 / 重新检查都会取消它，绝不擅自重启。
//
// 设计约束（单线程模型）：
//   * 不阻塞主循环：begin_check() / begin_download() 只置状态，真正的网络 I/O 在 step() 里分片推进
//     （宿主 idle hook 每轮调一次），网络再慢也不会卡住其他接口；
//   * 只信任本平台的发布包：按 -<slug>.zip / -<slug>.tar.gz 匹配 asset，不下载跨平台产物；
//   * 应用更新前必须先解压到 staging 并校验可执行文件存在，绝不直接覆盖运行中的目录。
//
// Windows 上正在运行的 exe/dll 无法被覆盖，因此 apply() 只做「解压 + 写替换脚本 + 请求退出」，
// 由 PowerShell 脚本等进程退出后再覆盖并重新拉起；其他平台可直接覆盖（inode 语义）后 execv。
#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "radmusic/error.hpp"
#include "radmusic/json.hpp"

namespace radmusic {

// 版本字符串解析（"v0.12.1" → 0,12,1）；返回 false 表示无法解析
bool parse_version(const char* text, int out[3]);
// a>b 返回 1，相等 0，a<b 返回 -1
int compare_version(const char* a, const char* b);
// 同上，但连预发布后缀一起比（语义对齐 semver）：
//   0.12.0 > 0.12.0-canary.2；0.12.0-canary.10 > 0.12.0-canary.9；0.12.0-dev.1 < 0.12.0-rc.1
int compare_version_full(const char* a, const char* b);

// ---------------------------------------------------------------- 发布频道
//
// 频道由 GitHub Release 的 tag / prerelease 标记推导，前端可以切换：
//   release：正式版（tag 无预发布后缀，如 v0.12.0；GitHub 上非 prerelease）
//   dev    ：开发版（-dev/-alpha/-beta/-rc/-preview 后缀，或 GitHub 标记为 prerelease）
//   canary ：金丝雀（-canary 后缀，随提交自动出包：最新但最不稳定）
// 每个频道各自取「该频道里最新的一条发布」，切换频道即切到该频道的头部版本。
constexpr const char* kUpdateChannels[] = {"release", "dev", "canary"};
constexpr int kUpdateChannelCount = 3;

// 归一化频道 id（未知值回落 "release"）
std::string normalize_update_channel(std::string_view id);
// 频道显示名（中文）
const char* update_channel_label(std::string_view id);
// tag + prerelease 标记 → 频道 id
std::string classify_update_channel(const std::string_view tag, bool prerelease);
// 版本串（"0.12.0-canary.3" / "0.11.0-dev"）→ 频道 id；用于识别当前构建属于哪个频道
std::string channel_of_version(std::string_view version);

// 多线程下载的分段方案：把 [0, total) 均分成不超过 threads 段，每段至少 min_segment 字节。
// total <= 0 或分段数不足 2 时返回单段（调用方退回单线程下载）。
struct UpdateSegment {
    long long start = 0;   // 含
    long long end = 0;     // 含
    long long size() const { return end - start + 1; }
};

std::vector<UpdateSegment> plan_update_segments(long long total, int threads, long long min_segment = 512 * 1024);

enum class UpdateState {
    Idle,          // 还没检查过
    Checking,      // 正在查询 GitHub（step 推进）
    UpToDate,      // 已是最新
    Available,     // 有新版本，可下载
    Downloading,   // 正在下载（step 推进）
    Downloaded,    // 已下载，可应用
    Applying,      // 已交给替换脚本 / 已发起重启
    Failed,        // 出错（error 有原因）
};

const char* update_state_id(UpdateState state);

struct UpdateAsset {
    std::string name;
    std::string url;
    long long size = 0;
};

// 一条 GitHub Release 的摘要（版本页的「版本信息」列表用）
struct ReleaseInfo {
    std::string tag;          // v0.12.0
    std::string version;      // 0.12.0（去掉 v 前缀）
    std::string name;         // 发布标题
    std::string channel;      // release / dev / canary
    std::string notes;        // 发布说明（正文）
    std::string published_at; // ISO8601
    std::string page_url;     // GitHub 发布页
    bool prerelease = false;
    bool draft = false;
    UpdateAsset asset;        // 本平台安装包（可能缺失）
};

class UpdateService {
public:
    UpdateService(std::filesystem::path updates_dir, std::string current_version, std::string repo,
                  std::string platform_slug);
    ~UpdateService();

    // 安装目录（exe 所在目录）与原始启动参数（apply 后用它拉起新进程，含 exe 路径）
    void set_install_dir(std::filesystem::path dir) { install_dir_ = std::move(dir); }
    void set_launch_argv(std::vector<std::string> argv) { launch_argv_ = std::move(argv); }
    // GitHub API 基址（默认 https://api.github.com；内网镜像 / GitHub Enterprise 可改）
    void set_api_base(std::string base) { api_base_ = std::move(base); }

    // 非 Windows：覆盖安装目录后重启自身（宿主注入 trigger_restart）
    std::function<Status()> restart_handler;
    // Windows：替换脚本已就绪，请求宿主退出（脚本会在退出后完成覆盖并拉起新进程）
    std::function<void()> request_exit;

    // 触发检查 / 下载（异步，实际工作由 step() 推进）
    // allow_downgrade：允许安装比当前构建更旧的版本。只在用户「显式切换频道」时传 true，
    // 否则切到正式版这类版本号更低的频道会无端提示降级。
    Status begin_check(bool allow_downgrade = false);
    // threads：下载线程数（1-16）。>1 且包足够大时按 Range 分段并发下载；<=0 用当前值。
    Status begin_download(int threads = 0);
    // 应用更新：解压 + 覆盖 + 重启（调用后本进程会退出）
    Status apply();

    // 下载线程数（1-16，默认取自宿主设置）
    void set_threads(int threads);
    int threads() const { return threads_; }

    // 下载后自动安装（宿主设置 update_auto_apply）：开启后下载完成即安排倒计时，
    // 到点自动 apply()。关设置会立刻取消尚未执行的倒计时。
    void set_auto_apply(bool enabled);
    bool auto_apply() const { return auto_apply_; }
    // 取消待执行的自动安装倒计时；更新包保留在 Downloaded，之后仍可手动安装
    void cancel_auto_apply();

    // 发布频道（release / dev / canary）。切换后旧的检查结果作废，
    // 需要重新 begin_check()；正在下载 / 已下载的包不受影响（由用户决定是否安装）。
    // 返回 true 表示频道确实变了。
    bool set_channel(std::string channel);
    const std::string& channel() const { return channel_; }

    // 由宿主 idle hook 驱动；返回 true 表示本轮做了事
    bool step();
    bool busy() const;

    json::Value status() const;
    const std::string& current_version() const { return current_version_; }
    static std::string detect_platform_slug();

private:
    void run_check();
    void run_download();
    // 下载完成的统一收尾：按 auto_apply_ 安排「倒计时 → apply()」（3 个下载完成点调用）
    void schedule_auto_apply();
    // 把一条 GitHub Release 的 JSON 转成 ReleaseInfo（本平台资源在这里挑好）
    ReleaseInfo parse_release(const json::Value& item) const;
    // 从 release 的 assets 里挑本平台安装包（优先 zip/tar.gz 中与平台匹配的那个）
    UpdateAsset pick_asset(const json::Value& item) const;
    // 当前频道最新的那条发布（releases_ 已按版本倒序排列）；没有返回 nullptr
    const ReleaseInfo* channel_head() const;
    // 清空检查结果（切换频道时用）
    void reset_check_result();
    // 多线程分段下载：起 workers_ 个线程各下一段，step() 里收尾
    Status start_parallel(const std::vector<UpdateSegment>& segments, long long total);
    void finish_parallel();
    void stop_workers();   // 析构 / 收尾时确保线程不悬空
    // 解压到 <updates>/stage 并校验可执行文件；成功返回 staging 目录
    Status prepare_stage(std::filesystem::path* stage_dir);
    Status apply_windows(const std::filesystem::path& stage_dir);
    Status apply_posix(const std::filesystem::path& stage_dir);
    std::filesystem::path part_path() const;

    std::filesystem::path updates_dir_;
    std::string current_version_;
    std::string repo_;
    std::string slug_;

    std::filesystem::path install_dir_;
    std::vector<std::string> launch_argv_;

    UpdateState state_ = UpdateState::Idle;
    bool pending_check_ = false;
    bool pending_download_ = false;
    bool pending_allow_downgrade_ = false;   // 本次检查是否允许降级安装
    std::string error_;

    // release 信息
    std::string latest_;
    std::string tag_;
    std::string notes_;
    std::string published_at_;
    std::string page_url_;
    std::string api_base_ = "https://api.github.com";
    bool has_update_ = false;
    UpdateAsset asset_;

    // 频道与发布列表（一次检查拉取最近 N 条 Release，按版本倒序）
    std::string channel_ = "release";
    std::vector<ReleaseInfo> releases_;

    std::filesystem::path archive_path_;
    long long received_ = 0;
    long long total_ = 0;
    std::string checked_at_;

    // 多线程分段下载状态（worker 只写自己的槽位 / 原子量，state_ 与 error_ 始终由主线程改）
    std::vector<std::thread> workers_;
    std::vector<std::string> segment_errors_;
    std::atomic<long long> parallel_received_{0};
    std::atomic<int> parallel_active_{0};
    std::atomic<bool> cancel_{false};
    bool parallel_mode_ = false;
    int threads_ = 4;
    int segments_ = 0;

    // 下载后自动安装：auto_apply_ 是设置开关，pending/at_ 是倒计时状态（只由主线程改）
    bool auto_apply_ = false;
    bool auto_apply_pending_ = false;
    std::chrono::steady_clock::time_point auto_apply_at_{};
};

}  // namespace radmusic
