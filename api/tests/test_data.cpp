// 设置页「数据」能力测试：GET /music/data 占用概览、POST /music/data/purge 清除全部数据
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "radmusic/api.hpp"
#include "radmusic/engine.hpp"
#include "radmusic/settings.hpp"
#include "radmusic/storage.hpp"
#include "test_framework.hpp"

using namespace radmusic;

namespace {

std::filesystem::path fresh_root(const std::string& name) {
    const auto root = std::filesystem::path("build") / "tmp_data_api" / name;
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    return root;
}

void write_file(const std::filesystem::path& path, const std::string& content) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.close();
}

bool is_empty_dir(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_directory(path, ec)) return false;
    std::filesystem::directory_iterator it(path, ec);
    return !ec && it == std::filesystem::directory_iterator();
}

struct DataFixture {
    std::filesystem::path root;
    std::unique_ptr<LibraryPaths> paths;
    std::unique_ptr<PluginRegistry> plugins;
    std::unique_ptr<CoreEngine> engine;
    std::unique_ptr<MetaCache> meta;
    std::unique_ptr<LibraryIndex> index;
    std::unique_ptr<SettingsStore> settings;
    std::unique_ptr<ApiDispatcher> api;

    explicit DataFixture(const std::string& name) {
        root = fresh_root(name);
        paths = std::make_unique<LibraryPaths>(root);
        paths->ensure_layout();
        plugins = std::make_unique<PluginRegistry>();
        engine = std::make_unique<CoreEngine>(*paths, *plugins);
        meta = std::make_unique<MetaCache>(*paths);
        index = std::make_unique<LibraryIndex>(*paths);
        index->load();
        settings = std::make_unique<SettingsStore>(*paths);
        settings->load();
        settings->values().theme = "ocean";
        settings->save();
        api = std::make_unique<ApiDispatcher>(*paths, *plugins, *engine, *meta, *index);
        api->set_settings(settings.get());
    }
};

json::Value targets_of(std::initializer_list<const char*> ids) {
    json::Value list = json::Value::array();
    for (const char* id : ids) list.push_back(id);
    return list;
}

json::Value purge_body(const json::Value& targets, const std::string& confirm) {
    json::Value body = json::Value::object();
    body.set("targets", targets);
    if (!confirm.empty()) body.set("confirm", confirm);
    return body;
}

}  // namespace

TEST_CASE(test_api_data_overview) {
    DataFixture f("overview");
    write_file(f.paths->track_dir("sha-a") / "audio.flac", "audio-bytes");
    write_file(f.paths->cache() / "meta" / "song" / "quality" / "base.json", "{\"a\":1}");
    write_file(f.paths->covers() / "c1.jpg", "cover");
    write_file(f.paths->logs() / "radmusic.log", "log line");

    const ApiResponse overview = f.api->call("GET /music/data");
    CHECK(overview.ok());
    CHECK(overview.data.get_int("usage_bytes", 0) > 0);
    CHECK_EQ(overview.data.get_string("root"), f.root.generic_u8string());

    const json::Value groups = overview.data.get("groups");
    CHECK_EQ(static_cast<long long>(groups.as_array().size()), 10);

    long long tracks_files = -1;
    long long cache_files = -1;
    for (const auto& group : groups.as_array()) {
        const std::string id = group.get_string("id");
        if (id == "tracks") tracks_files = group.get_int("files", 0);
        if (id == "cache") cache_files = group.get_int("files", 0);
    }
    // tracks 类含 base.json 元数据；cache 类只统计 cache/ 下非 meta 部分
    CHECK_EQ(tracks_files, 2LL);
    CHECK_EQ(cache_files, 0LL);
}

TEST_CASE(test_api_data_purge_requires_confirm) {
    DataFixture f("confirm");
    write_file(f.paths->track_dir("sha-b") / "audio.flac", "audio-bytes");

    // 缺确认短语 → 拒绝，文件保留
    const ApiResponse refused = f.api->call("POST /music/data/purge", purge_body(targets_of({"all"}), ""));
    CHECK_FALSE(refused.ok());
    CHECK(std::filesystem::exists(f.paths->track_dir("sha-b") / "audio.flac"));

    // 未知分类 → 拒绝，文件保留
    const ApiResponse unknown =
        f.api->call("POST /music/data/purge", purge_body(targets_of({"nope"}), "PURGE"));
    CHECK_FALSE(unknown.ok());
    CHECK(std::filesystem::exists(f.paths->track_dir("sha-b") / "audio.flac"));

    // 空 targets → 拒绝
    const ApiResponse empty =
        f.api->call("POST /music/data/purge", purge_body(json::Value::array(), "PURGE"));
    CHECK_FALSE(empty.ok());
}

TEST_CASE(test_api_data_purge_all) {
    DataFixture f("purge_all");
    write_file(f.paths->track_dir("sha-c") / "audio.flac", "audio-bytes");
    write_file(f.paths->cache() / "meta" / "song" / "quality" / "base.json", "{\"a\":1}");
    write_file(f.paths->covers() / "c1.jpg", "cover");
    write_file(f.paths->logs() / "radmusic.log", "log line");
    write_file(f.paths->playlist_file("pl_1"), "{}");
    write_file(f.paths->album_file("al_1"), "{}");
    write_file(f.paths->mv_dir("sha-c") / "mv.json", "{}");
    write_file(f.paths->presets() / "eq" / "bass.json", "{}");
    write_file(f.paths->plugins() / "demo" / "plugin.json", "{}");
    write_file(f.paths->index_file(), "{\"entries\":{}}");
    write_file(f.paths->usec_file(), "{}");

    const ApiResponse purged =
        f.api->call("POST /music/data/purge", purge_body(targets_of({"all"}), "PURGE"));
    CHECK(purged.ok());
    CHECK(purged.data.get_int("removed_files", 0) >= 10);
    CHECK_EQ(purged.data.get_int("failed", 0), 0LL);
    CHECK(purged.data.get_bool("dry_run", true) == false);

    // 目录被清空并重建，服务可继续使用（tracks/ 会重建 .staging，故只看文件）
    CHECK(is_empty_dir(f.paths->covers()));
    CHECK(is_empty_dir(f.paths->playlists()));
    CHECK(is_empty_dir(f.paths->albums()));
    CHECK(is_empty_dir(f.paths->mv()));
    CHECK(is_empty_dir(f.paths->presets()));
    CHECK(is_empty_dir(f.paths->plugins()));
    CHECK(std::filesystem::is_directory(f.paths->tracks_staging()));
    CHECK(std::filesystem::is_directory(f.paths->audio_cache()));
    CHECK_FALSE(std::filesystem::exists(f.paths->track_dir("sha-c") / "audio.flac"));

    // 索引清空、设置回到默认值并重新落盘
    CHECK_EQ(static_cast<long long>(f.index->size()), 0LL);
    SettingsStore reloaded(*f.paths);
    reloaded.load();
    CHECK_EQ(reloaded.values().theme, std::string("violet"));

    // 概览重新变小：只剩重建的空索引 / 默认设置等元文件
    long long remaining_tracks = -1;
    long long remaining_usage = -1;
    const ApiResponse after = f.api->call("GET /music/data");
    CHECK(after.ok());
    remaining_usage = after.data.get_int("usage_bytes", -1);
    for (const auto& group : after.data.get("groups").as_array()) {
        if (group.get_string("id") == "tracks") remaining_tracks = group.get_int("files", -1);
    }
    CHECK_EQ(remaining_tracks, 0LL);
    CHECK(remaining_usage >= 0);
    CHECK(remaining_usage < 8192);
}

TEST_CASE(test_api_data_purge_subset_keeps_others) {
    DataFixture f("purge_subset");
    write_file(f.paths->track_dir("sha-d") / "audio.flac", "audio-bytes");
    write_file(f.paths->logs() / "radmusic.log", "log line");
    write_file(f.paths->covers() / "c1.jpg", "cover");

    const ApiResponse purged =
        f.api->call("POST /music/data/purge", purge_body(targets_of({"logs"}), "PURGE"));
    CHECK(purged.ok());
    CHECK_EQ(purged.data.get_int("removed_files", 0), 1LL);

    CHECK(is_empty_dir(f.paths->logs()));
    CHECK(std::filesystem::exists(f.paths->track_dir("sha-d") / "audio.flac"));
    CHECK(std::filesystem::exists(f.paths->covers() / "c1.jpg"));
    // 设置未被清除
    SettingsStore reloaded(*f.paths);
    reloaded.load();
    CHECK_EQ(reloaded.values().theme, std::string("ocean"));
}

TEST_CASE(test_api_data_purge_tracks_also_drops_metadata) {
    DataFixture f("purge_tracks");
    write_file(f.paths->track_dir("sha-f") / "audio.flac", "audio-bytes");
    write_file(f.paths->meta_dir(MetaPath{"song", "quality"}) / "base.json", "{\"a\":1}");
    write_file(f.paths->audio_cache() / "sha-f.audio", "cached-audio");

    const ApiResponse purged =
        f.api->call("POST /music/data/purge", purge_body(targets_of({"tracks"}), "PURGE"));
    CHECK(purged.ok());
    // 曲目与元数据一起清掉，避免曲库里留下无法播放的条目
    CHECK_FALSE(std::filesystem::exists(f.paths->track_dir("sha-f") / "audio.flac"));
    CHECK_FALSE(std::filesystem::exists(f.paths->meta_dir(MetaPath{"song", "quality"}) / "base.json"));
    // 纯音频缓存属于 cache 类，未被连带删除
    CHECK(std::filesystem::exists(f.paths->audio_cache() / "sha-f.audio"));
}

TEST_CASE(test_api_data_purge_dry_run_keeps_files) {
    DataFixture f("dry_run");
    write_file(f.paths->track_dir("sha-e") / "audio.flac", "audio-bytes");

    json::Value body = purge_body(targets_of({"tracks"}), "PURGE");
    body.set("dry_run", true);
    const ApiResponse preview = f.api->call("POST /music/data/purge", body);
    CHECK(preview.ok());
    CHECK(preview.data.get_bool("dry_run", false) == true);
    CHECK(preview.data.get_int("removed_files", 0) >= 1);
    CHECK(std::filesystem::exists(f.paths->track_dir("sha-e") / "audio.flac"));
}
