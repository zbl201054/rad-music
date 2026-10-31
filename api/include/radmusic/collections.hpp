#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "radmusic/error.hpp"
#include "radmusic/json.hpp"
#include "radmusic/storage.hpp"

namespace radmusic {

// 歌单/专辑里的一条曲目引用（不复制音频，只存标识与展示所需的冗余字段）
struct PlaylistTrack {
    std::string platform;              // local / netease / radish ...
    std::string item_id;               // 平台曲目标识；local 用 sha
    std::string sha;                   // 本地/已下载曲目的 sha（可空）
    std::string name;
    std::string author;
    std::string pic;                   // 封面（在线 URL / data: / 相对路径 covers/<hash>.<ext>）；可为空
    int duration_seconds = 0;
    std::string group_key;             // 折叠键（2.7）；为空时按 name/author/duration 重算
    long long added_at = 0;            // Unix 秒

    std::string key() const;           // platform + ":" + item_id
    json::Value to_json() const;
    static PlaylistTrack from_json(const json::Value& v);
    static PlaylistTrack from_base(const struct BaseInfo& base);   // 由 base.json 生成
};

// 歌单（library/playlists/<id>.json）
struct Playlist {
    std::string id;
    std::string name;
    std::string description;
    std::string pic;
    std::string owner = "user";        // user / platform
    std::string platform = "local";    // local = 用户歌单；其他 = 在线歌单来源
    std::string source_id;             // 在线歌单在该平台的 id
    long long created_at = 0;
    long long updated_at = 0;
    std::vector<PlaylistTrack> tracks;

    int track_count() const { return static_cast<int>(tracks.size()); }
    long long total_duration_seconds() const;
    json::Value to_json() const;
    static Playlist from_json(const json::Value& v);
};

// 专辑（library/albums/<id>.json）
struct Album {
    std::string id;
    std::string name;
    std::string artist;
    std::string pic;
    std::string description;
    std::string platform = "local";
    std::string source_id;
    int year = 0;
    long long created_at = 0;
    long long updated_at = 0;
    std::vector<PlaylistTrack> tracks;

    int track_count() const { return static_cast<int>(tracks.size()); }
    long long total_duration_seconds() const;
    json::Value to_json() const;
    static Album from_json(const json::Value& v);
};

// MV（mv/<sha>/mv.json；location 可为在线 URL 或本地文件）
struct MvInfo {
    std::string sha;
    std::string platform = "local";
    std::string item_id;
    std::string version = "1.0.0";
    std::string title;
    std::string artist;
    std::string album;
    std::string location;              // URL 或相对曲库根的路径
    std::string pic;
    int duration_seconds = 0;
    int width = 0;
    int height = 0;
    long long created_at = 0;

    json::Value to_json() const;
    static MvInfo from_json(const json::Value& v);
};

class PlaylistStore {
public:
    explicit PlaylistStore(const LibraryPaths& paths);
    Status ensure_layout();

    Result<std::vector<Playlist>> list() const;                  // 按 updated_at 降序
    Result<Playlist> get(std::string_view id) const;             // 不存在 → 1030
    Result<Playlist> create(std::string_view name, std::string_view description = {},
                            std::string_view platform = "local", std::string_view source_id = {},
                            std::string_view pic = {});
    Result<Playlist> rename(std::string_view id, std::string_view name, std::string_view description);
    bool remove(std::string_view id);                            // 不存在返回 false
    Result<Playlist> add_track(std::string_view id, const PlaylistTrack& track);      // 同 key 已存在 → 不重复添加（返回现有歌单）
    Result<Playlist> add_tracks(std::string_view id, const std::vector<PlaylistTrack>& tracks);  // 批量添加，一次落盘
    Result<Playlist> remove_track(std::string_view id, std::string_view platform, std::string_view item_id);
    Result<Playlist> move_track(std::string_view id, std::string_view platform,
                                std::string_view item_id, int new_index);            // 越界钳制
    // 在线歌单同步：用插件给的完整曲目表覆盖（保留 id/created_at）
    Result<Playlist> replace_tracks(std::string_view id, const std::vector<PlaylistTrack>& tracks);

private:
    const LibraryPaths& paths_;
};

class AlbumStore {
public:
    explicit AlbumStore(const LibraryPaths& paths);
    Status ensure_layout();

    Result<std::vector<Album>> list() const;                     // 按 updated_at 降序
    Result<Album> get(std::string_view id) const;                // 不存在 → 1031
    Result<Album> create(std::string_view name, std::string_view artist,
                         std::string_view platform = "local", std::string_view source_id = {},
                         std::string_view pic = {}, int year = 0);
    // 编辑专辑元数据；空字符串表示“不修改该字段”，year<=0 表示不修改
    Result<Album> update(std::string_view id, std::string_view name, std::string_view artist,
                         std::string_view pic, std::string_view description, int year);
    bool remove(std::string_view id);
    Result<Album> add_track(std::string_view id, const PlaylistTrack& track);
    Result<Album> add_tracks(std::string_view id, const std::vector<PlaylistTrack>& tracks);   // 批量添加，一次落盘
    Result<Album> remove_track(std::string_view id, std::string_view platform, std::string_view item_id);
    Result<Album> replace_tracks(std::string_view id, const std::vector<PlaylistTrack>& tracks);

private:
    const LibraryPaths& paths_;
};

class MvStore {
public:
    explicit MvStore(const LibraryPaths& paths);
    Status ensure_layout();

    Result<std::vector<MvInfo>> list() const;                    // 按 created_at 降序
    Result<MvInfo> get(std::string_view sha) const;              // 不存在 → 1032
    Result<MvInfo> find_by_source(std::string_view platform, std::string_view item_id) const;
    // 防覆写（与 base.json 同思路）：同 sha 且内容一致 → 原样返回；同 sha 内容不同 → 覆盖（仅允许同 sha）；
    // 新 sha 且 (platform,item_id) 已被别的 sha 占用 → BaseConflict(1023)
    // sha 为空 → InvalidMetadata(1007)；写入必须原子（先 .tmp 再替换）
    Result<MvInfo> put(const MvInfo& info);
    bool remove(std::string_view sha);                           // 删除 mv/<sha>/ 整个目录

private:
    const LibraryPaths& paths_;
};

// 时间辅助（Unix 秒）；实现里用 std::chrono
long long collections_now_seconds();

}  // namespace radmusic
