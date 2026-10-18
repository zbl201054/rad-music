// Rad music API - 歌单 / 专辑 / MV 持久化（Readme 1.11–1.13 / 2.14–2.16）
//
// 目录约定（与 storage.hpp 的 LibraryPaths 完全一致）：
//   library/playlists/<id>.json     一个歌单一个文件
//   library/albums/<id>.json        一个专辑一个文件
//   mv/<sha>/mv.json                一个 MV 一个目录（与 tracks/<sha>/ 对齐）
//
// 设计取舍：
//  * 文件名即主键：读取时 id/sha 以文件（目录）名为准，避免 JSON 与路径不一致；
//  * 写入统一走 write_text_file_atomic(dir, target, dump(2))（先 .tmp 再原子替换）；
//  * list() 容错：单条损坏只跳过该条，整体仍成功；get() 命中损坏文件 → 1007；
//  * 无操作（重复 add_track、删除不存在的曲目、移动到原位）不刷新 updated_at，
//    避免同步轮询把列表顺序搅乱；真正的修改（rename/add/remove/move/replace）才刷新；
//  * 不抛异常：目录遍历一律用 error_code 版本，不使用 range-for 的 operator++。
#include "radmusic/collections.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <system_error>
#include <unordered_set>
#include <utility>

#include "radmusic/groupkey.hpp"
#include "radmusic/model.hpp"

namespace radmusic {
namespace {

const char* kJsonExt = ".json";

std::string path_to_utf8(const std::filesystem::path& p) {
    const auto utf8 = p.u8string();
    return utf8.empty() ? std::string() : std::string(reinterpret_cast<const char*>(utf8.c_str()));
}

// 单个路径分量是否可安全用作文件名（拒绝分隔符、盘符、通配符与控制字符）
bool is_safe_component(std::string_view s) {
    if (s.empty() || s == "." || s == "..") return false;
    for (char c : s) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|' || static_cast<unsigned char>(c) < 0x20 ||
            static_cast<unsigned char>(c) == 0x7F) {
            return false;
        }
    }
    return true;
}

std::string join_key(std::string_view platform, std::string_view item_id) {
    std::string key(platform);
    key.push_back(':');
    key += item_id;
    return key;
}

Status ensure_dir(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return Status(ErrorCode::RequestFailed,
                      "无法创建目录: " + path_to_utf8(dir) + " (" + ec.message() + ")");
    }
    return Status::success();
}

// 目录下所有 *.json 普通文件（跳过点开头的临时/隐藏项）
std::vector<std::filesystem::path> list_json_files(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return files;
    std::filesystem::directory_iterator it(dir, ec);
    const std::filesystem::directory_iterator end;
    while (!ec && it != end) {
        const std::filesystem::path entry_path = it->path();
        std::error_code fec;
        const bool regular = it->is_regular_file(fec);
        it.increment(ec);
        if (!regular) continue;
        const std::string name = path_to_utf8(entry_path.filename());
        if (name.size() <= 5 || name[0] == '.') continue;
        if (name.compare(name.size() - 5, 5, kJsonExt) != 0) continue;
        files.push_back(entry_path);
    }
    return files;
}

// 目录下所有子目录（跳过点开头的隐藏项）
std::vector<std::filesystem::path> list_subdirs(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> dirs;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return dirs;
    std::filesystem::directory_iterator it(dir, ec);
    const std::filesystem::directory_iterator end;
    while (!ec && it != end) {
        const std::filesystem::path entry_path = it->path();
        std::error_code fec;
        const bool directory = it->is_directory(fec);
        it.increment(ec);
        if (!directory) continue;
        const std::string name = path_to_utf8(entry_path.filename());
        if (name.empty() || name[0] == '.') continue;
        dirs.push_back(entry_path);
    }
    return dirs;
}

// 扫描 <prefix><数字>.json 取最大编号（损坏/无关文件不影响）
long long max_numeric_id(const std::filesystem::path& dir, std::string_view prefix) {
    long long best = 0;
    for (const auto& file : list_json_files(dir)) {
        const std::string stem = path_to_utf8(file.stem());
        if (stem.size() <= prefix.size()) continue;
        if (stem.compare(0, prefix.size(), prefix) != 0) continue;
        bool digits = true;
        long long value = 0;
        for (size_t i = prefix.size(); i < stem.size(); ++i) {
            const char c = stem[i];
            if (c < '0' || c > '9') {
                digits = false;
                break;
            }
            value = value * 10 + (c - '0');
            if (value > 999999999LL) {
                digits = false;
                break;
            }
        }
        if (!digits) continue;
        if (value > best) best = value;
    }
    return best;
}

std::string format_index_id(std::string_view prefix, long long index) {
    std::string digits = std::to_string(index < 1 ? 1 : index);
    while (digits.size() < 4) digits.insert(digits.begin(), '0');
    return std::string(prefix) + digits;
}

std::string next_playlist_id(const LibraryPaths& paths) {
    return format_index_id("pl_", max_numeric_id(paths.playlists(), "pl_") + 1);
}

std::string next_album_id(const LibraryPaths& paths) {
    return format_index_id("al_", max_numeric_id(paths.albums(), "al_") + 1);
}

Result<json::Value> read_json_object(const std::filesystem::path& file, std::string_view what) {
    auto text = read_text_file(file);
    if (!text.ok()) {
        return Result<json::Value>::failure(ErrorCode::InvalidMetadata,
                                            std::string(what) + " 读取失败: " + path_to_utf8(file));
    }
    json::ParseError err;
    const json::Value parsed = json::Value::parse(text.value(), &err);
    if (!parsed.is_object()) {
        return Result<json::Value>::failure(
            ErrorCode::InvalidMetadata, std::string(what) + " JSON 非法: " + path_to_utf8(file) + " (" +
                                            err.message + " @" + std::to_string(err.offset) + ")");
    }
    return Result<json::Value>::success(parsed);
}

Status write_json(const std::filesystem::path& dir, const std::filesystem::path& file,
                  const json::Value& value) {
    return write_text_file_atomic(dir, file, value.dump(2));
}

// 曲目引用规范化：补全折叠键、补全加入时间（0 → 现在）
PlaylistTrack normalize_track(const PlaylistTrack& track) {
    PlaylistTrack out = track;
    if (out.group_key.empty()) {
        out.group_key = radmusic::group_key(out.name, out.author, out.duration_seconds);
    }
    if (out.added_at <= 0) out.added_at = collections_now_seconds();
    return out;
}

bool tracks_equal(const std::vector<PlaylistTrack>& a, const std::vector<PlaylistTrack>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].to_json().dump() != b[i].to_json().dump()) return false;
    }
    return true;
}

// 同步覆盖时：已有曲目保留原 added_at（避免每次同步都刷新加入时间）
std::vector<PlaylistTrack> normalize_replacement(const std::vector<PlaylistTrack>& incoming,
                                                 const std::vector<PlaylistTrack>& current) {
    std::vector<PlaylistTrack> out;
    std::unordered_set<std::string> seen;   // 同 key 只保留首条（大歌单避免 O(n²)）
    std::string key;
    out.reserve(incoming.size());
    for (const auto& raw : incoming) {
        PlaylistTrack track = raw;
        key = track.key();
        if (!seen.insert(key).second) continue;
        if (track.added_at <= 0) {
            for (const auto& old : current) {
                if (old.key() == key && old.added_at > 0) {
                    track.added_at = old.added_at;
                    break;
                }
            }
        }
        out.push_back(normalize_track(track));
    }
    return out;
}

// ---------------------------------------------------------------- 歌单文件

Status save_playlist(const LibraryPaths& paths, const Playlist& playlist) {
    if (!is_safe_component(playlist.id)) {
        return Status(ErrorCode::InvalidMetadata, "歌单 id 非法: " + playlist.id);
    }
    return write_json(paths.playlists(), paths.playlist_file(playlist.id), playlist.to_json());
}

Result<Playlist> load_playlist(const std::filesystem::path& file) {
    auto object = read_json_object(file, "歌单");
    if (!object.ok()) return Result<Playlist>(object.status());
    Playlist playlist = Playlist::from_json(object.value());
    playlist.id = path_to_utf8(file.stem());   // 文件名即主键
    return Result<Playlist>::success(std::move(playlist));
}

Result<Playlist> read_playlist(const LibraryPaths& paths, std::string_view id) {
    if (!is_safe_component(id)) {
        return Result<Playlist>::failure(ErrorCode::PlaylistNotFound, "歌单不存在: " + std::string(id));
    }
    const auto file = paths.playlist_file(id);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) {
        return Result<Playlist>::failure(ErrorCode::PlaylistNotFound, "歌单不存在: " + std::string(id));
    }
    return load_playlist(file);
}

// ---------------------------------------------------------------- 专辑文件

Status save_album(const LibraryPaths& paths, const Album& album) {
    if (!is_safe_component(album.id)) {
        return Status(ErrorCode::InvalidMetadata, "专辑 id 非法: " + album.id);
    }
    return write_json(paths.albums(), paths.album_file(album.id), album.to_json());
}

Result<Album> load_album(const std::filesystem::path& file) {
    auto object = read_json_object(file, "专辑");
    if (!object.ok()) return Result<Album>(object.status());
    Album album = Album::from_json(object.value());
    album.id = path_to_utf8(file.stem());   // 文件名即主键
    return Result<Album>::success(std::move(album));
}

Result<Album> read_album(const LibraryPaths& paths, std::string_view id) {
    if (!is_safe_component(id)) {
        return Result<Album>::failure(ErrorCode::AlbumNotFound, "专辑不存在: " + std::string(id));
    }
    const auto file = paths.album_file(id);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) {
        return Result<Album>::failure(ErrorCode::AlbumNotFound, "专辑不存在: " + std::string(id));
    }
    return load_album(file);
}

// ---------------------------------------------------------------- MV 文件

// sha 作为目录名：正常 64 位 hex 经 fs_sanitize 后原样保留；
// 含分隔符等非法字符时被安全化，结构上避免路径穿越。
std::string mv_key(std::string_view sha) { return fs_sanitize(sha, 128); }

Result<MvInfo> load_mv(const std::filesystem::path& file, std::string_view fallback_sha) {
    auto object = read_json_object(file, "mv.json");
    if (!object.ok()) return Result<MvInfo>(object.status());
    MvInfo mv = MvInfo::from_json(object.value());
    if (mv.sha.empty()) mv.sha = std::string(fallback_sha);   // 目录名即主键
    if (mv.version.empty()) mv.version = "1.0.0";
    return Result<MvInfo>::success(std::move(mv));
}

std::vector<MvInfo> collect_mvs(const LibraryPaths& paths) {
    std::vector<MvInfo> out;
    for (const auto& dir : list_subdirs(paths.mv())) {
        const std::filesystem::path file = dir / "mv.json";
        std::error_code ec;
        if (!std::filesystem::is_regular_file(file, ec)) continue;
        auto mv = load_mv(file, path_to_utf8(dir.filename()));
        if (!mv.ok()) continue;   // 损坏条目跳过（list 不整体失败）
        out.push_back(std::move(mv.value()));
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------- helpers

long long collections_now_seconds() {
    using namespace std::chrono;
    return static_cast<long long>(duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
}

// ---------------------------------------------------------------- PlaylistTrack

std::string PlaylistTrack::key() const { return join_key(platform, item_id); }

json::Value PlaylistTrack::to_json() const {
    json::Value v = json::Value::object();
    v.set("platform", platform);
    v.set("item_id", item_id);
    v.set("sha", sha);
    v.set("name", name);
    v.set("author", author);
    v.set("duration", static_cast<long long>(duration_seconds));
    v.set("group_key", group_key);
    v.set("added_at", added_at);
    return v;
}

PlaylistTrack PlaylistTrack::from_json(const json::Value& v) {
    PlaylistTrack t;
    if (!v.is_object()) return t;
    t.platform = v.get_string("platform");
    t.item_id = v.get_string("item_id", v.get_string("id"));
    t.sha = v.get_string("sha");
    t.name = v.get_string("name");
    t.author = v.get_string("author");
    t.duration_seconds = static_cast<int>(v.get_int("duration", 0));
    t.group_key = v.get_string("group_key");
    t.added_at = static_cast<long long>(v.get_int("added_at", 0));
    return t;
}

PlaylistTrack PlaylistTrack::from_base(const BaseInfo& base) {
    PlaylistTrack t;
    t.platform = base.platform;
    t.sha = base.sha;
    t.item_id = base.item_id.empty() ? base.sha : base.item_id;   // local 用 sha
    t.name = base.name;
    t.author = base.author;
    t.duration_seconds = base.duration_seconds;
    t.group_key = base.group_key();
    return t;
}

// ---------------------------------------------------------------- Playlist

long long Playlist::total_duration_seconds() const {
    long long total = 0;
    for (const auto& track : tracks) {
        if (track.duration_seconds > 0) total += track.duration_seconds;
    }
    return total;
}

json::Value Playlist::to_json() const {
    json::Value v = json::Value::object();
    v.set("id", id);
    v.set("name", name);
    v.set("description", description);
    v.set("pic", pic);
    v.set("owner", owner);
    v.set("platform", platform);
    v.set("source_id", source_id);
    v.set("created_at", created_at);
    v.set("updated_at", updated_at);
    // 2.14：列表页需要的派生字段（只增不改，保持向后兼容）
    v.set("track_count", static_cast<long long>(tracks.size()));
    v.set("total_duration", total_duration_seconds());
    json::Value list = json::Value::array();
    for (const auto& track : tracks) list.push_back(track.to_json());
    v.set("tracks", list);
    return v;
}

Playlist Playlist::from_json(const json::Value& v) {
    Playlist p;
    if (!v.is_object()) return p;
    p.id = v.get_string("id");
    p.name = v.get_string("name");
    p.description = v.get_string("description");
    p.pic = v.get_string("pic");
    p.owner = v.get_string("owner", "user");
    p.platform = v.get_string("platform", "local");
    p.source_id = v.get_string("source_id");
    p.created_at = static_cast<long long>(v.get_int("created_at", 0));
    p.updated_at = static_cast<long long>(v.get_int("updated_at", 0));
    if (const json::Value* list = v.find("tracks")) {
        if (list->is_array()) {
            for (const auto& entry : list->as_array()) {
                if (entry.is_object()) p.tracks.push_back(PlaylistTrack::from_json(entry));
            }
        }
    }
    return p;
}

// ---------------------------------------------------------------- Album

long long Album::total_duration_seconds() const {
    long long total = 0;
    for (const auto& track : tracks) {
        if (track.duration_seconds > 0) total += track.duration_seconds;
    }
    return total;
}

json::Value Album::to_json() const {
    json::Value v = json::Value::object();
    v.set("id", id);
    v.set("name", name);
    v.set("artist", artist);
    v.set("pic", pic);
    v.set("description", description);
    v.set("platform", platform);
    v.set("source_id", source_id);
    v.set("year", static_cast<long long>(year));
    v.set("created_at", created_at);
    v.set("updated_at", updated_at);
    // 2.15：列表页需要的派生字段（只增不改）
    v.set("track_count", static_cast<long long>(tracks.size()));
    v.set("total_duration", total_duration_seconds());
    json::Value list = json::Value::array();
    for (const auto& track : tracks) list.push_back(track.to_json());
    v.set("tracks", list);
    return v;
}

Album Album::from_json(const json::Value& v) {
    Album a;
    if (!v.is_object()) return a;
    a.id = v.get_string("id");
    a.name = v.get_string("name");
    a.artist = v.get_string("artist");
    a.pic = v.get_string("pic");
    a.description = v.get_string("description");
    a.platform = v.get_string("platform", "local");
    a.source_id = v.get_string("source_id");
    a.year = static_cast<int>(v.get_int("year", 0));
    a.created_at = static_cast<long long>(v.get_int("created_at", 0));
    a.updated_at = static_cast<long long>(v.get_int("updated_at", 0));
    if (const json::Value* list = v.find("tracks")) {
        if (list->is_array()) {
            for (const auto& entry : list->as_array()) {
                if (entry.is_object()) a.tracks.push_back(PlaylistTrack::from_json(entry));
            }
        }
    }
    return a;
}

// ---------------------------------------------------------------- MvInfo

json::Value MvInfo::to_json() const {
    json::Value v = json::Value::object();
    v.set("sha", sha);
    v.set("platform", platform);
    v.set("item_id", item_id);
    v.set("version", version);
    v.set("title", title);
    v.set("artist", artist);
    v.set("album", album);
    v.set("location", location);
    v.set("pic", pic);
    v.set("duration", static_cast<long long>(duration_seconds));
    v.set("width", static_cast<long long>(width));
    v.set("height", static_cast<long long>(height));
    v.set("created_at", created_at);
    return v;
}

MvInfo MvInfo::from_json(const json::Value& v) {
    MvInfo m;
    if (!v.is_object()) return m;
    m.sha = v.get_string("sha");
    m.platform = v.get_string("platform", "local");
    m.item_id = v.get_string("item_id", v.get_string("id"));
    m.version = v.get_string("version", "1.0.0");
    m.title = v.get_string("title");
    m.artist = v.get_string("artist");
    m.album = v.get_string("album");
    m.location = v.get_string("location");
    m.pic = v.get_string("pic");
    m.duration_seconds = static_cast<int>(v.get_int("duration", 0));
    m.width = static_cast<int>(v.get_int("width", 0));
    m.height = static_cast<int>(v.get_int("height", 0));
    m.created_at = static_cast<long long>(v.get_int("created_at", 0));
    return m;
}

// ---------------------------------------------------------------- PlaylistStore

PlaylistStore::PlaylistStore(const LibraryPaths& paths) : paths_(paths) {}

Status PlaylistStore::ensure_layout() { return ensure_dir(paths_.playlists()); }

Result<std::vector<Playlist>> PlaylistStore::list() const {
    std::vector<Playlist> out;
    for (const auto& file : list_json_files(paths_.playlists())) {
        auto playlist = load_playlist(file);
        if (!playlist.ok()) continue;   // 损坏条目跳过，不整体失败
        out.push_back(std::move(playlist.value()));
    }
    std::sort(out.begin(), out.end(), [](const Playlist& a, const Playlist& b) {
        if (a.updated_at != b.updated_at) return a.updated_at > b.updated_at;
        return a.id < b.id;
    });
    return Result<std::vector<Playlist>>::success(std::move(out));
}

Result<Playlist> PlaylistStore::get(std::string_view id) const { return read_playlist(paths_, id); }

Result<Playlist> PlaylistStore::create(std::string_view name, std::string_view description,
                                       std::string_view platform, std::string_view source_id,
                                       std::string_view pic) {
    Status layout = ensure_layout();
    if (!layout.ok()) return Result<Playlist>(layout);

    Playlist playlist;
    playlist.name = std::string(name);
    playlist.description = std::string(description);
    playlist.platform = platform.empty() ? std::string("local") : std::string(platform);
    playlist.source_id = std::string(source_id);
    playlist.pic = std::string(pic);
    playlist.owner = playlist.platform == "local" ? std::string("user") : std::string("platform");

    const bool from_platform = !playlist.source_id.empty() && playlist.platform != "local";
    if (from_platform) {
        // 幂等同步：<platform>_<安全化 source_id>
        playlist.id = fs_sanitize(playlist.platform, 24) + "_" + fs_sanitize(playlist.source_id, 48);
        auto existing = read_playlist(paths_, playlist.id);
        if (existing.ok()) {
            Playlist current = existing.value();
            bool changed = false;
            if (!name.empty() && current.name != playlist.name) {
                current.name = playlist.name;
                changed = true;
            }
            if (!description.empty() && current.description != playlist.description) {
                current.description = playlist.description;
                changed = true;
            }
            if (!pic.empty() && current.pic != playlist.pic) {
                current.pic = playlist.pic;
                changed = true;
            }
            if (changed) {
                current.updated_at = collections_now_seconds();
                Status w = save_playlist(paths_, current);
                if (!w.ok()) return Result<Playlist>(w);
            }
            return Result<Playlist>::success(std::move(current));
        }
        // 已存在但损坏（1007）：落到下面重建
    } else {
        playlist.id = next_playlist_id(paths_);
    }

    const long long now = collections_now_seconds();
    playlist.created_at = now;
    playlist.updated_at = now;
    Status w = save_playlist(paths_, playlist);
    if (!w.ok()) return Result<Playlist>(w);
    return Result<Playlist>::success(std::move(playlist));
}

Result<Playlist> PlaylistStore::rename(std::string_view id, std::string_view name,
                                       std::string_view description) {
    auto loaded = read_playlist(paths_, id);
    if (!loaded.ok()) return loaded;
    Playlist playlist = loaded.value();
    playlist.name = std::string(name);
    playlist.description = std::string(description);
    playlist.updated_at = collections_now_seconds();
    Status w = save_playlist(paths_, playlist);
    if (!w.ok()) return Result<Playlist>(w);
    return Result<Playlist>::success(std::move(playlist));
}

bool PlaylistStore::remove(std::string_view id) {
    if (!is_safe_component(id)) return false;
    const auto file = paths_.playlist_file(id);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) return false;
    std::filesystem::remove(file, ec);
    return !ec;
}

Result<Playlist> PlaylistStore::add_track(std::string_view id, const PlaylistTrack& track) {
    auto loaded = read_playlist(paths_, id);
    if (!loaded.ok()) return loaded;
    Playlist playlist = loaded.value();

    const PlaylistTrack normalized = normalize_track(track);
    const std::string key = normalized.key();
    for (const auto& existing : playlist.tracks) {
        if (existing.key() == key) {
            return Result<Playlist>::success(std::move(playlist));   // 不重复添加、不报错
        }
    }
    playlist.tracks.push_back(normalized);
    playlist.updated_at = collections_now_seconds();
    Status w = save_playlist(paths_, playlist);
    if (!w.ok()) return Result<Playlist>(w);
    return Result<Playlist>::success(std::move(playlist));
}

Result<Playlist> PlaylistStore::add_tracks(std::string_view id,
                                           const std::vector<PlaylistTrack>& tracks) {
    auto loaded = read_playlist(paths_, id);
    if (!loaded.ok()) return loaded;
    Playlist playlist = loaded.value();

    bool changed = false;
    for (const PlaylistTrack& track : tracks) {
        const PlaylistTrack normalized = normalize_track(track);
        const std::string key = normalized.key();
        bool exists = false;
        for (const auto& existing : playlist.tracks) {
            if (existing.key() == key) {
                exists = true;   // 重复的跳过，不报错
                break;
            }
        }
        if (exists) continue;
        playlist.tracks.push_back(normalized);
        changed = true;
    }
    if (!changed) return Result<Playlist>::success(std::move(playlist));

    playlist.updated_at = collections_now_seconds();
    Status w = save_playlist(paths_, playlist);
    if (!w.ok()) return Result<Playlist>(w);
    return Result<Playlist>::success(std::move(playlist));
}

Result<Playlist> PlaylistStore::remove_track(std::string_view id, std::string_view platform,
                                             std::string_view item_id) {
    auto loaded = read_playlist(paths_, id);
    if (!loaded.ok()) return loaded;
    Playlist playlist = loaded.value();

    const std::string key = join_key(platform, item_id);
    auto it = std::find_if(playlist.tracks.begin(), playlist.tracks.end(),
                           [&key](const PlaylistTrack& track) { return track.key() == key; });
    if (it == playlist.tracks.end()) {
        return Result<Playlist>::success(std::move(playlist));   // 不存在 → 无操作
    }
    playlist.tracks.erase(it);
    playlist.updated_at = collections_now_seconds();
    Status w = save_playlist(paths_, playlist);
    if (!w.ok()) return Result<Playlist>(w);
    return Result<Playlist>::success(std::move(playlist));
}

Result<Playlist> PlaylistStore::move_track(std::string_view id, std::string_view platform,
                                           std::string_view item_id, int new_index) {
    auto loaded = read_playlist(paths_, id);
    if (!loaded.ok()) return loaded;
    Playlist playlist = loaded.value();

    const std::string key = join_key(platform, item_id);
    const auto it = std::find_if(playlist.tracks.begin(), playlist.tracks.end(),
                                [&key](const PlaylistTrack& track) { return track.key() == key; });
    if (it == playlist.tracks.end()) {
        return Result<Playlist>::success(std::move(playlist));   // 不存在 → 无操作
    }
    const int from = static_cast<int>(std::distance(playlist.tracks.begin(), it));
    int to = new_index;
    const int last = static_cast<int>(playlist.tracks.size()) - 1;
    if (to < 0) to = 0;            // 越界钳制
    if (to > last) to = last;
    if (to == from) return Result<Playlist>::success(std::move(playlist));

    PlaylistTrack moved = playlist.tracks[static_cast<size_t>(from)];
    playlist.tracks.erase(playlist.tracks.begin() + from);
    playlist.tracks.insert(playlist.tracks.begin() + to, std::move(moved));
    playlist.updated_at = collections_now_seconds();
    Status w = save_playlist(paths_, playlist);
    if (!w.ok()) return Result<Playlist>(w);
    return Result<Playlist>::success(std::move(playlist));
}

Result<Playlist> PlaylistStore::replace_tracks(std::string_view id,
                                               const std::vector<PlaylistTrack>& tracks) {
    auto loaded = read_playlist(paths_, id);
    if (!loaded.ok()) return loaded;
    Playlist playlist = loaded.value();

    std::vector<PlaylistTrack> next = normalize_replacement(tracks, playlist.tracks);
    if (tracks_equal(next, playlist.tracks)) {
        return Result<Playlist>::success(std::move(playlist));   // 内容一致 → 无写入
    }
    playlist.tracks = std::move(next);
    playlist.updated_at = collections_now_seconds();
    Status w = save_playlist(paths_, playlist);
    if (!w.ok()) return Result<Playlist>(w);
    return Result<Playlist>::success(std::move(playlist));
}

// ---------------------------------------------------------------- AlbumStore

AlbumStore::AlbumStore(const LibraryPaths& paths) : paths_(paths) {}

Status AlbumStore::ensure_layout() { return ensure_dir(paths_.albums()); }

Result<std::vector<Album>> AlbumStore::list() const {
    std::vector<Album> out;
    for (const auto& file : list_json_files(paths_.albums())) {
        auto album = load_album(file);
        if (!album.ok()) continue;   // 损坏条目跳过
        out.push_back(std::move(album.value()));
    }
    std::sort(out.begin(), out.end(), [](const Album& a, const Album& b) {
        if (a.updated_at != b.updated_at) return a.updated_at > b.updated_at;
        return a.id < b.id;
    });
    return Result<std::vector<Album>>::success(std::move(out));
}

Result<Album> AlbumStore::get(std::string_view id) const { return read_album(paths_, id); }

Result<Album> AlbumStore::create(std::string_view name, std::string_view artist,
                                 std::string_view platform, std::string_view source_id,
                                 std::string_view pic, int year) {
    Status layout = ensure_layout();
    if (!layout.ok()) return Result<Album>(layout);

    Album album;
    album.name = std::string(name);
    album.artist = std::string(artist);
    album.pic = std::string(pic);
    album.platform = platform.empty() ? std::string("local") : std::string(platform);
    album.source_id = std::string(source_id);
    album.year = year > 0 ? year : 0;

    const bool from_platform = !album.source_id.empty() && album.platform != "local";
    if (from_platform) {
        album.id = fs_sanitize(album.platform, 24) + "_" + fs_sanitize(album.source_id, 48);
        auto existing = read_album(paths_, album.id);
        if (existing.ok()) {
            Album current = existing.value();
            bool changed = false;
            if (!name.empty() && current.name != album.name) {
                current.name = album.name;
                changed = true;
            }
            if (!artist.empty() && current.artist != album.artist) {
                current.artist = album.artist;
                changed = true;
            }
            if (!pic.empty() && current.pic != album.pic) {
                current.pic = album.pic;
                changed = true;
            }
            if (album.year > 0 && current.year != album.year) {
                current.year = album.year;
                changed = true;
            }
            if (changed) {
                current.updated_at = collections_now_seconds();
                Status w = save_album(paths_, current);
                if (!w.ok()) return Result<Album>(w);
            }
            return Result<Album>::success(std::move(current));
        }
    } else {
        album.id = next_album_id(paths_);
    }

    const long long now = collections_now_seconds();
    album.created_at = now;
    album.updated_at = now;
    Status w = save_album(paths_, album);
    if (!w.ok()) return Result<Album>(w);
    return Result<Album>::success(std::move(album));
}

Result<Album> AlbumStore::update(std::string_view id, std::string_view name, std::string_view artist,
                                 std::string_view pic, std::string_view description, int year) {
    auto loaded = read_album(paths_, id);
    if (!loaded.ok()) return loaded;
    Album album = loaded.value();

    bool changed = false;
    if (!name.empty() && album.name != name) {
        album.name = std::string(name);
        changed = true;
    }
    if (!artist.empty() && album.artist != artist) {
        album.artist = std::string(artist);
        changed = true;
    }
    if (!pic.empty() && album.pic != pic) {
        album.pic = std::string(pic);
        changed = true;
    }
    if (!description.empty() && album.description != description) {
        album.description = std::string(description);
        changed = true;
    }
    if (year > 0 && album.year != year) {
        album.year = year;
        changed = true;
    }
    if (!changed) return Result<Album>::success(std::move(album));

    album.updated_at = collections_now_seconds();
    Status w = save_album(paths_, album);
    if (!w.ok()) return Result<Album>(w);
    return Result<Album>::success(std::move(album));
}

Result<Album> AlbumStore::add_tracks(std::string_view id,
                                     const std::vector<PlaylistTrack>& tracks) {
    auto loaded = read_album(paths_, id);
    if (!loaded.ok()) return loaded;
    Album album = loaded.value();

    bool changed = false;
    for (const PlaylistTrack& track : tracks) {
        const PlaylistTrack normalized = normalize_track(track);
        const std::string key = normalized.key();
        bool exists = false;
        for (const auto& existing : album.tracks) {
            if (existing.key() == key) {
                exists = true;
                break;
            }
        }
        if (exists) continue;
        album.tracks.push_back(normalized);
        changed = true;
    }
    if (!changed) return Result<Album>::success(std::move(album));

    album.updated_at = collections_now_seconds();
    Status w = save_album(paths_, album);
    if (!w.ok()) return Result<Album>(w);
    return Result<Album>::success(std::move(album));
}

bool AlbumStore::remove(std::string_view id) {
    if (!is_safe_component(id)) return false;
    const auto file = paths_.album_file(id);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) return false;
    std::filesystem::remove(file, ec);
    return !ec;
}

Result<Album> AlbumStore::add_track(std::string_view id, const PlaylistTrack& track) {
    auto loaded = read_album(paths_, id);
    if (!loaded.ok()) return loaded;
    Album album = loaded.value();

    const PlaylistTrack normalized = normalize_track(track);
    const std::string key = normalized.key();
    for (const auto& existing : album.tracks) {
        if (existing.key() == key) {
            return Result<Album>::success(std::move(album));   // 不重复添加
        }
    }
    album.tracks.push_back(normalized);
    album.updated_at = collections_now_seconds();
    Status w = save_album(paths_, album);
    if (!w.ok()) return Result<Album>(w);
    return Result<Album>::success(std::move(album));
}

Result<Album> AlbumStore::remove_track(std::string_view id, std::string_view platform,
                                       std::string_view item_id) {
    auto loaded = read_album(paths_, id);
    if (!loaded.ok()) return loaded;
    Album album = loaded.value();

    const std::string key = join_key(platform, item_id);
    auto it = std::find_if(album.tracks.begin(), album.tracks.end(),
                           [&key](const PlaylistTrack& track) { return track.key() == key; });
    if (it == album.tracks.end()) {
        return Result<Album>::success(std::move(album));
    }
    album.tracks.erase(it);
    album.updated_at = collections_now_seconds();
    Status w = save_album(paths_, album);
    if (!w.ok()) return Result<Album>(w);
    return Result<Album>::success(std::move(album));
}

Result<Album> AlbumStore::replace_tracks(std::string_view id,
                                         const std::vector<PlaylistTrack>& tracks) {
    auto loaded = read_album(paths_, id);
    if (!loaded.ok()) return loaded;
    Album album = loaded.value();

    std::vector<PlaylistTrack> next = normalize_replacement(tracks, album.tracks);
    if (tracks_equal(next, album.tracks)) {
        return Result<Album>::success(std::move(album));
    }
    album.tracks = std::move(next);
    album.updated_at = collections_now_seconds();
    Status w = save_album(paths_, album);
    if (!w.ok()) return Result<Album>(w);
    return Result<Album>::success(std::move(album));
}

// ---------------------------------------------------------------- MvStore

MvStore::MvStore(const LibraryPaths& paths) : paths_(paths) {}

Status MvStore::ensure_layout() { return ensure_dir(paths_.mv()); }

Result<std::vector<MvInfo>> MvStore::list() const {
    std::vector<MvInfo> out = collect_mvs(paths_);
    std::sort(out.begin(), out.end(), [](const MvInfo& a, const MvInfo& b) {
        if (a.created_at != b.created_at) return a.created_at > b.created_at;
        return a.sha < b.sha;
    });
    return Result<std::vector<MvInfo>>::success(std::move(out));
}

Result<MvInfo> MvStore::get(std::string_view sha) const {
    if (sha.empty()) {
        return Result<MvInfo>::failure(ErrorCode::MvNotFound, "MV 不存在: sha 为空");
    }
    const std::string key = mv_key(sha);
    const auto file = paths_.mv_file(key);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) {
        return Result<MvInfo>::failure(ErrorCode::MvNotFound, "MV 不存在: " + std::string(sha));
    }
    return load_mv(file, key);   // 命中损坏文件 → 1007
}

Result<MvInfo> MvStore::find_by_source(std::string_view platform, std::string_view item_id) const {
    if (platform.empty() || item_id.empty()) {
        return Result<MvInfo>::failure(ErrorCode::MvNotFound, "MV 不存在: platform/item_id 为空");
    }
    auto all = list();
    if (!all.ok()) return Result<MvInfo>(all.status());
    for (const auto& mv : all.value()) {
        if (mv.platform == platform && mv.item_id == item_id) {
            return Result<MvInfo>::success(mv);
        }
    }
    return Result<MvInfo>::failure(ErrorCode::MvNotFound,
                                    "MV 不存在: " + std::string(platform) + ":" + std::string(item_id));
}

Result<MvInfo> MvStore::put(const MvInfo& info) {
    if (info.sha.empty()) {
        return Result<MvInfo>::failure(ErrorCode::InvalidMetadata, "MV 缺少 sha");
    }
    Status layout = ensure_layout();
    if (!layout.ok()) return Result<MvInfo>(layout);

    const std::string key = mv_key(info.sha);
    const auto file = paths_.mv_file(key);

    std::optional<MvInfo> existing;
    std::error_code ec;
    if (std::filesystem::is_regular_file(file, ec)) {
        auto loaded = load_mv(file, key);
        if (loaded.ok()) existing = loaded.value();
        // 损坏的 mv.json 视为可重建（与 MetaCache::put 一致）
    }

    // 防覆写：(platform, item_id) 只能指向一个 sha
    if (!info.platform.empty() && !info.item_id.empty()) {
        for (const auto& other : collect_mvs(paths_)) {
            if (mv_key(other.sha) == key) continue;
            if (other.platform == info.platform && other.item_id == info.item_id) {
                return Result<MvInfo>::failure(
                    ErrorCode::BaseConflict, "同一 (platform, item_id) 已被其它 MV 占用: " +
                                                 other.platform + ":" + other.item_id + " -> sha=" + other.sha);
            }
        }
    }

    MvInfo out = info;
    if (out.version.empty()) out.version = "1.0.0";
    if (existing.has_value()) {
        // 同 sha 才允许覆盖；created_at 保留首次写入值
        out.created_at = existing->created_at > 0 ? existing->created_at : collections_now_seconds();
        if (existing->to_json().dump() == out.to_json().dump()) {
            return Result<MvInfo>::success(std::move(existing.value()));   // 内容一致 → 原样返回
        }
    } else {
        out.created_at = info.created_at > 0 ? info.created_at : collections_now_seconds();
    }

    Status w = write_json(paths_.mv_dir(key), file, out.to_json());
    if (!w.ok()) return Result<MvInfo>(w);
    return Result<MvInfo>::success(std::move(out));
}

bool MvStore::remove(std::string_view sha) {
    if (sha.empty()) return false;
    const auto dir = paths_.mv_dir(mv_key(sha));
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return false;
    std::filesystem::remove_all(dir, ec);
    return !ec;
}

}  // namespace radmusic
