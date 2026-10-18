#include "radmusic/merge.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>

#include "radmusic/groupkey.hpp"
#include "radmusic/storage.hpp"

namespace radmusic {
namespace {

constexpr int kMergeVersion = 1;

std::string merge_file_path(const LibraryPaths& paths) {
    return (paths.library() / "merge.json").u8string();
}

long long now_epoch_seconds() {
    return static_cast<long long>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

std::vector<std::string> string_array(const json::Value& v) {
    std::vector<std::string> out;
    if (!v.is_array()) return out;
    for (const auto& item : v.as_array()) {
        const std::string s = item.as_string();
        if (!s.empty()) out.push_back(s);
    }
    return out;
}

json::Value string_array_json(const std::vector<std::string>& values) {
    json::Value arr = json::Value::array();
    for (const auto& value : values) arr.push_back(value);
    return arr;
}

}  // namespace

std::string song_key_of_base(const BaseInfo& base) {
    if (base.name.empty() && !base.item_id.empty()) {
        return fallback_song_key(base.platform, base.item_id);
    }
    return song_key(base.name, base.author, base.duration_seconds);
}

// ---------------------------------------------------------------- MergeMember

json::Value MergeMember::to_json() const {
    json::Value v = json::Value::object();
    v.set("song_key", song_key);
    v.set("title", title);
    v.set("artist", artist);
    v.set("duration", static_cast<long long>(duration_seconds));
    v.set("shas", string_array_json(shas));
    return v;
}

MergeMember MergeMember::from_json(const json::Value& v) {
    MergeMember m;
    if (!v.is_object()) return m;
    m.song_key = v.get_string("song_key");
    m.title = v.get_string("title");
    m.artist = v.get_string("artist");
    m.duration_seconds = static_cast<int>(v.get_int("duration", v.get_int("duration_seconds", 0)));
    if (const json::Value* shas = v.find("shas")) m.shas = string_array(*shas);
    return m;
}

// ---------------------------------------------------------------- MergeGroup

json::Value MergeGroup::to_json() const {
    json::Value v = json::Value::object();
    v.set("song_key", song_key);
    v.set("title", title);
    v.set("artist", artist);
    v.set("duration", static_cast<long long>(duration_seconds));
    v.set("created_at", static_cast<long long>(created_at));
    json::Value members = json::Value::array();
    for (const auto& member : this->members) members.push_back(member.to_json());
    v.set("members", members);
    return v;
}

MergeGroup MergeGroup::from_json(const json::Value& v) {
    MergeGroup g;
    if (!v.is_object()) return g;
    g.song_key = v.get_string("song_key");
    g.title = v.get_string("title");
    g.artist = v.get_string("artist");
    g.duration_seconds = static_cast<int>(v.get_int("duration", v.get_int("duration_seconds", 0)));
    g.created_at = static_cast<long long>(v.get_int("created_at", 0));
    if (const json::Value* members = v.find("members"); members && members->is_array()) {
        for (const auto& item : members->as_array()) g.members.push_back(MergeMember::from_json(item));
    }
    return g;
}

// ---------------------------------------------------------------- MergeStore

Status MergeStore::load() {
    groups_.clear();
    const std::filesystem::path file = merge_file_path(paths_);
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) return Status::success();

    auto text = read_text_file(file);
    if (!text.ok()) return text.status();
    if (text.value().empty()) return Status::success();

    json::ParseError err;
    json::Value parsed = json::Value::parse(text.value(), &err);
    if (!parsed.is_object()) {
        return Status(ErrorCode::InvalidMetadata,
                      "merge.json 解析失败: " + err.message + " @" + std::to_string(err.offset));
    }
    if (const json::Value* groups = parsed.find("groups"); groups && groups->is_array()) {
        for (const auto& item : groups->as_array()) {
            MergeGroup group = MergeGroup::from_json(item);
            if (group.song_key.empty()) continue;
            groups_.push_back(group);
        }
    }
    return Status::success();
}

Status MergeStore::save() const {
    const std::filesystem::path file = merge_file_path(paths_);
    return write_text_file_atomic(paths_.library(), file, to_json().dump(2));
}

json::Value MergeStore::to_json() const {
    json::Value v = json::Value::object();
    v.set("version", static_cast<long long>(kMergeVersion));
    json::Value groups = json::Value::array();
    for (const auto& group : groups_) groups.push_back(group.to_json());
    v.set("groups", groups);
    return v;
}

const MergeGroup* MergeStore::find(std::string_view song_key_value) const {
    if (song_key_value.empty()) return nullptr;
    for (const auto& group : groups_) {
        if (group.song_key == song_key_value) return &group;
        for (const auto& member : group.members) {
            if (member.song_key == song_key_value) return &group;
        }
    }
    return nullptr;
}

MergeGroup* MergeStore::find_mutable(std::string_view song_key_value) {
    if (song_key_value.empty()) return nullptr;
    for (auto& group : groups_) {
        if (group.song_key == song_key_value) return &group;
        for (const auto& member : group.members) {
            if (member.song_key == song_key_value) return &group;
        }
    }
    return nullptr;
}

std::optional<std::string> MergeStore::canonical_song_key(std::string_view song_key_value) const {
    const MergeGroup* group = find(song_key_value);
    if (!group) return std::nullopt;
    if (group->song_key == song_key_value) return std::nullopt;   // 已是规范键，无需重定向
    return group->song_key;
}

bool MergeStore::is_merged(std::string_view song_key_value) const {
    const MergeGroup* group = find(song_key_value);
    return group != nullptr && !group->members.empty();
}

bool MergeStore::normalize(BaseInfo& base) const {
    const std::string key = song_key_of_base(base);
    const MergeGroup* group = find(key);
    if (!group) return false;
    if (base.name == group->title && base.author == group->artist &&
        base.duration_seconds == group->duration_seconds) {
        return false;
    }
    // 原始身份只在第一次改写时记录（重复合并时不覆盖，保证能还原到最初的标签）
    if (base.orig_name.empty()) base.orig_name = base.name;
    if (base.orig_author.empty()) base.orig_author = base.author;
    if (base.orig_duration == 0) base.orig_duration = base.duration_seconds;
    base.name = group->title;
    base.author = group->artist;
    base.duration_seconds = group->duration_seconds;
    return true;
}

void MergeStore::restore(BaseInfo& base, std::string_view fallback_title,
                         std::string_view fallback_artist, int fallback_duration) {
    const std::string title = base.orig_name.empty() ? std::string(fallback_title) : base.orig_name;
    const std::string artist = base.orig_author.empty() ? std::string(fallback_artist) : base.orig_author;
    const int duration = base.orig_duration > 0 ? base.orig_duration : fallback_duration;
    base.name = title.empty() ? base.name : title;
    base.author = artist.empty() ? base.author : artist;
    if (duration >= 0) base.duration_seconds = duration;
    base.orig_name.clear();
    base.orig_author.clear();
    base.orig_duration = 0;
}

Status MergeStore::add_or_update(const MergeGroup& group) {
    if (group.song_key.empty()) {
        return Status(ErrorCode::InvalidMetadata, "合并组缺少 song_key");
    }
    if (group.title.empty()) {
        return Status(ErrorCode::InvalidMetadata, "合并组缺少标题");
    }
    MergeGroup incoming = group;
    if (incoming.created_at <= 0) incoming.created_at = now_epoch_seconds();

    for (auto& existing : groups_) {
        if (existing.song_key != incoming.song_key) continue;
        existing.title = incoming.title;
        existing.artist = incoming.artist;
        existing.duration_seconds = incoming.duration_seconds;
        // 成员按 song_key 合并，sha 取并集（重复合并/新增音质都能追踪到）
        for (const auto& member : incoming.members) {
            auto it = std::find_if(existing.members.begin(), existing.members.end(),
                                   [&](const MergeMember& m) { return m.song_key == member.song_key; });
            if (it == existing.members.end()) {
                existing.members.push_back(member);
                continue;
            }
            it->title = member.title.empty() ? it->title : member.title;
            it->artist = member.artist.empty() ? it->artist : member.artist;
            if (member.duration_seconds > 0) it->duration_seconds = member.duration_seconds;
            for (const auto& sha : member.shas) {
                if (std::find(it->shas.begin(), it->shas.end(), sha) == it->shas.end()) {
                    it->shas.push_back(sha);
                }
            }
        }
        return save();
    }
    groups_.push_back(incoming);
    return save();
}

bool MergeStore::remove(std::string_view song_key_value) {
    const std::string key(song_key_value);
    const size_t before = groups_.size();
    groups_.erase(std::remove_if(groups_.begin(), groups_.end(),
                                 [&](const MergeGroup& group) {
                                     if (group.song_key == key) return true;
                                     for (const auto& member : group.members) {
                                         if (member.song_key == key) return true;
                                     }
                                     return false;
                                 }),
                  groups_.end());
    if (groups_.size() == before) return false;
    (void)save();
    return true;
}

bool MergeStore::remove_member(std::string_view song_key_value, std::string_view member_song_key) {
    MergeGroup* group = find_mutable(song_key_value);
    if (!group) return false;
    const std::string member_key(member_song_key);
    const size_t before = group->members.size();
    group->members.erase(std::remove_if(group->members.begin(), group->members.end(),
                                        [&](const MergeMember& member) {
                                            return member.song_key == member_key;
                                        }),
                         group->members.end());
    const bool changed = group->members.size() != before;
    // 只剩一个成员时「合并」没有意义，整组撤销
    if (group->members.size() <= 1) return remove(group->song_key);
    if (!changed) return false;
    (void)save();
    return true;
}

}  // namespace radmusic
