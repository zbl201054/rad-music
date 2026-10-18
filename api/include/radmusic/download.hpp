// Rad music API - 下载（Readme.md 1.8 / 2.10）
//
// 下载把在线播放源曲目**永久**保存进本地曲库（与缓存不同，见 1.9）。
// 关键规则：
//   * 折叠条目必须显式指定 source，缺省一律拒绝（1007，1.8 / 2.10 规则 1）；
//   * 不同音质各建一条任务、各存一份文件，互不覆盖（1.8 规则 1 / 2.10 规则 2）；
//   * 同曲同质去重：已存在同 group_key + quality_key 的记录 → skip（1.8 规则 2）；
//   * 完成后校验 SHA-256，不一致返回 1022 并保留 .part 供重试（1.8 流程 3/6）；
//   * 落盘后写入 cache/meta/<song_key>/<quality_key>/base.json 并刷新库索引（1.8 流程 5）。
//
// 注入点：真实网络 I/O 由宿主提供（resolve_url / resolve_meta / fetch）；
// 未注入时 resolve_* 走插件 request，fetch 仅支持 file:// 或本地路径。
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "radmusic/error.hpp"
#include "radmusic/model.hpp"
#include "radmusic/plugin.hpp"
#include "radmusic/storage.hpp"

namespace radmusic {

enum class DownloadState { Queued, Running, Paused, Done, Failed, Canceled };
const char* download_state_id(DownloadState state);
DownloadState download_state_from_id(std::string_view id);

struct DownloadTask {
    std::string id;
    std::string source;            // platform，必须显式指定（1.8 规则：折叠条目必须选源）
    std::string item_id;
    std::string quality_request;   // auto / hires / lossless / high / medium / low / 平台 quality_id
    std::string filename_template; // 默认 {artist} - {title} [{quality}]
    std::string filename;          // 落盘后的实际文件名（未完成时为空，不要拿模板冒充）
    DownloadState state = DownloadState::Queued;
    long long received = 0;
    long long total = 0;
    int retry = 0;
    ErrorCode last_error = ErrorCode::Ok;
    std::string message;
    Quality quality_resolved;
    std::string sha;
    std::string path;              // 落盘路径
    json::Value to_json() const;
};

class DownloadManager {
public:
    DownloadManager(const LibraryPaths& paths, MetaCache& meta, LibraryIndex& index,
                    PluginRegistry& plugins);
    ~DownloadManager();

    // 注入点（宿主/测试提供；默认实现走插件 request）
    // get_url(source, item_id, quality) -> URL
    std::function<Result<std::string>(const std::string&, const std::string&, const std::string&)> resolve_url;
    // get_song(source, item_id, quality) -> BaseInfo（含 name/author/duration/quality）
    std::function<Result<BaseInfo>(const std::string&, const std::string&, const std::string&)> resolve_meta;
    // fetch(url, out_path, received, total, error)：分片写入 out_path，可被 pause/cancel 中断（返回 false + error）
    std::function<bool(const std::string&, const std::filesystem::path&, long long*, long long*, std::string*)> fetch;
    // 事件回调（1.8 / 2.10）
    std::function<void(const DownloadTask&)> on_progress;
    std::function<void(const DownloadTask&)> on_done;
    std::function<void(const DownloadTask&)> on_failed;

    // 创建任务：同曲同质已存在时按既有 meta 的 sha 判定为 Skipped（返回的 task.state == Done 且 message 说明）
    Result<DownloadTask> create(const std::string& source, const std::string& item_id,
                                const std::string& quality = "auto",
                                const std::string& filename_template = "{artist} - {title} [{quality}]");
    std::vector<DownloadTask> list() const;
    Result<DownloadTask> get(std::string_view id) const;
    Status pause(std::string_view id);
    Status resume(std::string_view id);
    Status cancel(std::string_view id);
    Result<bool> remove(std::string_view id, bool delete_file = false);
    // 执行一个排队任务（测试与宿主轮询都可用）；无任务返回 false
    bool step();
    // 任务持久化（<root>/cache/downloads.json）
    Status save() const;
    Status load();
    size_t running_count() const;

private:
    const LibraryPaths& paths_;
    MetaCache& meta_;
    LibraryIndex& index_;
    PluginRegistry& plugins_;
    std::vector<DownloadTask> tasks_;
    long long next_id_ = 1;

    DownloadTask* find_task(std::string_view id);
    const DownloadTask* find_task(std::string_view id) const;
    std::string make_task_id();
    DownloadState external_state(std::string_view id) const;

    // 执行一个已置为 Running 的任务：解析 → 下载 → 校验 → 入库 → 事件
    void run_task(const std::string& id);

    // 默认实现（未注入时使用）
    Result<BaseInfo> default_resolve_meta(const std::string& source, const std::string& item_id,
                                          const std::string& quality) const;
    Result<std::string> default_resolve_url(const std::string& source, const std::string& item_id,
                                            const std::string& quality) const;
    // 仅支持 file:// 或本地路径；网络 URL 需宿主注入 fetch
    bool default_fetch(const std::string& url, const std::filesystem::path& out_path,
                       long long* received, long long* total, std::string* error) const;
};

}  // namespace radmusic
