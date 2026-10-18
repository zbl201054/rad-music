// Rad music API - 缓存管理实现（Readme.md 1.9 / 2.11）
//
// 不抛异常：所有文件系统操作均使用 error_code 重载，失败以 Status 表达。
#include "radmusic/cache.hpp"

#include <algorithm>
#include <chrono>
#include <system_error>

#include "radmusic/groupkey.hpp"
#include "radmusic/json.hpp"
#include "radmusic/quality.hpp"

namespace radmusic {
namespace {

// 层顺序固定：meta, audio, stream, lyric, cover, tmp（1.9「缓存分层」）
const char* const kLayers[] = {"meta", "audio", "stream", "lyric", "cover", "tmp"};
constexpr size_t kLayerCount = sizeof(kLayers) / sizeof(kLayers[0]);
const char* const kConfigFile = "cache.json";

std::string path_to_utf8(const std::filesystem::path& p) {
    const auto u8 = p.u8string();
    return u8.empty() ? std::string() : std::string(reinterpret_cast<const char*>(u8.c_str()));
}

std::string lower_ascii(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>((c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c));
    }
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

// 递归列出目录下的普通文件（顺序稳定：按路径字典序）
std::vector<std::filesystem::path> list_files_recursive(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return out;
    std::filesystem::recursive_directory_iterator it(
        dir, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) return out;
    for (; it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        std::error_code fec;
        if (!it->is_regular_file(fec) || fec) continue;
        out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

// 删除空的子目录（自底向上），层目录自身保留
void prune_empty_dirs(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return;
    std::vector<std::filesystem::path> dirs;
    std::filesystem::recursive_directory_iterator it(
        dir, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) return;
    for (; it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        std::error_code dec;
        if (it->is_directory(dec) && !dec) dirs.push_back(it->path());
    }
    std::sort(dirs.begin(), dirs.end(), [](const std::filesystem::path& a, const std::filesystem::path& b) {
        return path_to_utf8(a).size() > path_to_utf8(b).size();
    });
    for (const auto& d : dirs) {
        std::error_code rec;
        if (std::filesystem::is_empty(d, rec) && !rec) std::filesystem::remove(d, rec);
    }
}

// 解析层的名字列表；支持 "a,b,c" 与 ""/"all"；未知层名返回 false
bool parse_layers(std::string_view type, std::vector<std::string>* out) {
    const std::string_view t = trim(type);
    auto push_all = [out]() {
        for (const char* name : kLayers) {
            if (std::find(out->begin(), out->end(), name) == out->end()) out->push_back(name);
        }
    };
    if (t.empty() || lower_ascii(t) == "all") {
        push_all();
        return true;
    }
    size_t start = 0;
    for (;;) {
        const size_t comma = t.find(',', start);
        const std::string_view token =
            trim(comma == std::string_view::npos ? t.substr(start) : t.substr(start, comma - start));
        if (!token.empty()) {
            const std::string name = lower_ascii(token);
            if (name == "all") {
                push_all();
            } else {
                bool known = false;
                for (const char* layer : kLayers) {
                    if (name == layer) {
                        known = true;
                        if (std::find(out->begin(), out->end(), name) == out->end()) out->push_back(name);
                        break;
                    }
                }
                if (!known) return false;
            }
        }
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return !out->empty();
}

std::uintmax_t dir_bytes(const std::filesystem::path& dir) {
    std::uintmax_t total = 0;
    for (const auto& f : list_files_recursive(dir)) {
        std::error_code ec;
        const auto size = std::filesystem::file_size(f, ec);
        if (!ec) total += size;
    }
    return total;
}

}  // namespace

// ---------------------------------------------------------------- CacheConfig

json::Value CacheConfig::to_json() const {
    json::Value v = json::Value::object();
    v.set("limit_meta", static_cast<unsigned long long>(limit_meta));
    v.set("limit_audio", static_cast<unsigned long long>(limit_audio));
    v.set("limit_lyric", static_cast<unsigned long long>(limit_lyric));
    v.set("limit_cover", static_cast<unsigned long long>(limit_cover));
    v.set("preload", preload);
    return v;
}

CacheConfig CacheConfig::from_json(const json::Value& v) {
    CacheConfig c;
    if (!v.is_object()) return c;
    auto read_limit = [&v](const char* key, std::uintmax_t fallback) -> std::uintmax_t {
        const json::Value* n = v.find(key);
        if (!n || !n->is_number()) return fallback;
        const int64_t value = n->as_int(static_cast<int64_t>(fallback));
        return value < 0 ? 0 : static_cast<std::uintmax_t>(value);
    };
    c.limit_meta = read_limit("limit_meta", c.limit_meta);
    c.limit_audio = read_limit("limit_audio", c.limit_audio);
    c.limit_lyric = read_limit("limit_lyric", c.limit_lyric);
    c.limit_cover = read_limit("limit_cover", c.limit_cover);
    c.preload = v.get_bool("preload", c.preload);
    return c;
}

// ---------------------------------------------------------------- CacheManager

CacheManager::CacheManager(const LibraryPaths& paths, MetaCache& meta, LibraryIndex& index)
    : paths_(paths), meta_(meta), index_(index) {
    load_config();   // 配置位于 <root>/cache/cache.json；缺失/损坏时用默认值
}

Status CacheManager::ensure_layout() {
    const Status layout = paths_.ensure_layout();
    if (!layout.ok()) return layout;
    load_config();
    return Status::success();
}

bool CacheManager::load_config() {
    const auto file = paths_.cache() / kConfigFile;
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) return false;
    const auto text = read_text_file(file);
    if (!text.ok()) return false;
    json::ParseError err;
    const json::Value parsed = json::Value::parse(text.value(), &err);
    if (!parsed.is_object()) return false;
    config_ = CacheConfig::from_json(parsed);
    return true;
}

std::filesystem::path CacheManager::layer_path(std::string_view layer) const {
    if (layer == "meta") return paths_.meta();
    if (layer == "audio") return paths_.audio_cache();
    if (layer == "stream") return paths_.stream_cache();
    if (layer == "lyric") return paths_.lyric_cache();
    if (layer == "cover") return paths_.cover_cache();
    if (layer == "tmp") return paths_.tmp();
    return {};
}

bool CacheManager::key_in_use(std::string_view key) const {
    if (key.empty()) return false;
    return std::find(in_use_.begin(), in_use_.end(), key) != in_use_.end();
}

bool CacheManager::file_in_use(const std::filesystem::path& file) const {
    const std::string name = path_to_utf8(file.filename());
    const std::string stem = path_to_utf8(file.stem());
    return key_in_use(name) || key_in_use(stem);
}

CacheStats CacheManager::stats() const {
    CacheStats out;
    for (const char* name : kLayers) {
        CacheLayerStats layer;
        layer.name = name;
        for (const auto& file : list_files_recursive(layer_path(name))) {
            std::error_code ec;
            const auto size = std::filesystem::file_size(file, ec);
            ++layer.entries;
            if (!ec) layer.bytes += size;
        }
        out.usage_bytes += layer.bytes;
        out.layers.push_back(layer);
    }
    out.limit_bytes = config_.limit_meta + config_.limit_audio + config_.limit_lyric + config_.limit_cover;
    out.broken_meta = meta_.broken_paths().size();
    out.pending_eviction = in_use_.size();
    return out;
}

Result<size_t> CacheManager::clear(std::string_view type, size_t* skipped) {
    std::vector<std::string> layers;
    if (!parse_layers(type, &layers)) {
        return Result<size_t>::failure(ErrorCode::InvalidMetadata, "未知缓存层: " + std::string(type));
    }

    size_t removed = 0;
    size_t skipped_count = 0;
    bool meta_cleared = false;

    for (const auto& layer : layers) {
        if (layer == "meta") {
            meta_cleared = true;
            const auto root = paths_.meta();
            std::error_code ec;
            if (!std::filesystem::exists(root, ec)) continue;
            const std::filesystem::directory_iterator end;
            // 显式 increment(ec)：任何一步失败都以 error_code 表达，绝不抛出
            for (std::filesystem::directory_iterator song_it(root, ec); song_it != end;
                 song_it.increment(ec)) {
                if (ec) break;
                std::error_code sec;
                if (!song_it->is_directory(sec) || sec) continue;
                const std::string song_name = path_to_utf8(song_it->path().filename());
                if (!song_name.empty() && song_name[0] == '.') continue;   // .staging 等
                std::error_code inner_ec;
                for (std::filesystem::directory_iterator quality_it(song_it->path(), inner_ec);
                     quality_it != end; quality_it.increment(inner_ec)) {
                    if (inner_ec) break;
                    std::error_code qec;
                    if (!quality_it->is_directory(qec) || qec) continue;
                    const std::string quality_name = path_to_utf8(quality_it->path().filename());
                    if (!quality_name.empty() && quality_name[0] == '.') continue;

                    const MetaPath path = meta_.path_for_song_quality(song_name, quality_name);
                    bool in_use = key_in_use(song_name) || key_in_use(quality_name) ||
                                  key_in_use(song_name + "/" + quality_name);
                    if (!in_use) {
                        const auto existing = meta_.read(path);
                        if (existing.ok()) in_use = key_in_use(existing.value().sha);
                    }
                    if (in_use) {
                        // 1.9 规则 2 / 2.11 规则 4：pending eviction，跳过并计入 skipped
                        ++skipped_count;
                        continue;
                    }
                    std::error_code rec;
                    std::filesystem::remove_all(quality_it->path(), rec);
                    if (!rec) ++removed;
                }
            }
            prune_empty_dirs(root);
            continue;
        }

        // 其余层：逐文件删除（绝不触碰 tracks/、presets/、index/、usec.json、library.lock）
        const auto dir = layer_path(layer);
        if (dir.empty()) continue;
        for (const auto& file : list_files_recursive(dir)) {
            if (file_in_use(file)) {
                ++skipped_count;
                continue;
            }
            std::error_code rec;
            std::filesystem::remove(file, rec);
            if (!rec) ++removed;
        }
        prune_empty_dirs(dir);
    }

    if (meta_cleared) {
        // meta 层是缓存，重建来源是库索引（1.9 规则 8 / 2.11 规则 2/3）
        Status refreshed;
        if (meta_.count() == 0) {
            index_.clear();
            refreshed = index_.save();
        } else {
            refreshed = index_.rebuild_from_disk(meta_);
        }
        if (!refreshed.ok()) {
            if (skipped) *skipped = skipped_count;
            return Result<size_t>(refreshed);
        }
    }

    if (skipped) *skipped = skipped_count;
    return Result<size_t>::success(removed);
}

Status CacheManager::rebuild_meta(std::string_view song_key, std::string_view quality_key) {
    const bool full = song_key.empty() && quality_key.empty();
    size_t matched = 0;
    for (const auto& entry : meta_.list_all_with_paths()) {
        const MetaPath& path = entry.first;
        if (!full) {
            if (!song_key.empty() && path.song_key != song_key) continue;
            if (!quality_key.empty() && path.quality_key != quality_key) continue;
        }
        // 重建必须落在同一 song_key/quality_key 路径（1.9 规则 8）：同内容重写 → Skipped/Merged
        const auto outcome = meta_.put(entry.second, ConflictPolicy::Skip);
        if (!outcome.ok()) return outcome.status();
        ++matched;
    }
    if (!full && matched == 0) {
        return Status(ErrorCode::BaseRebuildFailed,
                      "无可重建的 base.json 条目: song_key=" + std::string(song_key) +
                          " quality_key=" + std::string(quality_key));
    }
    return index_.rebuild_from_disk(meta_);
}

Status CacheManager::set_config(const CacheConfig& config) {
    config_ = config;
    const Status saved = write_text_file_atomic(paths_.cache(), paths_.cache() / kConfigFile,
                                                config_.to_json().dump(2));
    if (!saved.ok()) return saved;
    // 2.11 规则 6：缩小上限时按 LRU 立即淘汰（in_use 的条目仍被跳过）
    if (dir_bytes(paths_.audio_cache()) > config_.limit_audio) {
        const auto evicted = evict_audio_to_limit();
        if (!evicted.ok()) return evicted.status();
    }
    return Status::success();
}

CacheConfig CacheManager::config() const { return config_; }

void CacheManager::mark_in_use(std::string_view key) {
    if (key.empty()) return;
    const std::string value(key);
    if (std::find(in_use_.begin(), in_use_.end(), value) == in_use_.end()) in_use_.push_back(value);
}

void CacheManager::release_in_use(std::string_view key) {
    const std::string value(key);
    in_use_.erase(std::remove(in_use_.begin(), in_use_.end(), value), in_use_.end());
}

std::vector<std::string> CacheManager::in_use() const { return in_use_; }

Result<size_t> CacheManager::evict_audio_to_limit() {
    struct Entry {
        std::filesystem::path path;
        std::filesystem::file_time_type stamp;
        std::uintmax_t bytes = 0;
    };

    std::vector<Entry> entries;
    std::uintmax_t total = 0;
    for (const auto& file : list_files_recursive(paths_.audio_cache())) {
        std::error_code sec;
        const auto size = std::filesystem::file_size(file, sec);
        if (sec) continue;
        std::error_code tec;
        auto stamp = std::filesystem::last_write_time(file, tec);
        if (tec) stamp = std::filesystem::file_time_type::min();
        entries.push_back(Entry{file, stamp, size});
        total += size;
    }
    if (total <= config_.limit_audio) return Result<size_t>::success(0);

    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.stamp != b.stamp) return a.stamp < b.stamp;   // mtime 最旧优先
        return path_to_utf8(a.path) < path_to_utf8(b.path);
    });

    size_t evicted = 0;
    for (const auto& entry : entries) {
        if (total <= config_.limit_audio) break;
        if (file_in_use(entry.path)) continue;   // 1.9 规则 2：正在播放/下载的条目不得淘汰
        std::error_code rec;
        std::filesystem::remove(entry.path, rec);
        if (rec) continue;
        total = (total > entry.bytes) ? (total - entry.bytes) : 0;
        ++evicted;
    }
    prune_empty_dirs(paths_.audio_cache());
    return Result<size_t>::success(evicted);
}

Status CacheManager::touch_audio(std::string_view sha) {
    if (sha.empty()) return Status(ErrorCode::FileNotFound, "音频缓存键为空");
    const std::string key(sha);
    size_t touched = 0;
    for (const auto& file : list_files_recursive(paths_.audio_cache())) {
        if (path_to_utf8(file.stem()) != key && path_to_utf8(file.filename()) != key) continue;
        std::error_code ec;
        std::filesystem::last_write_time(file, std::filesystem::file_time_type::clock::now(), ec);
        if (!ec) ++touched;
    }
    if (touched == 0) {
        return Status(ErrorCode::FileNotFound, "音频缓存不存在: " + key);
    }
    return Status::success();
}

}  // namespace radmusic
