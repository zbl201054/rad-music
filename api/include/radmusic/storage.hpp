// Rad music API - 目录约定、元数据缓存与库索引（Readme.md 1.1 / 1.9 / 1.10 / 4）
//
// 关键规则（1.1「存放位置」）：
//   cache/meta/<song_key>/<quality_key>/base.json
//   一首歌一个文件夹、每个音质一个文件夹 → 路径即唯一键，结构上防止覆写。
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "radmusic/error.hpp"
#include "radmusic/json.hpp"
#include "radmusic/model.hpp"

namespace radmusic {

class MergeStore;   // merge.hpp：自定义合并（构造后由 set_merge_store 装配，未装配则行为不变）

// ---------------- 写冲突策略（1.10「冲突类型与处理」） ----------------
enum class ConflictPolicy { Skip, Merge, Overwrite };
const char* conflict_policy_id(ConflictPolicy policy);
ConflictPolicy conflict_policy_from_id(std::string_view id);

enum class WriteAction { Created, Merged, Skipped, Overwritten };
const char* write_action_id(WriteAction action);

// 合并后同音质多实体的目录名：<quality_key>~2 / ~3（见 MetaCache::write_direct 的调用方）
std::string quality_key_variant(std::string_view quality_key_value, int index);

struct MetaPath {
    std::string song_key;
    std::string quality_key;

    bool valid() const { return !song_key.empty() && !quality_key.empty(); }
    bool operator==(const MetaPath& other) const {
        return song_key == other.song_key && quality_key == other.quality_key;
    }
    bool operator!=(const MetaPath& other) const { return !(*this == other); }
};

struct PutOutcome {
    WriteAction action = WriteAction::Created;
    BaseInfo base;          // 落盘后的最终记录
    MetaPath path;
};

// ---------------- 目录布局（Readme 第 4 节） ----------------
struct LibraryPaths {
    std::filesystem::path root;

    explicit LibraryPaths(std::filesystem::path root_path) : root(std::move(root_path)) {}

    std::filesystem::path tracks() const { return root / "tracks"; }
    std::filesystem::path tracks_staging() const { return tracks() / ".staging"; }
    std::filesystem::path track_dir(std::string_view sha) const { return tracks() / std::string(sha); }
    std::filesystem::path audio_file(std::string_view sha, std::string_view ext) const {
        return track_dir(sha) / ("audio." + std::string(ext));
    }
    std::filesystem::path sec_file(std::string_view sha) const { return track_dir(sha) / "sec.json"; }
    std::filesystem::path lyric_file(std::string_view sha, std::string_view suffix = "lrc") const {
        return track_dir(sha) / ("lyric." + std::string(suffix));
    }
    std::filesystem::path covers() const { return root / "covers"; }
    std::filesystem::path index_dir() const { return root / "index"; }
    std::filesystem::path index_file() const { return index_dir() / "index.json"; }
    std::filesystem::path library_lock() const { return root / "library.lock"; }
    std::filesystem::path usec_file() const { return root / "usec.json"; }
    std::filesystem::path presets() const { return root / "presets"; }
    std::filesystem::path plugins() const { return root / "plugins"; }

    // 歌单 / 专辑 / MV / 日志（v0.10 新增；全部是新增目录，旧曲库缺省也合法）
    std::filesystem::path library() const { return root / "library"; }
    std::filesystem::path playlists() const { return library() / "playlists"; }
    std::filesystem::path playlist_file(std::string_view id) const {
        return playlists() / (std::string(id) + ".json");
    }
    std::filesystem::path albums() const { return library() / "albums"; }
    std::filesystem::path album_file(std::string_view id) const {
        return albums() / (std::string(id) + ".json");
    }
    // MV：mv/<sha>/mv.json + 视频文件（组织方式与 tracks/<sha>/ 对齐）
    std::filesystem::path mv() const { return root / "mv"; }
    std::filesystem::path mv_dir(std::string_view sha) const { return mv() / std::string(sha); }
    std::filesystem::path mv_file(std::string_view sha) const { return mv_dir(sha) / "mv.json"; }
    std::filesystem::path logs() const { return root / "logs"; }

    // cache 层（1.9）
    std::filesystem::path cache() const { return root / "cache"; }
    std::filesystem::path meta() const { return cache() / "meta"; }
    std::filesystem::path meta_staging() const { return meta() / ".staging"; }
    std::filesystem::path meta_dir(const MetaPath& p) const {
        return meta() / p.song_key / p.quality_key;
    }
    std::filesystem::path meta_file(const MetaPath& p) const { return meta_dir(p) / "base.json"; }
    std::filesystem::path audio_cache() const { return cache() / "audio"; }
    std::filesystem::path stream_cache() const { return cache() / "stream"; }
    std::filesystem::path lyric_cache() const { return cache() / "lyric"; }
    std::filesystem::path cover_cache() const { return cache() / "cover"; }
    std::filesystem::path tmp() const { return cache() / "tmp"; }

    // 幂等创建全部目录
    Status ensure_layout() const;
};

// ---------------- 库写锁（library.lock，防并发导入冲突） ----------------
class LibraryLock {
public:
    explicit LibraryLock(const LibraryPaths& paths) : paths_(paths) {}
    ~LibraryLock();

    // 成功：返回 Ok；已被占用：1026 library locked
    Status acquire(int timeout_ms = 2000);
    void release();
    bool held() const { return held_; }

private:
    const LibraryPaths& paths_;
    bool held_ = false;
};

// ---------------- 库索引（持久指针，1.10 规则 5） ----------------
// 唯一键 → 音频位置/缓存路径；索引与磁盘不一致时以磁盘为准并重建。
class LibraryIndex {
public:
    explicit LibraryIndex(const LibraryPaths& paths) : paths_(paths) {}

    Status load();
    Status save() const;

    void upsert(const BaseInfo& base, const MetaPath& path);
    bool remove_by_sha(std::string_view sha);
    std::optional<json::Value> find_by_sha(std::string_view sha) const;
    std::optional<json::Value> find_by_platform_id(std::string_view platform, std::string_view item_id) const;
    std::vector<json::Value> all() const;
    size_t size() const;
    void clear();
    // 以磁盘为准重建（扫描 cache/meta）
    Status rebuild_from_disk(const class MetaCache& cache);

    const json::Value& raw() const { return entries_; }

private:
    const LibraryPaths& paths_;
    json::Value entries_ = json::Value::object();   // { sha|platform:id : {...} }
    std::string key_of(const BaseInfo& base) const;
};

// ---------------- 元数据缓存（base.json 的读写与防覆写） ----------------
class MetaCache {
public:
    explicit MetaCache(const LibraryPaths& paths) : paths_(paths) {}

    const LibraryPaths& paths() const { return paths_; }

    // 自定义合并（merge.hpp）：装配后 path_for 会把属于同一合并组的 song_key 重定向到规范键，
    // put 会把元数据规范化（否则落盘后 song_key 与内容不一致 → 1029 损坏条目）
    void set_merge_store(const MergeStore* merge) { merge_ = merge; }
    const MergeStore* merge_store() const { return merge_; }

    // 由元数据推导路径（1.1）：song_key ← group_key，quality_key ← platform + quality
    MetaPath path_for(const BaseInfo& base) const;
    MetaPath path_for_song_quality(std::string_view song_key, std::string_view quality_key) const;

    // 目录名与内容一致性（1029）：不一致即损坏条目
    Status validate_path(const MetaPath& path, const BaseInfo& base) const;

    Result<BaseInfo> read(const MetaPath& path) const;
    Result<BaseInfo> read_by_key(std::string_view song_key, std::string_view quality_key) const;

    std::vector<std::pair<MetaPath, BaseInfo>> list_all_with_paths() const;
    std::vector<BaseInfo> list_all() const;

    std::optional<MetaPath> find_by_sha(std::string_view sha) const;
    std::optional<MetaPath> find_by_platform_id(std::string_view platform,
                                                std::string_view item_id) const;
    // 同一首歌 + 同一音质（唯一键）
    std::optional<MetaPath> find_by_group_quality(std::string_view group_key_value,
                                                  std::string_view quality_key_value) const;

    // 写入（防覆写 + 原子提交）：
    //  目标不存在 → Created
    //  同 sha → 内容一致 Skipped / 有可补全字段 Merged
    //  不同 sha 但同一 (song, quality) → Skip 策略返回 1023；Merge 合并；Overwrite 覆盖
    //  同 sha 已存在于其它路径 / location 冲突 → 1023（或 Skipped，若为同一内容重复导入）
    Result<PutOutcome> put(const BaseInfo& base, ConflictPolicy policy = ConflictPolicy::Skip);

    // 直接落盘（不做唯一性/冲突检查）：供「自定义合并」搬移条目使用，
    // 调用方需自行保证路径唯一（同音质冲突时用 quality_key 加 ~2/~3 变体后缀）
    Status write_direct(const MetaPath& path, const BaseInfo& base);

    Status erase(const MetaPath& path);
    size_t clear_all();                       // 返回删除的条目数
    size_t count() const;
    std::uintmax_t bytes() const;
    // 损坏条目（目录名与内容不一致 / JSON 非法，见 1029）
    std::vector<MetaPath> broken_paths() const;

private:
    const LibraryPaths& paths_;
    const MergeStore* merge_ = nullptr;
    // 全库扫描查找（同曲同质 / location 冲突检测）；返回 (path, base)
    std::vector<std::pair<MetaPath, BaseInfo>> scan_matching(
        const std::function<bool(const BaseInfo&)>& pred) const;
};

// 文本文件工具（供其它模块复用）
Result<std::string> read_text_file(const std::filesystem::path& path);
Status write_text_file_atomic(const std::filesystem::path& dir, const std::filesystem::path& target,
                              std::string_view content);

}  // namespace radmusic
