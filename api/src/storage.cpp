#include "radmusic/storage.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "radmusic/groupkey.hpp"
#include "radmusic/merge.hpp"
#include "radmusic/quality.hpp"

namespace radmusic {
namespace {

const char* kTmpSuffix = ".tmp";
const char* kBakSuffix = ".bak";
// 合并后同一首歌可能出现「同音质多实体」（例如两个不同在线 id 都是无损），
// 目录名无法再靠 platform+quality 区分，用 <quality_key>~2 / ~3 作为变体后缀
const char* kQualityVariantSep = "~";

std::string path_to_utf8(const std::filesystem::path& p) {
    return p.u8string().empty() ? std::string() : std::string(reinterpret_cast<const char*>(p.u8string().c_str()));
}

bool starts_with_dot(const std::filesystem::path& p) {
    const std::string name = path_to_utf8(p.filename());
    return !name.empty() && name[0] == '.';
}

// 文件级原子替换：优先平台 API，其次 rename，最后 remove+rename
Status replace_file(const std::filesystem::path& tmp, const std::filesystem::path& target) {
    std::error_code ec;
#ifdef _WIN32
    if (MoveFileExW(tmp.wstring().c_str(), target.wstring().c_str(), MOVEFILE_REPLACE_EXISTING)) {
        return Status::success();
    }
#endif
    std::filesystem::rename(tmp, target, ec);
    if (!ec) return Status::success();

    std::error_code ec2;
    std::filesystem::remove(target, ec2);
    std::filesystem::rename(tmp, target, ec2);
    if (!ec2) return Status::success();

    return Status(ErrorCode::RequestFailed, "原子替换失败: " + path_to_utf8(target) + " (" + ec2.message() + ")");
}

}  // namespace

const char* conflict_policy_id(ConflictPolicy policy) {
    switch (policy) {
        case ConflictPolicy::Merge: return "merge";
        case ConflictPolicy::Overwrite: return "overwrite";
        default: return "skip";
    }
}

ConflictPolicy conflict_policy_from_id(std::string_view id) {
    if (id == "merge") return ConflictPolicy::Merge;
    if (id == "overwrite") return ConflictPolicy::Overwrite;
    return ConflictPolicy::Skip;
}

const char* write_action_id(WriteAction action) {
    switch (action) {
        case WriteAction::Created: return "created";
        case WriteAction::Merged: return "merged";
        case WriteAction::Skipped: return "skipped";
        default: return "overwritten";
    }
}

Result<std::string> read_text_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return Result<std::string>::failure(ErrorCode::FileNotFound, "无法读取: " + path_to_utf8(path));
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return Result<std::string>::success(ss.str());
}

Status write_text_file_atomic(const std::filesystem::path& dir, const std::filesystem::path& target,
                              std::string_view content) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return Status(ErrorCode::RequestFailed, "无法创建目录: " + path_to_utf8(dir) + " (" + ec.message() + ")");
    }

    const std::filesystem::path tmp = target.parent_path() / (path_to_utf8(target.filename()) + kTmpSuffix);
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return Status(ErrorCode::RequestFailed, "无法写入临时文件: " + path_to_utf8(tmp));
        }
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.flush();
        if (!out.good()) {
            out.close();
            std::filesystem::remove(tmp, ec);
            return Status(ErrorCode::RequestFailed, "写入临时文件失败: " + path_to_utf8(tmp));
        }
    }
    return replace_file(tmp, target);
}

// 合并组的变体目录名：<quality_key>~<index>
std::string quality_key_variant(std::string_view quality_key_value, int index) {
    if (index <= 1) return std::string(quality_key_value);
    return std::string(quality_key_value) + kQualityVariantSep + std::to_string(index);
}

// ---------------------------------------------------------------- LibraryPaths

Status LibraryPaths::ensure_layout() const {
    std::error_code ec;
    const std::filesystem::path dirs[] = {
        root,          tracks(),      tracks_staging(), covers(),     index_dir(),
        presets(),     plugins(),     cache(),          meta(),       meta_staging(),
        audio_cache(), stream_cache(), lyric_cache(),  cover_cache(), tmp(),
        // v0.10：歌单 / 专辑 / MV / 日志（新增目录，旧曲库升级后自动补齐）
        library(),     playlists(),   albums(),         mv(),         logs(),
    };
    for (const auto& d : dirs) {
        std::filesystem::create_directories(d, ec);
        if (ec) {
            return Status(ErrorCode::RequestFailed, "无法创建目录: " + path_to_utf8(d) + " (" + ec.message() + ")");
        }
    }
    return Status::success();
}

// ---------------------------------------------------------------- LibraryLock

LibraryLock::~LibraryLock() { release(); }

Status LibraryLock::acquire(int timeout_ms) {
    const auto lock_path = paths_.library_lock();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::error_code ec;

    for (;;) {
        if (std::filesystem::create_directory(lock_path, ec)) {
            // 写入持有者信息（失败不影响持锁）
            std::ofstream owner(lock_path / "owner", std::ios::trunc);
            if (owner) {
                owner << "pid=" << static_cast<long long>(
#ifdef _WIN32
                                 GetCurrentProcessId()
#else
                                 getpid()
#endif
                             )
                      << "\n";
            }
            held_ = true;
            return Status::success();
        }

        // 僵尸锁：超过 15 分钟未更新视为残留
        std::error_code tec;
        const auto stamp = std::filesystem::last_write_time(lock_path, tec);
        if (!tec) {
            const auto now = std::filesystem::file_time_type::clock::now();
            const auto age = std::chrono::duration_cast<std::chrono::minutes>(now - stamp).count();
            if (age >= 15) {
                std::filesystem::remove_all(lock_path, tec);
                continue;
            }
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            return Status(ErrorCode::LibraryLocked, "曲库被其他进程或任务占用: " + path_to_utf8(lock_path));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

void LibraryLock::release() {
    if (!held_) return;
    std::error_code ec;
    std::filesystem::remove_all(paths_.library_lock(), ec);
    held_ = false;
}

// ---------------------------------------------------------------- LibraryIndex

std::string LibraryIndex::key_of(const BaseInfo& base) const {
    if (!base.sha.empty()) return "sha:" + base.sha;
    if (!base.platform.empty() && !base.item_id.empty()) return "src:" + base.platform + ":" + base.item_id;
    return "grp:" + base.group_key() + ":" + quality_key(base.platform, base.quality);
}

Status LibraryIndex::load() {
    const auto file = paths_.index_file();
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        entries_ = json::Value::object();
        return Status::success();
    }
    auto text = read_text_file(file);
    if (!text.ok()) return text.status();
    json::ParseError err;
    json::Value parsed = json::Value::parse(text.value(), &err);
    if (!parsed.is_object()) {
        return Status(ErrorCode::InvalidMetadata,
                      "index.json 解析失败: " + err.message + " @" + std::to_string(err.offset));
    }
    entries_ = parsed;
    return Status::success();
}

Status LibraryIndex::save() const {
    return write_text_file_atomic(paths_.index_dir(), paths_.index_file(), entries_.dump(2));
}

void LibraryIndex::upsert(const BaseInfo& base, const MetaPath& path) {
    if (!entries_.is_object()) entries_ = json::Value::object();
    json::Value entry = json::Value::object();
    entry.set("sha", base.sha);
    entry.set("platform", base.platform);
    if (!base.item_id.empty()) entry.set("item_id", base.item_id);
    entry.set("location", base.location);
    if (base.duration_seconds > 0) entry.set("duration", static_cast<long long>(base.duration_seconds));
    if (!base.name.empty()) entry.set("name", base.name);
    if (!base.author.empty()) entry.set("author", base.author);
    if (base.quality.is_known() || !base.quality.quality_id.empty()) entry.set("quality", base.quality.to_json());
    entry.set("song_key", path.song_key);
    entry.set("quality_key", path.quality_key);
    entry.set("meta", path_to_utf8(paths_.meta_file(path)));
    entries_.set(key_of(base), entry);
}

bool LibraryIndex::remove_by_sha(std::string_view sha) {
    if (!entries_.is_object() || sha.empty()) return false;
    bool removed = entries_.erase("sha:" + std::string(sha));
    std::vector<std::string> drop;
    for (const auto& kv : entries_.as_object()) {
        if (kv.second.get_string("sha") == sha) drop.push_back(kv.first);
    }
    for (const auto& k : drop) removed = entries_.erase(k) || removed;
    return removed;
}

std::optional<json::Value> LibraryIndex::find_by_sha(std::string_view sha) const {
    if (sha.empty()) return std::nullopt;
    const json::Value* v = entries_.find("sha:" + std::string(sha));
    if (v && v->is_object()) return *v;
    for (const auto& kv : entries_.as_object()) {
        if (kv.second.get_string("sha") == sha) return kv.second;
    }
    return std::nullopt;
}

std::optional<json::Value> LibraryIndex::find_by_platform_id(std::string_view platform,
                                                            std::string_view item_id) const {
    for (const auto& kv : entries_.as_object()) {
        if (kv.second.get_string("platform") == platform && kv.second.get_string("item_id") == item_id) {
            return kv.second;
        }
    }
    return std::nullopt;
}

std::vector<json::Value> LibraryIndex::all() const {
    std::vector<json::Value> out;
    out.reserve(entries_.size());
    for (const auto& kv : entries_.as_object()) out.push_back(kv.second);
    return out;
}

size_t LibraryIndex::size() const { return entries_.size(); }

void LibraryIndex::clear() { entries_ = json::Value::object(); }

Status LibraryIndex::rebuild_from_disk(const MetaCache& cache) {
    clear();
    for (const auto& [path, base] : cache.list_all_with_paths()) upsert(base, path);
    return save();
}

// ---------------------------------------------------------------- MetaCache

MetaPath MetaCache::path_for(const BaseInfo& base) const {
    MetaPath p;
    if (base.name.empty() && !base.item_id.empty()) {
        p.song_key = fallback_song_key(base.platform, base.item_id);
    } else {
        p.song_key = radmusic::song_key(base.name, base.author, base.duration_seconds);
    }
    // 自定义合并：属于同一合并组的曲目一律写入规范 song_key（后续导入/下载自动归组）
    if (merge_) {
        if (auto canonical = merge_->canonical_song_key(p.song_key)) p.song_key = *canonical;
    }
    p.quality_key = radmusic::quality_key(base.platform, base.quality);
    return p;
}

MetaPath MetaCache::path_for_song_quality(std::string_view song_key_value,
                                         std::string_view quality_key_value) const {
    MetaPath p;
    p.song_key = std::string(song_key_value);
    p.quality_key = std::string(quality_key_value);
    return p;
}

Status MetaCache::validate_path(const MetaPath& path, const BaseInfo& base) const {
    if (!path.valid()) return Status(ErrorCode::MetaPathMismatch, "缓存路径缺少 song_key/quality_key");
    if (path.song_key.rfind("online_", 0) == 0) {
        const std::string expect = fallback_song_key(base.platform, base.item_id);
        if (path.song_key != expect) {
            return Status(ErrorCode::MetaPathMismatch,
                          "song_key 与 platform/id 不一致: " + path.song_key + " != " + expect);
        }
    } else if (song_key_consistent(path.song_key, base.name, base.author, base.duration_seconds) == false) {
        return Status(ErrorCode::MetaPathMismatch, "song_key 无法由元数据反推: " + path.song_key);
    }
    const std::string expect_quality = radmusic::quality_key(base.platform, base.quality);
    if (path.quality_key != expect_quality) {
        // 变体后缀（合并产生的同音质多实体）：<expect_quality>~2 / ~3 …
        const std::string prefix = expect_quality + kQualityVariantSep;
        const bool variant =
            path.quality_key.size() > prefix.size() && path.quality_key.rfind(prefix, 0) == 0;
        if (!variant) {
            return Status(ErrorCode::MetaPathMismatch,
                          "quality_key 与 platform/quality 不一致: " + path.quality_key + " != " + expect_quality);
        }
    }
    return Status::success();
}

Result<BaseInfo> MetaCache::read(const MetaPath& path) const {
    if (!path.valid()) {
        return Result<BaseInfo>::failure(ErrorCode::MetaPathMismatch, "缓存路径非法");
    }
    auto file = paths_.meta_file(path);
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        // 崩溃恢复：.tmp 未替换成功时尝试 .bak
        const auto bak = file.parent_path() / (path_to_utf8(file.filename()) + kBakSuffix);
        if (std::filesystem::exists(bak, ec)) {
            file = bak;
        } else {
            return Result<BaseInfo>::failure(ErrorCode::FileNotFound, "base.json 不存在: " + path_to_utf8(file));
        }
    }
    auto text = read_text_file(file);
    if (!text.ok()) return Result<BaseInfo>(text.status());

    json::ParseError err;
    json::Value parsed = json::Value::parse(text.value(), &err);
    if (!parsed.is_object()) {
        return Result<BaseInfo>::failure(
            ErrorCode::InvalidMetadata,
            "base.json 解析失败: " + err.message + " @" + std::to_string(err.offset));
    }
    BaseInfo base = BaseInfo::from_json(parsed);
    Status v = base.validate();
    if (!v.ok()) return Result<BaseInfo>(v);
    Status consistent = validate_path(path, base);
    if (!consistent.ok()) return Result<BaseInfo>(consistent);
    return Result<BaseInfo>::success(base);
}

Result<BaseInfo> MetaCache::read_by_key(std::string_view song_key_value,
                                       std::string_view quality_key_value) const {
    return read(path_for_song_quality(song_key_value, quality_key_value));
}

std::vector<std::pair<MetaPath, BaseInfo>> MetaCache::list_all_with_paths() const {
    std::vector<std::pair<MetaPath, BaseInfo>> out;
    std::error_code ec;
    const auto root = paths_.meta();
    if (!std::filesystem::exists(root, ec)) return out;

    for (const auto& song_dir : std::filesystem::directory_iterator(root, ec)) {
        if (!song_dir.is_directory() || starts_with_dot(song_dir.path())) continue;
        for (const auto& quality_dir : std::filesystem::directory_iterator(song_dir.path(), ec)) {
            if (!quality_dir.is_directory() || starts_with_dot(quality_dir.path())) continue;
            MetaPath p;
            p.song_key = path_to_utf8(song_dir.path().filename());
            p.quality_key = path_to_utf8(quality_dir.path().filename());
            auto base = read(p);
            if (!base.ok()) continue;   // 损坏条目在 broken_paths() 中单独暴露
            out.emplace_back(p, base.value());
        }
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        if (a.first.song_key != b.first.song_key) return a.first.song_key < b.first.song_key;
        return a.first.quality_key < b.first.quality_key;
    });
    return out;
}

std::vector<BaseInfo> MetaCache::list_all() const {
    std::vector<BaseInfo> out;
    for (auto& [path, base] : list_all_with_paths()) out.push_back(base);
    return out;
}

std::vector<MetaPath> MetaCache::broken_paths() const {
    std::vector<MetaPath> out;
    std::error_code ec;
    const auto root = paths_.meta();
    if (!std::filesystem::exists(root, ec)) return out;
    for (const auto& song_dir : std::filesystem::directory_iterator(root, ec)) {
        if (!song_dir.is_directory() || starts_with_dot(song_dir.path())) continue;
        for (const auto& quality_dir : std::filesystem::directory_iterator(song_dir.path(), ec)) {
            if (!quality_dir.is_directory() || starts_with_dot(quality_dir.path())) continue;
            MetaPath p;
            p.song_key = path_to_utf8(song_dir.path().filename());
            p.quality_key = path_to_utf8(quality_dir.path().filename());
            if (!read(p).ok()) out.push_back(p);
        }
    }
    return out;
}

std::optional<MetaPath> MetaCache::find_by_sha(std::string_view sha) const {
    if (sha.empty()) return std::nullopt;
    for (const auto& [path, base] : list_all_with_paths()) {
        if (base.sha == sha) return path;
    }
    return std::nullopt;
}

std::optional<MetaPath> MetaCache::find_by_platform_id(std::string_view platform,
                                                       std::string_view item_id) const {
    for (const auto& [path, base] : list_all_with_paths()) {
        if (base.platform == platform && base.item_id == item_id) return path;
    }
    return std::nullopt;
}

std::optional<MetaPath> MetaCache::find_by_group_quality(std::string_view group_key_value,
                                                         std::string_view quality_key_value) const {
    for (const auto& [path, base] : list_all_with_paths()) {
        if (path.quality_key != quality_key_value) continue;
        const SongIdentity a{base.name, base.author, base.duration_seconds};
        SongIdentity want;
        if (!parse_group_key(group_key_value, &want)) continue;
        if (songs_fold_together(a, want)) return path;
    }
    return std::nullopt;
}

std::vector<std::pair<MetaPath, BaseInfo>> MetaCache::scan_matching(
    const std::function<bool(const BaseInfo&)>& pred) const {
    std::vector<std::pair<MetaPath, BaseInfo>> out;
    for (const auto& entry : list_all_with_paths()) {
        if (pred(entry.second)) out.push_back(entry);
    }
    return out;
}

Result<PutOutcome> MetaCache::put(const BaseInfo& base, ConflictPolicy policy) {
    // 自定义合并：先按所属合并组规范化元数据，否则落盘后 song_key 与内容不一致（1029）
    BaseInfo incoming = base;
    if (merge_) merge_->normalize(incoming);

    Status valid = incoming.validate();
    if (!valid.ok()) return Result<PutOutcome>(valid);

    const MetaPath target = path_for(incoming);
    if (!target.valid()) {
        return Result<PutOutcome>::failure(ErrorCode::MetaPathMismatch, "无法推导缓存路径");
    }

    // ---- 全局唯一性：同 sha ----
    for (const auto& [path, existing] : list_all_with_paths()) {
        if (existing.sha != incoming.sha) continue;
        if (path == target) break;    // 同路径，交给下面的逻辑
        // 同一内容已在别处：重复导入 → skip，返回已有条目（1.10）
        return Result<PutOutcome>::success(PutOutcome{WriteAction::Skipped, existing, path});
    }

    // ---- location 冲突：同一物理文件不得有第二条记录 ----
    if (!incoming.location.empty()) {
        for (const auto& [path, existing] : list_all_with_paths()) {
            if (existing.location != incoming.location) continue;
            if (path == target) continue;
            if (existing.sha == incoming.sha) {
                return Result<PutOutcome>::success(PutOutcome{WriteAction::Skipped, existing, path});
            }
            return Result<PutOutcome>::failure(
                ErrorCode::BaseConflict,
                "location 已被其它 base.json 占用: " + existing.location + " -> " + path.song_key + "/" +
                    path.quality_key);
        }
    }

    // ---- 同路径（同一首歌 + 同一音质）----
    auto existing_result = read(target);
    if (existing_result.ok()) {
        const BaseInfo& existing = existing_result.value();

        if (existing.sha == incoming.sha) {
            if (policy == ConflictPolicy::Overwrite) {
                // 同内容同路径 + 显式覆写：直接写回（如识别重新关联在线版本）
                Status w = write_text_file_atomic(paths_.meta_dir(target), paths_.meta_file(target),
                                                  incoming.to_json().dump(2));
                if (!w.ok()) return Result<PutOutcome>(w);
                return Result<PutOutcome>::success(PutOutcome{WriteAction::Overwritten, incoming, target});
            }
            if (existing.to_json().dump() == incoming.to_json().dump()) {
                return Result<PutOutcome>::success(PutOutcome{WriteAction::Skipped, existing, target});
            }
            BaseInfo merged = existing;
            auto fill = [](std::string& dst, const std::string& src) {
                if (dst.empty() && !src.empty()) dst = src;
            };
            fill(merged.author, incoming.author);
            fill(merged.pic, incoming.pic);
            fill(merged.album, incoming.album);
            fill(merged.tag_source, incoming.tag_source);
            fill(merged.item_id, incoming.item_id);
            fill(merged.match_platform, incoming.match_platform);
            if (merged.duration_seconds == 0) merged.duration_seconds = incoming.duration_seconds;
            if (merged.track_no == 0) merged.track_no = incoming.track_no;
            if (!merged.quality.is_known() && incoming.quality.is_known()) merged.quality = incoming.quality;
            Status w = write_text_file_atomic(paths_.meta_dir(target), paths_.meta_file(target),
                                              merged.to_json().dump(2));
            if (!w.ok()) return Result<PutOutcome>(w);
            return Result<PutOutcome>::success(PutOutcome{WriteAction::Merged, merged, target});
        }

        // 不同 sha：同一首歌 + 同一音质的不同实体
        if (policy == ConflictPolicy::Skip) {
            return Result<PutOutcome>::failure(
                ErrorCode::BaseConflict,
                "同一首歌同一音质已存在不同内容: 既有 sha=" + existing.sha + "，新 sha=" + incoming.sha);
        }
        if (policy == ConflictPolicy::Merge) {
            BaseInfo merged = existing;
            if (merged.duration_seconds == 0) merged.duration_seconds = incoming.duration_seconds;
            if (!merged.quality.is_known() && incoming.quality.is_known()) merged.quality = incoming.quality;
            Status w = write_text_file_atomic(paths_.meta_dir(target), paths_.meta_file(target),
                                              merged.to_json().dump(2));
            if (!w.ok()) return Result<PutOutcome>(w);
            return Result<PutOutcome>::success(PutOutcome{WriteAction::Merged, merged, target});
        }
        // Overwrite
        Status w = write_text_file_atomic(paths_.meta_dir(target), paths_.meta_file(target),
                                          incoming.to_json().dump(2));
        if (!w.ok()) return Result<PutOutcome>(w);
        return Result<PutOutcome>::success(PutOutcome{WriteAction::Overwritten, incoming, target});
    }

    // 目标不存在（或损坏）：直接创建（损坏条目视为可重建）
    Status w = write_text_file_atomic(paths_.meta_dir(target), paths_.meta_file(target),
                                      incoming.to_json().dump(2));
    if (!w.ok()) return Result<PutOutcome>(w);
    return Result<PutOutcome>::success(PutOutcome{WriteAction::Created, incoming, target});
}

Status MetaCache::write_direct(const MetaPath& path, const BaseInfo& base) {
    Status valid = base.validate();
    if (!valid.ok()) return valid;
    if (!path.valid()) return Status(ErrorCode::MetaPathMismatch, "缓存路径非法");
    Status consistent = validate_path(path, base);
    if (!consistent.ok()) return consistent;
    return write_text_file_atomic(paths_.meta_dir(path), paths_.meta_file(path), base.to_json().dump(2));
}

Status MetaCache::erase(const MetaPath& path) {
    if (!path.valid()) return Status(ErrorCode::MetaPathMismatch, "缓存路径非法");
    std::error_code ec;
    const auto dir = paths_.meta_dir(path);
    if (!std::filesystem::exists(dir, ec)) {
        return Status(ErrorCode::FileNotFound, "缓存条目不存在: " + path_to_utf8(dir));
    }
    std::filesystem::remove_all(dir, ec);
    if (ec) return Status(ErrorCode::RequestFailed, "删除失败: " + path_to_utf8(dir) + " (" + ec.message() + ")");
    return Status::success();
}

size_t MetaCache::clear_all() {
    std::error_code ec;
    size_t removed = 0;
    for (const auto& [path, base] : list_all_with_paths()) {
        (void)base;
        std::filesystem::remove_all(paths_.meta_dir(path), ec);
        if (!ec) ++removed;
    }
    // 清理暂存残留
    std::filesystem::remove_all(paths_.meta_staging(), ec);
    return removed;
}

size_t MetaCache::count() const { return list_all_with_paths().size(); }

std::uintmax_t MetaCache::bytes() const {
    std::uintmax_t total = 0;
    std::error_code ec;
    for (const auto& [path, base] : list_all_with_paths()) {
        (void)base;
        const auto file = paths_.meta_file(path);
        const auto size = std::filesystem::file_size(file, ec);
        if (!ec) total += size;
    }
    return total;
}

}  // namespace radmusic
