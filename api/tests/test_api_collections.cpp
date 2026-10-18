// v0.10 新增能力的 API 测试：歌单 / 专辑 / MV / 日志（Readme 2.14–2.17）
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "radmusic/api.hpp"
#include "radmusic/collections.hpp"
#include "radmusic/crypto.hpp"
#include "radmusic/log.hpp"
#include "radmusic/storage.hpp"
#include "test_framework.hpp"

using namespace radmusic;

namespace {

std::filesystem::path fresh_root(const std::string& name) {
    const auto root = std::filesystem::path("build") / "tmp_collections_api" / name;
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    return root;
}

std::string write_file(const std::filesystem::path& path, const std::string& content) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.close();
    return path.u8string();
}

Quality lossless() {
    Quality q;
    q.level = QualityLevel::Lossless;
    q.codec = "flac";
    q.lossless = true;
    q.bitrate = 1411;
    q.sample_rate = 44100;
    q.bit_depth = 16;
    q.channels = 2;
    return q;
}

struct Fixture {
    std::filesystem::path root;
    std::unique_ptr<LibraryPaths> paths;
    std::unique_ptr<PluginRegistry> plugins;
    std::unique_ptr<CoreEngine> engine;
    std::unique_ptr<MetaCache> meta;
    std::unique_ptr<LibraryIndex> index;
    std::unique_ptr<PlaylistStore> playlists;
    std::unique_ptr<AlbumStore> albums;
    std::unique_ptr<MvStore> mvs;
    std::unique_ptr<ApiDispatcher> api;

    explicit Fixture(const std::string& name) {
        root = fresh_root(name);
        paths = std::make_unique<LibraryPaths>(root);
        paths->ensure_layout();
        plugins = std::make_unique<PluginRegistry>();
        engine = std::make_unique<CoreEngine>(*paths, *plugins);
        meta = std::make_unique<MetaCache>(*paths);
        index = std::make_unique<LibraryIndex>(*paths);
        index->load();
        playlists = std::make_unique<PlaylistStore>(*paths);
        albums = std::make_unique<AlbumStore>(*paths);
        mvs = std::make_unique<MvStore>(*paths);
        api = std::make_unique<ApiDispatcher>(*paths, *plugins, *engine, *meta, *index);
        api->set_playlists(playlists.get());
        api->set_albums(albums.get());
        api->set_mvs(mvs.get());
    }

    // 造一首真实曲目并写入 base.json 缓存，返回 sha
    std::string add_local_track(const std::string& name, const std::string& content) {
        const auto incoming = root / "incoming" / (name + ".flac");
        write_file(incoming, content);
        bool ok = false;
        const std::string sha = crypto::sha256_file_hex(incoming.u8string(), &ok);
        const auto final_path = paths->track_dir(sha) / "audio.flac";
        write_file(final_path, content);

        BaseInfo base;
        base.platform = "local";
        base.version = "1.0.0";
        base.sha = sha;
        base.location = final_path.u8string();
        base.name = name;
        base.author = "Collection Artist";
        base.duration_seconds = 180;
        base.quality = lossless();
        meta->put(base);
        return sha;
    }
};

json::Value body_with(std::initializer_list<std::pair<const char*, json::Value>> items) {
    json::Value value = json::Value::object();
    for (const auto& kv : items) value.set(kv.first, kv.second);
    return value;
}

}  // namespace

TEST_CASE(test_api_playlist_lifecycle) {
    Fixture f("playlist");
    const std::string sha = f.add_local_track("Playlist Song", "playlist audio content");

    // 创建
    json::Value create = json::Value::object();
    create.set("name", "我的收藏");
    create.set("description", "测试歌单");
    const ApiResponse created = f.api->call("POST /music/playlist", create);
    CHECK(created.ok());
    const std::string id = created.data.get_string("id");
    CHECK(!id.empty());
    CHECK_EQ(created.data.get_string("name"), std::string("我的收藏"));
    CHECK_EQ(created.data.get_int("track_count"), 0);

    // 列表
    const ApiResponse list = f.api->call("GET /music/playlist");
    CHECK(list.ok());
    CHECK_EQ(list.data.get_int("total"), 1);
    CHECK_EQ(list.data.get("playlists").size(), size_t{1});

    // 加歌（用 sha，从缓存补全 name/author/duration）
    json::Value add = json::Value::object();
    add.set("sha", sha);
    const ApiResponse added = f.api->call("POST /music/playlist/" + id + "/add", add);
    CHECK(added.ok());
    if (added.ok()) {
        CHECK_EQ(added.data.get_int("track_count"), 1);
        const json::Value& track = added.data.get("tracks")[0];
        CHECK_EQ(track.get_string("sha"), sha);
        CHECK_EQ(track.get_string("name"), std::string("Playlist Song"));
        CHECK_EQ(track.get_string("platform"), std::string("local"));
        CHECK_EQ(track.get_string("author"), std::string("Collection Artist"));
        CHECK(track.get_string("group_key").find("playlist song") != std::string::npos);
    }

    // 重复添加 → 幂等（不报错、不增加）
    const ApiResponse again = f.api->call("POST /music/playlist/" + id + "/add", add);
    CHECK(again.ok());
    if (again.ok()) CHECK_EQ(again.data.get_int("track_count"), 1);

    // 再加一首并调整顺序
    const std::string sha2 = f.add_local_track("Second Song", "second audio content");
    json::Value add2 = json::Value::object();
    add2.set("sha", sha2);
    CHECK(f.api->call("POST /music/playlist/" + id + "/add", add2).ok());
    json::Value move = json::Value::object();
    move.set("platform", "local");
    move.set("item_id", sha2);
    move.set("index", 0);
    const ApiResponse moved = f.api->call("POST /music/playlist/" + id + "/move", move);
    CHECK(moved.ok());
    if (moved.ok()) {
        CHECK_EQ(moved.data.get("tracks")[0].get_string("sha"), sha2);   // 移到首位
    }

    // 移除
    json::Value remove = json::Value::object();
    remove.set("platform", "local");
    remove.set("item_id", sha);
    const ApiResponse removed = f.api->call("POST /music/playlist/" + id + "/remove", remove);
    CHECK(removed.ok());
    if (removed.ok()) CHECK_EQ(removed.data.get_int("track_count"), 1);

    // 改名
    json::Value rename = json::Value::object();
    rename.set("name", "改过的名字");
    rename.set("description", "new desc");
    const ApiResponse renamed = f.api->call("POST /music/playlist/" + id, rename);
    CHECK(renamed.ok());
    if (renamed.ok()) CHECK_EQ(renamed.data.get_string("name"), std::string("改过的名字"));

    // 删除
    const ApiResponse deleted = f.api->call("DELETE /music/playlist/" + id);
    CHECK(deleted.ok());
    const ApiResponse missing = f.api->call("GET /music/playlist/" + id);
    CHECK_FALSE(missing.ok());
    CHECK(missing.code == ErrorCode::PlaylistNotFound);
    // 歌单删除不能影响音频文件
    CHECK(std::filesystem::exists(f.paths->track_dir(sha) / "audio.flac"));
}

TEST_CASE(test_api_playlist_errors) {
    Fixture f("playlist_errors");
    // 不存在 → 1030
    CHECK(f.api->call("GET /music/playlist/pl_9999").code == ErrorCode::PlaylistNotFound);
    CHECK(f.api->call("DELETE /music/playlist/pl_9999").code == ErrorCode::PlaylistNotFound);

    // 加歌缺字段 → 1007
    json::Value create = json::Value::object();
    create.set("name", "空歌单");
    const std::string id = f.api->call("POST /music/playlist", create).data.get_string("id");
    const ApiResponse bad = f.api->call("POST /music/playlist/" + id + "/add", json::Value::object());
    CHECK_FALSE(bad.ok());
    CHECK(bad.code == ErrorCode::InvalidMetadata);

    // 未装配的模块 → 1011
    f.api->set_playlists(nullptr);
    CHECK(f.api->call("GET /music/playlist").code == ErrorCode::RequestFailed);
}

TEST_CASE(test_api_album_lifecycle) {
    Fixture f("album");
    json::Value create = json::Value::object();
    create.set("name", "Example Album");
    create.set("artist", "Example Artist");
    create.set("year", 2024);
    create.set("platform", "netease");
    create.set("source_id", "12345");
    const ApiResponse created = f.api->call("POST /music/album", create);
    CHECK(created.ok());
    const std::string id = created.data.get_string("id");
    CHECK(id.find("netease_") == 0);            // 在线来源 → <platform>_<source_id>
    CHECK_EQ(created.data.get_int("year"), 2024);

    // 同参数再建 → 幂等（同一个 id）
    const ApiResponse again = f.api->call("POST /music/album", create);
    CHECK(again.ok());
    CHECK_EQ(again.data.get_string("id"), id);

    json::Value add = json::Value::object();
    add.set("platform", "netease");
    add.set("item_id", "2002");
    add.set("name", "Track 1");
    add.set("author", "Example Artist");
    add.set("duration", 215);
    const ApiResponse added = f.api->call("POST /music/album/" + id + "/add", add);
    CHECK(added.ok());
    if (added.ok()) {
        CHECK_EQ(added.data.get_int("track_count"), 1);
        CHECK_EQ(added.data.get_int("total_duration"), 215);
    }

    const ApiResponse list = f.api->call("GET /music/album");
    CHECK(list.ok());
    CHECK_EQ(list.data.get_int("total"), 1);

    const ApiResponse removed = f.api->call("DELETE /music/album/" + id);
    CHECK(removed.ok());
    CHECK(f.api->call("GET /music/album/" + id).code == ErrorCode::AlbumNotFound);
    CHECK(f.api->call("DELETE /music/album/al_9999").code == ErrorCode::AlbumNotFound);
}

TEST_CASE(test_api_mv_lifecycle) {
    Fixture f("mv");
    const std::string sha(64, 'a');
    json::Value put = json::Value::object();
    put.set("sha", sha);
    put.set("platform", "netease");
    put.set("item_id", "mv_001");
    put.set("title", "Example Song (MV)");
    put.set("artist", "Example Artist");
    put.set("location", "https://example.invalid/mv.mp4");
    put.set("duration_seconds", 240);
    put.set("width", 1920);
    put.set("height", 1080);
    const ApiResponse created = f.api->call("POST /music/mv", put);
    CHECK(created.ok());
    if (created.ok()) {
        CHECK_EQ(created.data.get_string("sha"), sha);
        CHECK_EQ(created.data.get_string("title"), std::string("Example Song (MV)"));
        CHECK_EQ(created.data.get_int("width"), 1920);
    }

    // 读取 / 列表 / 按来源查找
    const ApiResponse got = f.api->call("GET /music/mv/" + sha);
    CHECK(got.ok());
    CHECK_EQ(got.data.get_string("platform"), std::string("netease"));
    const ApiResponse list = f.api->call("GET /music/mv");
    CHECK(list.ok());
    CHECK_EQ(list.data.get_int("total"), 1);
    const ApiResponse by_source = f.api->call("GET /music/mv?platform=netease&id=mv_001");
    CHECK(by_source.ok());
    CHECK_EQ(by_source.data.get_string("sha"), sha);

    // 同 sha 覆盖（允许）
    json::Value update = put;
    update.set("title", "Updated MV");
    CHECK(f.api->call("POST /music/mv", update).ok());
    CHECK_EQ(f.api->call("GET /music/mv/" + sha).data.get_string("title"), std::string("Updated MV"));

    // 不同 sha 但同一来源 id → 1023
    json::Value conflict = put;
    conflict.set("sha", std::string(64, 'b'));
    const ApiResponse conflicted = f.api->call("POST /music/mv", conflict);
    CHECK_FALSE(conflicted.ok());
    CHECK(conflicted.code == ErrorCode::BaseConflict);

    // 缺 sha → 1007；未知 sha → 1032
    json::Value no_sha = json::Value::object();
    no_sha.set("title", "x");
    CHECK(f.api->call("POST /music/mv", no_sha).code == ErrorCode::InvalidMetadata);
    CHECK(f.api->call("GET /music/mv/" + std::string(64, 'c')).code == ErrorCode::MvNotFound);

    // 删除
    const ApiResponse removed = f.api->call("DELETE /music/mv/" + sha);
    CHECK(removed.ok());
    CHECK(f.api->call("GET /music/mv/" + sha).code == ErrorCode::MvNotFound);
    CHECK_FALSE(std::filesystem::exists(f.paths->mv_dir(sha)));
}

TEST_CASE(test_api_logs) {
    const auto root = fresh_root("logs");
    LibraryPaths paths(root);
    paths.ensure_layout();
    LibraryIndex index(paths);
    PluginRegistry plugins;
    CoreEngine engine(paths, plugins);
    MetaCache meta(paths);
    ApiDispatcher api(paths, plugins, engine, meta, index);

    // 装配内存 sink（默认静默）
    LogConfig config;
    config.level = LogLevel::Debug;
    config.console = false;
    config.memory = true;
    config.memory_capacity = 100;
    CHECK(log_configure(config).ok());
    CHECK(log_memory_sink() != nullptr);

    RM_LOG_INFO("test", "第一条日志");
    RM_LOG_WARN("test", "第二条日志");
    RM_LOG_ERROR("http", "第三条日志");

    const ApiResponse logs = api.call("GET /music/logs?lines=10");
    CHECK(logs.ok());
    CHECK_EQ(logs.data.get_string("level"), std::string("debug"));
    CHECK(logs.data.get_int("total") >= 3);
    const json::Value& records = logs.data.get("records");
    CHECK(records.size() >= 3);
    bool found_warn = false;
    for (const auto& record : records.as_array()) {
        if (record.get_string("message") == "第二条日志") {
            found_warn = true;
            CHECK_EQ(record.get_string("level"), std::string("warn"));
            CHECK(record.get_int("ts_ms") > 0);
            CHECK_EQ(record.get_string("category"), std::string("test"));
        }
    }
    CHECK(found_warn);

    // 级别过滤
    const ApiResponse warns = api.call("GET /music/logs?level=warn");
    CHECK(warns.ok());
    for (const auto& record : warns.data.get("records").as_array()) {
        CHECK_EQ(record.get_string("level"), std::string("warn"));
    }

    // 分类过滤
    const ApiResponse http_only = api.call("GET /music/logs?category=http");
    CHECK(http_only.ok());
    for (const auto& record : http_only.data.get("records").as_array()) {
        CHECK_EQ(record.get_string("category"), std::string("http"));
    }

    // lines 上限 500
    const ApiResponse capped = api.call("GET /music/logs?lines=99999");
    CHECK(capped.ok());
    CHECK(capped.data.get("records").size() <= 500);

    // 运行时调整级别
    json::Value level = json::Value::object();
    level.set("level", "error");
    const ApiResponse changed = api.call("POST /music/logs/level", level);
    CHECK(changed.ok());
    CHECK_EQ(changed.data.get_string("level"), std::string("error"));
    CHECK(!Logger::instance().enabled(LogLevel::Info));

    // 未知级别 → 1007
    json::Value bad = json::Value::object();
    bad.set("level", "verbose");
    const ApiResponse invalid = api.call("POST /music/logs/level", bad);
    CHECK_FALSE(invalid.ok());
    CHECK(invalid.code == ErrorCode::InvalidMetadata);

    // 恢复默认，避免影响后续用例
    Logger::instance().clear_sinks();
    Logger::instance().set_level(LogLevel::Info);
}

TEST_CASE(test_api_backward_compatibility_v010) {
    // v0.10 是纯增量：旧接口字段必须保持不变
    const auto root = fresh_root("compat");
    LibraryPaths paths(root);
    paths.ensure_layout();
    LibraryIndex index(paths);
    PluginRegistry plugins;
    CoreEngine engine(paths, plugins);
    MetaCache meta(paths);
    ApiDispatcher api(paths, plugins, engine, meta, index);

    // 旧接口仍然可用且结构不变
    const ApiResponse status = api.call("GET /music/status");
    CHECK(status.ok());
    CHECK(status.data.contains("state"));
    CHECK(status.data.contains("position_ms"));
    CHECK(status.data.contains("duration_ms"));
    CHECK(status.data.contains("volume"));

    // 旧错误码语义不变
    CHECK(api.call("GET /music/nope").code == ErrorCode::RequestFailed);

    // 新增接口在模块未装配时返回 1011（与既有未装配模块行为一致）
    CHECK(api.call("GET /music/playlist").code == ErrorCode::RequestFailed);
    CHECK(api.call("GET /music/album").code == ErrorCode::RequestFailed);
    CHECK(api.call("GET /music/mv").code == ErrorCode::RequestFailed);

    // 日志接口即使没有 sink 也不崩，返回空记录（先清掉前面用例留下的内存 sink）
    LogConfig quiet;
    quiet.console = false;
    quiet.memory = false;
    CHECK(log_configure(quiet).ok());
    const ApiResponse logs = api.call("GET /music/logs");
    CHECK(logs.ok());
    CHECK_EQ(logs.data.get_int("total"), 0);

    // 旧曲库（缺少 library/mv/logs 目录）的路径辅助函数仍然可推导
    CHECK(paths.playlists().filename() == "playlists");
    CHECK(paths.albums().filename() == "albums");
    CHECK(paths.mv().filename() == "mv");
    CHECK(paths.logs().filename() == "logs");
}
