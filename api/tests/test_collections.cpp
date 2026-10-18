// 歌单 / 专辑 / MV 持久化测试（Readme 1.11–1.13 / 2.14–2.16）
//
// 每个用例使用独立的临时曲库根 build/tmp_collections/<case>/，
// 进入时 remove_all 清空、结束时再清理一次。
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "radmusic/collections.hpp"
#include "radmusic/model.hpp"
#include "test_framework.hpp"

using namespace radmusic;

namespace {

std::filesystem::path fresh_root(const std::string& name) {
    const auto root = std::filesystem::path("build") / "tmp_collections" / name;
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    return root;
}

void drop_root(const std::filesystem::path& root) {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

// 造一个合法的 64 位小写 sha（由 seed 生成，便于区分不同内容）
std::string fake_sha(char seed) {
    std::string s(64, '0');
    for (size_t i = 0; i < 64; ++i) s[i] = static_cast<char>('a' + ((seed + static_cast<char>(i)) % 6));
    return s;
}

PlaylistTrack mk_track(const std::string& platform, const std::string& item_id, const std::string& name,
                       const std::string& author, int duration) {
    PlaylistTrack t;
    t.platform = platform;
    t.item_id = item_id;
    t.name = name;
    t.author = author;
    t.duration_seconds = duration;
    return t;
}

// 直接改写落盘 JSON 的某个整数字段（用于确定性地构造时间戳/排序场景）
bool patch_json_field(const std::filesystem::path& file, const std::string& key, long long value) {
    auto text = read_text_file(file);
    if (!text.ok()) return false;
    json::Value parsed = json::Value::parse(text.value());
    if (!parsed.is_object()) return false;
    parsed.set(key, value);
    return write_text_file_atomic(file.parent_path(), file, parsed.dump(2)).ok();
}

// 直接写入原始文本（用于制造损坏 JSON）
bool write_raw(const std::filesystem::path& file, const std::string& content) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << content;
    out.close();
    return true;
}

}  // namespace

// ---------------------------------------------------------------- 歌单

TEST_CASE(test_collections_playlist_create_get_list) {
    const auto root = fresh_root("playlist_create");
    LibraryPaths paths(root);
    PlaylistStore store(paths);

    CHECK(store.ensure_layout().ok());
    CHECK(std::filesystem::is_directory(paths.playlists()));
    CHECK(std::filesystem::is_directory(paths.library()));

    auto first = store.create("morning", "first mix");
    CHECK(first.ok());
    if (first.ok()) {
        CHECK_EQ(first.value().id, std::string("pl_0001"));
        CHECK(first.value().created_at > 0);
        CHECK(first.value().updated_at > 0);
        CHECK_EQ(first.value().created_at, first.value().updated_at);
        CHECK_EQ(first.value().platform, std::string("local"));
        CHECK_EQ(first.value().owner, std::string("user"));
        CHECK_EQ(first.value().track_count(), 0);
        CHECK_EQ(first.value().total_duration_seconds(), 0LL);
        CHECK(std::filesystem::is_regular_file(paths.playlist_file("pl_0001")));
    }

    auto second = store.create("night", "second mix");
    CHECK(second.ok());
    if (second.ok()) CHECK_EQ(second.value().id, std::string("pl_0002"));

    // id 递增：删掉最大的后仍不复用编号（扫现存文件取最大编号 +1）
    CHECK(store.remove("pl_0002"));
    auto third = store.create("late night");
    CHECK(third.ok());
    if (third.ok()) CHECK_EQ(third.value().id, std::string("pl_0002"));

    // get 回读
    auto loaded = store.get("pl_0001");
    CHECK(loaded.ok());
    if (loaded.ok()) {
        CHECK_EQ(loaded.value().id, std::string("pl_0001"));
        CHECK_EQ(loaded.value().name, std::string("morning"));
        CHECK_EQ(loaded.value().description, std::string("first mix"));
    }

    // 排序：updated_at 降序；同值按 id 升序
    CHECK(patch_json_field(paths.playlist_file("pl_0001"), "updated_at", 100));
    CHECK(patch_json_field(paths.playlist_file("pl_0002"), "updated_at", 200));
    auto descending = store.list();
    CHECK(descending.ok());
    if (descending.ok()) {
        CHECK_EQ(descending.value().size(), size_t{2});
        CHECK_EQ(descending.value().at(0).id, std::string("pl_0002"));
        CHECK_EQ(descending.value().at(1).id, std::string("pl_0001"));
    }
    CHECK(patch_json_field(paths.playlist_file("pl_0001"), "updated_at", 300));
    CHECK(patch_json_field(paths.playlist_file("pl_0002"), "updated_at", 300));
    auto tied = store.list();
    CHECK(tied.ok());
    if (tied.ok()) {
        CHECK_EQ(tied.value().size(), size_t{2});
        CHECK_EQ(tied.value().at(0).id, std::string("pl_0001"));
        CHECK_EQ(tied.value().at(1).id, std::string("pl_0002"));
    }

    drop_root(root);
}

TEST_CASE(test_collections_playlist_track_add_dedup_remove_move) {
    const auto root = fresh_root("playlist_tracks");
    LibraryPaths paths(root);
    PlaylistStore store(paths);

    auto created = store.create("demo");
    CHECK(created.ok());
    if (!created.ok()) {
        drop_root(root);
        return;
    }
    const std::string id = created.value().id;
    const std::string local_id = fake_sha(1);

    // add_track
    auto added = store.add_track(id, mk_track("local", local_id, "Song A", "Artist", 100));
    CHECK(added.ok());
    if (added.ok()) {
        CHECK_EQ(added.value().track_count(), 1);
        CHECK_EQ(added.value().total_duration_seconds(), 100LL);
        CHECK(added.value().tracks.at(0).added_at > 0);          // 自动补加入时间
        CHECK_FALSE(added.value().tracks.at(0).group_key.empty());   // 自动补折叠键
    }

    // 去重：同 platform + item_id 加两次 → 仍然 1 条、不报错
    auto again = store.add_track(id, mk_track("local", local_id, "Song A", "Artist", 100));
    CHECK(again.ok());
    if (again.ok()) CHECK_EQ(again.value().track_count(), 1);

    auto with_second = store.add_track(id, mk_track("netease", "2002", "Song B", "Artist", 200));
    CHECK(with_second.ok());
    if (with_second.ok()) {
        CHECK_EQ(with_second.value().track_count(), 2);
        CHECK_EQ(with_second.value().total_duration_seconds(), 300LL);
    }

    // 删除不存在的曲目 → 无操作、不报错
    auto missing = store.remove_track(id, "local", "does-not-exist");
    CHECK(missing.ok());
    if (missing.ok()) CHECK_EQ(missing.value().track_count(), 2);

    // move：越界钳制到最后 / 最前
    auto to_tail = store.move_track(id, "local", local_id, 99);
    CHECK(to_tail.ok());
    if (to_tail.ok()) {
        CHECK_EQ(to_tail.value().track_count(), 2);
        CHECK_EQ(to_tail.value().tracks.at(0).item_id, std::string("2002"));
        CHECK_EQ(to_tail.value().tracks.at(1).item_id, local_id);
    }
    auto to_head = store.move_track(id, "local", local_id, -5);
    CHECK(to_head.ok());
    if (to_head.ok()) {
        CHECK_EQ(to_head.value().tracks.at(0).item_id, local_id);
        CHECK_EQ(to_head.value().tracks.at(1).item_id, std::string("2002"));
    }
    // 移到原位 → 顺序不变
    auto same = store.move_track(id, "local", local_id, 0);
    CHECK(same.ok());
    if (same.ok()) CHECK_EQ(same.value().tracks.at(0).item_id, local_id);
    // 不存在的曲目 move → 无操作
    auto no_move = store.move_track(id, "netease", "nope", 0);
    CHECK(no_move.ok());
    if (no_move.ok()) CHECK_EQ(no_move.value().track_count(), 2);

    // remove_track 落盘
    auto removed = store.remove_track(id, "local", local_id);
    CHECK(removed.ok());
    if (removed.ok()) {
        CHECK_EQ(removed.value().track_count(), 1);
        CHECK_EQ(removed.value().total_duration_seconds(), 200LL);
    }
    auto reloaded = store.get(id);
    CHECK(reloaded.ok());
    if (reloaded.ok()) {
        CHECK_EQ(reloaded.value().track_count(), 1);
        CHECK_EQ(reloaded.value().tracks.at(0).item_id, std::string("2002"));
        CHECK_EQ(reloaded.value().tracks.at(0).platform, std::string("netease"));
    }

    drop_root(root);
}

TEST_CASE(test_collections_playlist_rename_refreshes_updated_at) {
    const auto root = fresh_root("playlist_rename");
    LibraryPaths paths(root);
    PlaylistStore store(paths);

    auto created = store.create("old name", "old description");
    CHECK(created.ok());
    if (!created.ok()) {
        drop_root(root);
        return;
    }
    const std::string id = created.value().id;
    // 把时间戳改成过去，确保 rename 刷新 updated_at 可观测（秒级分辨率下 sleep 2ms 不可靠）
    CHECK(patch_json_field(paths.playlist_file(id), "created_at", 1));
    CHECK(patch_json_field(paths.playlist_file(id), "updated_at", 1));

    auto renamed = store.rename(id, "new name", "new description");
    CHECK(renamed.ok());
    if (renamed.ok()) {
        CHECK_EQ(renamed.value().name, std::string("new name"));
        CHECK_EQ(renamed.value().description, std::string("new description"));
        CHECK_EQ(renamed.value().created_at, 1LL);       // 创建时间不变
        CHECK(renamed.value().updated_at > 1LL);         // 修改时间刷新
    }
    auto reloaded = store.get(id);
    CHECK(reloaded.ok());
    if (reloaded.ok()) CHECK_EQ(reloaded.value().name, std::string("new name"));

    drop_root(root);
}

TEST_CASE(test_collections_playlist_not_found_and_remove) {
    const auto root = fresh_root("playlist_missing");
    LibraryPaths paths(root);
    PlaylistStore store(paths);
    CHECK(store.ensure_layout().ok());

    auto missing = store.get("pl_9999");
    CHECK_FALSE(missing.ok());
    CHECK(missing.status().code == ErrorCode::PlaylistNotFound);

    auto empty_id = store.get("");
    CHECK_FALSE(empty_id.ok());
    CHECK(empty_id.status().code == ErrorCode::PlaylistNotFound);

    auto unsafe = store.get("..\\escape");
    CHECK_FALSE(unsafe.ok());
    CHECK(unsafe.status().code == ErrorCode::PlaylistNotFound);

    CHECK_FALSE(store.remove("pl_9999"));
    CHECK_FALSE(store.remove(""));

    auto created = store.create("temp");
    CHECK(created.ok());
    if (created.ok()) {
        CHECK(store.remove(created.value().id));
        CHECK_FALSE(store.remove(created.value().id));       // 第二次返回 false
        CHECK_FALSE(std::filesystem::exists(paths.playlist_file(created.value().id)));
    }
    auto listed = store.list();
    CHECK(listed.ok());
    if (listed.ok()) CHECK_EQ(listed.value().size(), size_t{0});

    drop_root(root);
}

TEST_CASE(test_collections_playlist_broken_json_is_skipped) {
    const auto root = fresh_root("playlist_broken");
    LibraryPaths paths(root);
    PlaylistStore store(paths);

    auto good = store.create("good");
    CHECK(good.ok());
    // 非法 JSON
    CHECK(write_raw(paths.playlist_file("pl_broken"), "{ this is not json"));
    // 合法 JSON 但不是对象
    CHECK(write_raw(paths.playlist_file("pl_array"), "[1, 2, 3]"));

    auto listed = store.list();
    CHECK(listed.ok());     // 单条损坏不影响整体
    if (listed.ok()) {
        CHECK_EQ(listed.value().size(), size_t{1});
        CHECK_EQ(listed.value().at(0).name, std::string("good"));
    }

    auto broken = store.get("pl_broken");
    CHECK_FALSE(broken.ok());
    CHECK(broken.status().code == ErrorCode::InvalidMetadata);

    auto array = store.get("pl_array");
    CHECK_FALSE(array.ok());
    CHECK(array.status().code == ErrorCode::InvalidMetadata);

    // 损坏条目可以被显式重建覆盖（同 id 再 create 走 source_id 路径；这里直接用 rename 修复）
    auto repaired = store.rename("pl_broken", "repaired", "");
    CHECK_FALSE(repaired.ok());   // rename 不覆盖损坏文件（仍报 1007）
    CHECK(repaired.status().code == ErrorCode::InvalidMetadata);

    drop_root(root);
}

TEST_CASE(test_collections_playlist_platform_source_id_is_idempotent) {
    const auto root = fresh_root("playlist_source");
    LibraryPaths paths(root);
    PlaylistStore store(paths);

    auto first = store.create("netease list", "desc", "netease", "12345");
    CHECK(first.ok());
    if (!first.ok()) {
        drop_root(root);
        return;
    }
    CHECK_EQ(first.value().id, std::string("netease_12345"));
    CHECK_EQ(first.value().platform, std::string("netease"));
    CHECK_EQ(first.value().owner, std::string("platform"));
    const long long created_at = first.value().created_at;

    // 同参数再次 create → 同一个 id（幂等），不新建文件
    auto second = store.create("netease list", "desc", "netease", "12345");
    CHECK(second.ok());
    if (second.ok()) {
        CHECK_EQ(second.value().id, std::string("netease_12345"));
        CHECK_EQ(second.value().created_at, created_at);
        CHECK_EQ(second.value().track_count(), 0);
    }
    auto listed = store.list();
    CHECK(listed.ok());
    if (listed.ok()) CHECK_EQ(listed.value().size(), size_t{1});

    // 在线歌单同步：完整曲目表覆盖，重复同步不再刷新 updated_at
    const std::vector<PlaylistTrack> tracks = {
        mk_track("netease", "1", "A", "X", 100),
        mk_track("netease", "2", "B", "Y", 200),
    };
    auto synced = store.replace_tracks("netease_12345", tracks);
    CHECK(synced.ok());
    long long synced_at = 0;
    if (synced.ok()) {
        CHECK_EQ(synced.value().track_count(), 2);
        CHECK_EQ(synced.value().total_duration_seconds(), 300LL);
        CHECK_EQ(synced.value().created_at, created_at);
        synced_at = synced.value().updated_at;
    }
    auto resynced = store.replace_tracks("netease_12345", tracks);
    CHECK(resynced.ok());
    if (resynced.ok()) {
        CHECK_EQ(resynced.value().track_count(), 2);
        CHECK_EQ(resynced.value().updated_at, synced_at);   // 内容一致 → 不刷新
    }
    auto shrunk = store.replace_tracks("netease_12345", {tracks.at(0)});
    CHECK(shrunk.ok());
    if (shrunk.ok()) CHECK_EQ(shrunk.value().track_count(), 1);

    // 不安全字符被安全化，仍落在 playlists 目录下（不产生子目录）
    auto unsafe = store.create("weird", "", "netease", "a/b:c");
    CHECK(unsafe.ok());
    if (unsafe.ok()) {
        CHECK_EQ(unsafe.value().id, std::string("netease_a_b_c"));
        CHECK(std::filesystem::is_regular_file(paths.playlist_file("netease_a_b_c")));
        CHECK_EQ(paths.playlist_file(unsafe.value().id).parent_path(), paths.playlists());
    }

    drop_root(root);
}

// ---------------------------------------------------------------- 专辑

TEST_CASE(test_collections_album_create_tracks_list) {
    const auto root = fresh_root("album_basic");
    LibraryPaths paths(root);
    AlbumStore store(paths);

    CHECK(store.ensure_layout().ok());
    CHECK(std::filesystem::is_directory(paths.albums()));

    auto first = store.create("Album A", "Artist A", "local", "", "covers/a.jpg", 2001);
    CHECK(first.ok());
    if (!first.ok()) {
        drop_root(root);
        return;
    }
    CHECK_EQ(first.value().id, std::string("al_0001"));
    CHECK_EQ(first.value().year, 2001);
    CHECK_EQ(first.value().pic, std::string("covers/a.jpg"));
    CHECK(first.value().created_at > 0);
    CHECK_EQ(first.value().created_at, first.value().updated_at);
    CHECK_EQ(first.value().platform, std::string("local"));
    CHECK(std::filesystem::is_regular_file(paths.album_file("al_0001")));

    auto second = store.create("Album B", "Artist B");
    CHECK(second.ok());
    if (second.ok()) CHECK_EQ(second.value().id, std::string("al_0002"));

    // 曲目：添加 + 去重 + 总时长
    const std::string id = first.value().id;
    auto added = store.add_track(id, mk_track("local", fake_sha(1), "T1", "Artist A", 120));
    CHECK(added.ok());
    auto added2 = store.add_track(id, mk_track("local", fake_sha(2), "T2", "Artist A", 180));
    CHECK(added2.ok());
    auto dedup = store.add_track(id, mk_track("local", fake_sha(1), "T1", "Artist A", 120));
    CHECK(dedup.ok());
    if (dedup.ok()) {
        CHECK_EQ(dedup.value().track_count(), 2);
        CHECK_EQ(dedup.value().total_duration_seconds(), 300LL);
    }

    // 排序：updated_at 降序
    CHECK(patch_json_field(paths.album_file("al_0001"), "updated_at", 50));
    CHECK(patch_json_field(paths.album_file("al_0002"), "updated_at", 60));
    auto listed = store.list();
    CHECK(listed.ok());
    if (listed.ok()) {
        CHECK_EQ(listed.value().size(), size_t{2});
        CHECK_EQ(listed.value().at(0).id, std::string("al_0002"));
        CHECK_EQ(listed.value().at(1).id, std::string("al_0001"));
    }

    // remove_track / replace_tracks
    auto removed = store.remove_track(id, "local", fake_sha(1));
    CHECK(removed.ok());
    if (removed.ok()) {
        CHECK_EQ(removed.value().track_count(), 1);
        CHECK_EQ(removed.value().total_duration_seconds(), 180LL);
    }
    auto replaced = store.replace_tracks(id, {mk_track("netease", "9001", "T9", "Z", 60)});
    CHECK(replaced.ok());
    if (replaced.ok()) CHECK_EQ(replaced.value().track_count(), 1);
    auto reloaded = store.get(id);
    CHECK(reloaded.ok());
    if (reloaded.ok()) CHECK_EQ(reloaded.value().tracks.at(0).item_id, std::string("9001"));

    // 不存在 → 1031；remove 返回 false
    auto missing = store.get("al_9999");
    CHECK_FALSE(missing.ok());
    CHECK(missing.status().code == ErrorCode::AlbumNotFound);
    CHECK_FALSE(store.remove("al_9999"));
    auto missing_track = store.add_track("al_9999", mk_track("local", fake_sha(3), "x", "y", 1));
    CHECK_FALSE(missing_track.ok());
    CHECK(missing_track.status().code == ErrorCode::AlbumNotFound);

    // 平台来源 id + 幂等
    auto platform_album = store.create("Album P", "Artist P", "netease", "777");
    CHECK(platform_album.ok());
    if (platform_album.ok()) {
        CHECK_EQ(platform_album.value().id, std::string("netease_777"));
        auto again = store.create("Album P", "Artist P", "netease", "777");
        CHECK(again.ok());
        if (again.ok()) {
            CHECK_EQ(again.value().id, std::string("netease_777"));
            CHECK_EQ(again.value().created_at, platform_album.value().created_at);
        }
    }

    CHECK(store.remove(id));
    CHECK_FALSE(std::filesystem::exists(paths.album_file(id)));

    drop_root(root);
}

TEST_CASE(test_collections_album_broken_json_is_skipped) {
    const auto root = fresh_root("album_broken");
    LibraryPaths paths(root);
    AlbumStore store(paths);

    auto good = store.create("Good Album", "Artist");
    CHECK(good.ok());
    CHECK(write_raw(paths.album_file("al_broken"), "{ broken"));

    auto listed = store.list();
    CHECK(listed.ok());
    if (listed.ok()) {
        CHECK_EQ(listed.value().size(), size_t{1});
        CHECK_EQ(listed.value().at(0).name, std::string("Good Album"));
    }
    auto broken = store.get("al_broken");
    CHECK_FALSE(broken.ok());
    CHECK(broken.status().code == ErrorCode::InvalidMetadata);

    drop_root(root);
}

// ---------------------------------------------------------------- MV

TEST_CASE(test_collections_mv_put_overwrite_and_conflict) {
    const auto root = fresh_root("mv_put");
    LibraryPaths paths(root);
    MvStore store(paths);

    CHECK(store.ensure_layout().ok());
    CHECK(std::filesystem::is_directory(paths.mv()));

    MvInfo mv;
    mv.sha = fake_sha(1);
    mv.platform = "netease";
    mv.item_id = "mv100";
    mv.title = "MV One";
    mv.artist = "Artist";
    mv.album = "Album";
    // location 留空：允许「尚未下载」

    auto put1 = store.put(mv);
    CHECK(put1.ok());
    if (!put1.ok()) {
        drop_root(root);
        return;
    }
    CHECK_EQ(put1.value().sha, mv.sha);
    CHECK_EQ(put1.value().version, std::string("1.0.0"));
    CHECK(put1.value().created_at > 0);
    CHECK(std::filesystem::is_regular_file(paths.mv_file(mv.sha)));
    CHECK(std::filesystem::is_regular_file(paths.mv_dir(mv.sha) / "mv.json"));
    const long long created_at = put1.value().created_at;

    // 同 sha 同内容 → 原样返回，不报错
    auto put2 = store.put(mv);
    CHECK(put2.ok());
    if (put2.ok()) {
        CHECK_EQ(put2.value().created_at, created_at);
        CHECK_EQ(put2.value().title, std::string("MV One"));
    }

    // 同 sha 不同内容 → 覆盖成功且内容更新
    MvInfo changed = mv;
    changed.title = "MV One Remaster";
    changed.duration_seconds = 240;
    changed.location = "mv/" + mv.sha + "/video.mp4";
    auto put3 = store.put(changed);
    CHECK(put3.ok());
    if (put3.ok()) CHECK_EQ(put3.value().created_at, created_at);   // 创建时间保留
    auto fetched = store.get(mv.sha);
    CHECK(fetched.ok());
    if (fetched.ok()) {
        CHECK_EQ(fetched.value().title, std::string("MV One Remaster"));
        CHECK_EQ(fetched.value().duration_seconds, 240);
        CHECK_EQ(fetched.value().location, "mv/" + mv.sha + "/video.mp4");
        CHECK_EQ(fetched.value().created_at, created_at);
    }

    // 新 sha 但同 (platform, item_id) → 1023
    MvInfo other;
    other.sha = fake_sha(2);
    other.platform = "netease";
    other.item_id = "mv100";
    other.title = "MV Other";
    auto conflict = store.put(other);
    CHECK_FALSE(conflict.ok());
    CHECK(conflict.status().code == ErrorCode::BaseConflict);
    CHECK_FALSE(std::filesystem::exists(paths.mv_file(other.sha)));

    // sha 为空 → 1007
    MvInfo no_sha;
    no_sha.platform = "local";
    no_sha.title = "No Sha";
    auto invalid = store.put(no_sha);
    CHECK_FALSE(invalid.ok());
    CHECK(invalid.status().code == ErrorCode::InvalidMetadata);

    // get 不存在 → 1032
    auto missing = store.get(fake_sha(9));
    CHECK_FALSE(missing.ok());
    CHECK(missing.status().code == ErrorCode::MvNotFound);
    auto empty = store.get("");
    CHECK_FALSE(empty.ok());
    CHECK(empty.status().code == ErrorCode::MvNotFound);

    drop_root(root);
}

TEST_CASE(test_collections_mv_find_list_remove_and_broken) {
    const auto root = fresh_root("mv_find");
    LibraryPaths paths(root);
    MvStore store(paths);

    MvInfo a;
    a.sha = fake_sha(3);
    a.platform = "local";
    a.item_id = "m3";
    a.title = "A";
    CHECK(store.put(a).ok());

    MvInfo b;
    b.sha = fake_sha(4);
    b.platform = "netease";
    b.item_id = "m4";
    b.title = "B";
    CHECK(store.put(b).ok());

    // find_by_source
    auto found = store.find_by_source("netease", "m4");
    CHECK(found.ok());
    if (found.ok()) {
        CHECK_EQ(found.value().sha, b.sha);
        CHECK_EQ(found.value().title, std::string("B"));
    }
    auto not_found = store.find_by_source("netease", "nope");
    CHECK_FALSE(not_found.ok());
    CHECK(not_found.status().code == ErrorCode::MvNotFound);
    auto empty_source = store.find_by_source("", "");
    CHECK_FALSE(empty_source.ok());
    CHECK(empty_source.status().code == ErrorCode::MvNotFound);

    // 排序：created_at 降序，同值按 sha 升序
    CHECK(patch_json_field(paths.mv_file(a.sha), "created_at", 100));
    CHECK(patch_json_field(paths.mv_file(b.sha), "created_at", 200));
    auto descending = store.list();
    CHECK(descending.ok());
    if (descending.ok()) {
        CHECK_EQ(descending.value().size(), size_t{2});
        CHECK_EQ(descending.value().at(0).sha, b.sha);
        CHECK_EQ(descending.value().at(1).sha, a.sha);
    }
    CHECK(patch_json_field(paths.mv_file(a.sha), "created_at", 300));
    CHECK(patch_json_field(paths.mv_file(b.sha), "created_at", 300));
    auto tied = store.list();
    CHECK(tied.ok());
    if (tied.ok()) {
        CHECK_EQ(tied.value().size(), size_t{2});
        CHECK_EQ(tied.value().at(0).sha, a.sha);   // fake_sha(3) < fake_sha(4)
        CHECK_EQ(tied.value().at(1).sha, b.sha);
    }

    // 损坏的 mv.json：list 跳过，get → 1007
    CHECK(write_raw(paths.mv_file(fake_sha(7)), "{ nope"));
    auto listed = store.list();
    CHECK(listed.ok());
    if (listed.ok()) CHECK_EQ(listed.value().size(), size_t{2});
    auto broken = store.get(fake_sha(7));
    CHECK_FALSE(broken.ok());
    CHECK(broken.status().code == ErrorCode::InvalidMetadata);
    auto broken_source = store.find_by_source("local", "m3");
    CHECK(broken_source.ok());   // 损坏条目没有 platform/item_id，不影响正常条目

    // remove：删除整个 mv/<sha>/ 目录
    CHECK(std::filesystem::is_directory(paths.mv_dir(a.sha)));
    CHECK(store.remove(a.sha));
    CHECK_FALSE(std::filesystem::exists(paths.mv_dir(a.sha)));
    CHECK_FALSE(store.remove(a.sha));      // 第二次 false
    CHECK_FALSE(store.remove(""));

    auto after = store.list();
    CHECK(after.ok());
    if (after.ok()) {
        CHECK_EQ(after.value().size(), size_t{1});
        CHECK_EQ(after.value().at(0).sha, b.sha);
    }
    auto gone = store.get(a.sha);
    CHECK_FALSE(gone.ok());
    CHECK(gone.status().code == ErrorCode::MvNotFound);

    CHECK(store.remove(fake_sha(7)));      // 只删目录（无有效 JSON 也算存在）

    drop_root(root);
}

// ---------------------------------------------------------------- 曲目引用

TEST_CASE(test_collections_track_from_base_and_json_roundtrip) {
    BaseInfo base;
    base.platform = "local";
    base.version = "1.0.0";
    base.sha = fake_sha(5);
    base.location = "tracks/" + base.sha + "/audio.flac";
    base.name = "Song X";
    base.author = "Artist X";
    base.duration_seconds = 201;

    const PlaylistTrack local = PlaylistTrack::from_base(base);
    CHECK_EQ(local.platform, std::string("local"));
    CHECK_EQ(local.item_id, base.sha);          // local 用 sha 作 item_id
    CHECK_EQ(local.sha, base.sha);
    CHECK_EQ(local.name, std::string("Song X"));
    CHECK_EQ(local.author, std::string("Artist X"));
    CHECK_EQ(local.duration_seconds, 201);
    CHECK_EQ(local.group_key, base.group_key());
    CHECK_EQ(local.key(), "local:" + base.sha);

    BaseInfo online = base;
    online.platform = "netease";
    online.item_id = "2002";
    CHECK_EQ(PlaylistTrack::from_base(online).item_id, std::string("2002"));
    CHECK_EQ(PlaylistTrack::from_base(online).key(), std::string("netease:2002"));

    // 空 base → 不崩溃
    const PlaylistTrack empty_track = PlaylistTrack::from_base(BaseInfo{});
    CHECK_EQ(empty_track.key(), std::string(":"));

    // to_json → from_json 字段一致
    PlaylistTrack track = local;
    track.added_at = 1700000000;
    const PlaylistTrack back = PlaylistTrack::from_json(track.to_json());
    CHECK_EQ(back.platform, track.platform);
    CHECK_EQ(back.item_id, track.item_id);
    CHECK_EQ(back.sha, track.sha);
    CHECK_EQ(back.name, track.name);
    CHECK_EQ(back.author, track.author);
    CHECK_EQ(back.duration_seconds, track.duration_seconds);
    CHECK_EQ(back.group_key, track.group_key);
    CHECK_EQ(back.added_at, track.added_at);
    CHECK_EQ(back.key(), track.key());
    CHECK_EQ(back.to_json().dump(), track.to_json().dump());

    // Playlist / Album / MvInfo 回环
    Playlist playlist;
    playlist.id = "pl_0007";
    playlist.name = "mix";
    playlist.description = "desc";
    playlist.pic = "covers/p.jpg";
    playlist.platform = "netease";
    playlist.source_id = "777";
    playlist.owner = "platform";
    playlist.created_at = 111;
    playlist.updated_at = 222;
    playlist.tracks.push_back(track);
    const Playlist playlist_back = Playlist::from_json(playlist.to_json());
    CHECK_EQ(playlist_back.id, playlist.id);
    CHECK_EQ(playlist_back.name, playlist.name);
    CHECK_EQ(playlist_back.description, playlist.description);
    CHECK_EQ(playlist_back.pic, playlist.pic);
    CHECK_EQ(playlist_back.platform, playlist.platform);
    CHECK_EQ(playlist_back.source_id, playlist.source_id);
    CHECK_EQ(playlist_back.owner, playlist.owner);
    CHECK_EQ(playlist_back.created_at, playlist.created_at);
    CHECK_EQ(playlist_back.updated_at, playlist.updated_at);
    CHECK_EQ(playlist_back.track_count(), 1);
    CHECK_EQ(playlist_back.total_duration_seconds(), 201LL);
    CHECK_EQ(playlist_back.tracks.at(0).key(), track.key());

    Album album;
    album.id = "al_0007";
    album.name = "Album";
    album.artist = "Artist";
    album.pic = "covers/a.jpg";
    album.description = "ad";
    album.platform = "netease";
    album.source_id = "888";
    album.year = 1999;
    album.created_at = 333;
    album.updated_at = 444;
    album.tracks.push_back(track);
    const Album album_back = Album::from_json(album.to_json());
    CHECK_EQ(album_back.id, album.id);
    CHECK_EQ(album_back.name, album.name);
    CHECK_EQ(album_back.artist, album.artist);
    CHECK_EQ(album_back.pic, album.pic);
    CHECK_EQ(album_back.platform, album.platform);
    CHECK_EQ(album_back.source_id, album.source_id);
    CHECK_EQ(album_back.year, album.year);
    CHECK_EQ(album_back.created_at, album.created_at);
    CHECK_EQ(album_back.updated_at, album.updated_at);
    CHECK_EQ(album_back.track_count(), 1);
    CHECK_EQ(album_back.total_duration_seconds(), 201LL);

    MvInfo mv;
    mv.sha = fake_sha(6);
    mv.platform = "netease";
    mv.item_id = "mv9";
    mv.version = "2.0.0";
    mv.title = "MV";
    mv.artist = "Artist";
    mv.album = "Album";
    mv.location = "https://example.invalid/v.mp4";
    mv.pic = "covers/mv.jpg";
    mv.duration_seconds = 300;
    mv.width = 1920;
    mv.height = 1080;
    mv.created_at = 555;
    const MvInfo mv_back = MvInfo::from_json(mv.to_json());
    CHECK_EQ(mv_back.sha, mv.sha);
    CHECK_EQ(mv_back.platform, mv.platform);
    CHECK_EQ(mv_back.item_id, mv.item_id);
    CHECK_EQ(mv_back.version, mv.version);
    CHECK_EQ(mv_back.title, mv.title);
    CHECK_EQ(mv_back.artist, mv.artist);
    CHECK_EQ(mv_back.album, mv.album);
    CHECK_EQ(mv_back.location, mv.location);
    CHECK_EQ(mv_back.pic, mv.pic);
    CHECK_EQ(mv_back.duration_seconds, mv.duration_seconds);
    CHECK_EQ(mv_back.width, mv.width);
    CHECK_EQ(mv_back.height, mv.height);
    CHECK_EQ(mv_back.created_at, mv.created_at);

    CHECK(collections_now_seconds() > 0);
}
