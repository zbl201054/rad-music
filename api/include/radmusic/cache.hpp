// Rad music API - 缓存管理（Readme.md 1.9 / 2.11）
//
// 缓存分层：meta / audio / stream / lyric / cover / tmp（1.9「缓存分层」）。
// 关键规则：
//   * 清缓存**不得**删除 tracks/、presets/、index/、usec.json、library.lock（1.9 规则 3）；
//   * 正在播放/正在下载的条目**不得被淘汰**：mark_in_use → pending eviction（1.9 规则 2）；
//   * 清理 meta 层 = 删除 base.json（其结果通过库索引 index/ 重建，1.9 规则 8 / 2.11 规则 2/3）；
//   * 音频层按 LRU（文件 mtime）淘汰到上限（1.9 规则 2 / 2.11 规则 6）。
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "radmusic/error.hpp"
#include "radmusic/model.hpp"
#include "radmusic/storage.hpp"

namespace radmusic {

struct CacheLayerStats {
    std::string name;
    size_t entries = 0;
    std::uintmax_t bytes = 0;
};

struct CacheStats {
    std::uintmax_t usage_bytes = 0;
    std::uintmax_t limit_bytes = 0;
    std::vector<CacheLayerStats> layers;   // 顺序：meta, audio, stream, lyric, cover, tmp
    size_t broken_meta = 0;
    size_t pending_eviction = 0;
};

struct CacheConfig {
    std::uintmax_t limit_meta = 64ull * 1024 * 1024;
    std::uintmax_t limit_audio = 5ull * 1024 * 1024 * 1024;
    std::uintmax_t limit_lyric = 16ull * 1024 * 1024;
    std::uintmax_t limit_cover = 64ull * 1024 * 1024;
    bool preload = false;
    json::Value to_json() const;
    static CacheConfig from_json(const json::Value& v);
};

class CacheManager {
public:
    CacheManager(const LibraryPaths& paths, MetaCache& meta, LibraryIndex& index);

    Status ensure_layout();
    CacheStats stats() const;
    // type: ""/"all" 表示全部；支持 meta/audio/stream/lyric/cover/tmp
    // 清理 meta 层 = 删除 base.json（1.9 规则 2/3）；正在播放/下载的条目标记 pending eviction 并跳过
    Result<size_t> clear(std::string_view type, size_t* skipped = nullptr);
    // 重建 meta 层：song_key/quality_key 为空表示全量（重建 = 由 MetaCache 读取现存条目并刷新库索引）
    Status rebuild_meta(std::string_view song_key = {}, std::string_view quality_key = {});
    Status set_config(const CacheConfig& config);
    CacheConfig config() const;
    // 正在播放/下载的条目（1.9 规则 2：不得被淘汰）
    void mark_in_use(std::string_view key);
    void release_in_use(std::string_view key);
    std::vector<std::string> in_use() const;
    // 按 LRU 淘汰 audio 层到上限；跳过 in_use；返回淘汰条数
    Result<size_t> evict_audio_to_limit();
    // 记录一次音频缓存访问（LRU 时间戳）
    Status touch_audio(std::string_view sha);

private:
    const LibraryPaths& paths_;
    MetaCache& meta_;
    LibraryIndex& index_;
    mutable CacheConfig config_;
    std::vector<std::string> in_use_;

    // ---- 内部工具（不改变上面的公开契约） ----
    std::filesystem::path layer_path(std::string_view layer) const;
    bool key_in_use(std::string_view key) const;
    bool file_in_use(const std::filesystem::path& file) const;
    // cache/cache.json → config_（失败静默，返回 false）
    bool load_config();
};

}  // namespace radmusic
