// API 层测试（Readme 第 2 节：接口一览 / 通用返回结构 / 错误码）
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "radmusic/api.hpp"
#include "radmusic/crypto.hpp"
#include "radmusic/decrypt.hpp"
#include "radmusic/engine.hpp"
#include "radmusic/groupkey.hpp"
#include "radmusic/quality.hpp"
#include "radmusic/storage.hpp"
#include "test_framework.hpp"

using namespace radmusic;

namespace {

std::filesystem::path fresh_root(const std::string& name) {
    const auto root = std::filesystem::path("build") / "tmp_api" / name;
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

Quality high_mp3() {
    Quality q;
    q.level = QualityLevel::High;
    q.codec = "mp3";
    q.lossless = false;
    q.bitrate = 320;
    return q;
}

struct RealTrack {
    std::string path;
    std::string sha;
};

RealTrack make_track(const std::filesystem::path& root, const std::string& content) {
    const auto incoming = root / "incoming" / "song.flac";
    write_file(incoming, content);
    bool ok = false;
    const std::string sha = crypto::sha256_file_hex(incoming.u8string(), &ok);
    const auto final_path = root / "tracks" / sha / "audio.flac";
    write_file(final_path, content);
    std::error_code ec;
    std::filesystem::remove_all(root / "incoming", ec);
    return RealTrack{final_path.u8string(), sha};
}

BaseInfo local_base(const RealTrack& track) {
    BaseInfo base;
    base.platform = "local";
    base.version = "1.0.0";
    base.sha = track.sha;
    base.location = track.path;
    base.name = "Example Song";
    base.author = "Example Artist";
    base.secret = 0;
    base.duration_seconds = 200;
    base.quality = lossless();
    base.tag_source = "vorbis";
    return base;
}

// 播放源类假插件（2.5 / 2.6 / 2.13）
class FakeSource : public ISourcePlugin {
public:
    explicit FakeSource(std::string platform) : platform_(std::move(platform)) {
        manifest_.id = platform_ + ".source";
        manifest_.name = platform_ + " 源";
        manifest_.type = PluginType::Source;
        manifest_.platforms = {platform_};
        manifest_.capabilities = {"search", "get_url", "get_lyric", "get_qualities"};
        QualityLevelSpec a;
        a.id = "lossless";
        a.label = "无损";
        a.spec = lossless();
        a.spec.quality_id = "lossless";
        QualityLevelSpec b;
        b.id = "exhigh";
        b.label = "极高";
        b.spec = high_mp3();
        b.spec.quality_id = "exhigh";
        manifest_.quality_levels = {a, b};
        manifest_.limits.max_concurrency = 2;
        manifest_.permissions = {"network"};
    }

    const PluginManifest& manifest() const override { return manifest_; }
    std::vector<std::string> platforms() const override { return manifest_.platforms; }
    bool supports(std::string_view) const override { return true; }
    std::vector<std::string> ops() const override { return manifest_.capabilities; }
    std::vector<QualityLevelSpec> quality_levels() const override { return manifest_.quality_levels; }

    Result<BaseInfo> resolve(std::string_view) override {
        return Result<BaseInfo>::failure(ErrorCode::RequestFailed, "not implemented");
    }

    Result<SearchItem> search(std::string_view keyword, const json::Value&) override {
        SearchItem item;
        if (keyword != "hit") return Result<SearchItem>::success(item);   // 0 条
        item.group_key = group_key("Hit Song", "Hit Artist", 210);
        item.title = "Hit Song";
        item.artists = {"Hit Artist"};
        item.duration_seconds = 210;
        SourceEntry entry;
        entry.platform = platform_;
        entry.item_id = platform_ + ":1";
        entry.available = true;
        entry.quality = manifest_.quality_levels.front().spec;
        entry.qualities = {manifest_.quality_levels[1].spec, manifest_.quality_levels[0].spec};
        entry.base.name = item.title;
        entry.base.author = "Hit Artist";
        item.sources = {entry};
        return Result<SearchItem>::success(item);
    }

    Result<std::string> open(std::string_view) override {
        return Result<std::string>::success("https://example.invalid/stream");
    }

    Result<json::Value> request(std::string_view op, const json::Value& params) override {
        if (op == "get_lyric") {
            json::Value v = json::Value::object();
            v.set("lrc", std::string("[00:01.00]hello\n[00:03.00]world\n"));
            return Result<json::Value>::success(v);
        }
        if (op == "get_url") {
            json::Value v = json::Value::object();
            v.set("url", std::string("https://example.invalid/a.flac"));
            v.set("quality", lossless().to_json());
            return Result<json::Value>::success(v);
        }
        if (op == "get_qualities") {
            json::Value arr = json::Value::array();
            arr.push_back(manifest_.quality_levels[0].to_json());
            arr.push_back(manifest_.quality_levels[1].to_json());
            return Result<json::Value>::success(arr);
        }
        return Result<json::Value>::failure(ErrorCode::UnsupportedCapability, "no such op");
    }

    Result<std::vector<Quality>> qualities(std::string_view) override {
        std::vector<Quality> out;
        for (const auto& spec : manifest_.quality_levels) out.push_back(spec.spec);
        return Result<std::vector<Quality>>::success(out);
    }

    json::Value last_params;

private:
    std::string platform_;
    PluginManifest manifest_;
};

// 其他类假插件（有界面 + audio_dsp 权限）
class FakeFunction : public IFunctionPlugin {
public:
    FakeFunction() {
        manifest_.id = "eq.advanced";
        manifest_.name = "高级均衡器";
        manifest_.type = PluginType::Function;
        manifest_.platforms.clear();
        manifest_.ui.panel = "ui/panel.html";
        manifest_.ui.settings = "ui/settings.html";
        manifest_.ui.menu = {"工具", "高级均衡器"};
        manifest_.permissions = {"audio_dsp"};
    }
    const PluginManifest& manifest() const override { return manifest_; }
    std::vector<std::string> ui_entries() const override { return {"panel", "settings"}; }
    Status on_enable(HostApi&) override { return Status::success(); }
    Status on_disable() override { return Status::success(); }

private:
    PluginManifest manifest_;
};

struct Fixture {
    std::filesystem::path root;
    std::unique_ptr<LibraryPaths> paths;
    std::unique_ptr<PluginRegistry> plugins;
    std::unique_ptr<CoreEngine> engine;
    std::unique_ptr<MetaCache> meta;
    std::unique_ptr<LibraryIndex> index;
    std::unique_ptr<ApiDispatcher> api;
    std::shared_ptr<FakeSource> radish;
    std::shared_ptr<FakeSource> netease;
    std::shared_ptr<FakeFunction> function;

    explicit Fixture(const std::string& name) {
        root = fresh_root(name);
        paths = std::make_unique<LibraryPaths>(root);
        paths->ensure_layout();
        plugins = std::make_unique<PluginRegistry>();
        engine = std::make_unique<CoreEngine>(*paths, *plugins);
        meta = std::make_unique<MetaCache>(*paths);
        index = std::make_unique<LibraryIndex>(*paths);
        api = std::make_unique<ApiDispatcher>(*paths, *plugins, *engine, *meta, *index);
        radish = std::make_shared<FakeSource>("radish");
        netease = std::make_shared<FakeSource>("netease");
        function = std::make_shared<FakeFunction>();
        plugins->add_source(radish);
        plugins->add_source(netease);
        plugins->add_function(function);
        api->set_source_priority({"radish", "netease"});
    }
};

}  // namespace

TEST_CASE(test_api_status_and_unknown_path) {
    Fixture f("status");
    const ApiResponse status = f.api->call("GET /music/status");
    CHECK(status.ok());
    CHECK_EQ(status.message, std::string("ok"));
    CHECK(status.data.get_string("state") == std::string("stopped"));

    const ApiResponse unknown = f.api->call("GET /music/nope");
    CHECK_FALSE(unknown.ok());
    CHECK(unknown.code == ErrorCode::RequestFailed);
    CHECK(unknown.to_json().get_int("code") == 1011);
}

TEST_CASE(test_api_load_play_and_info) {
    Fixture f("load");
    const RealTrack track = make_track(f.root, "api engine content");
    const BaseInfo base = local_base(track);
    CHECK(f.meta->put(base).ok());
    write_file(f.paths->lyric_file(track.sha), "[00:01.00]api line\n");

    json::Value body = json::Value::object();
    body.set("sha", base.sha);
    const ApiResponse loaded = f.api->call("POST /music/load", body);
    CHECK(loaded.ok());
    if (loaded.ok()) {
        CHECK_EQ(loaded.data.get("base").get_string("sha"), base.sha);
        CHECK_EQ(loaded.data.get_int("lyric_lines"), 1);
        CHECK_FALSE(loaded.data.get_bool("sha_warning"));
    }
    CHECK_EQ(f.index->size(), size_t{1});   // load 后写入库索引

    const ApiResponse started = f.api->call("POST /music/start");
    CHECK(started.ok());
    CHECK(started.data.get_string("state") == std::string("playing"));

    json::Value seek = json::Value::object();
    seek.set("seconds", 12.5);
    const ApiResponse seeked = f.api->call("POST /music/seek", seek);
    CHECK(seeked.ok());
    CHECK_EQ(seeked.data.get_int("position_ms"), 12500);

    CHECK(f.api->call("POST /music/pause").ok());
    CHECK(f.api->call("POST /music/resume").ok());
    CHECK(f.api->call("POST /music/stop").ok());

    const ApiResponse info = f.api->call("GET /music/info?sha=" + base.sha);
    CHECK(info.ok());
    CHECK_EQ(info.data.get("base").get_string("name"), std::string("Example Song"));
    CHECK_EQ(info.data.get_string("quality_key"), std::string("local.lossless-flac-1411"));

    // 缺少标识
    const ApiResponse missing = f.api->call("POST /music/load", json::Value::object());
    CHECK_FALSE(missing.ok());
    CHECK(missing.code == ErrorCode::InvalidMetadata);
}

TEST_CASE(test_api_verify_password) {
    Fixture f("verify");
    const auto plain = f.root / "p.bin";
    write_file(plain, "payload for api verify");

    Decryptor dec(*f.paths);
    SecInfo sec;
    CHECK(dec.encrypt_file(plain, f.root / "c.bin", "pw", 200, &sec).ok());
    bool ok = false;
    const std::string sha = crypto::sha256_file_hex((f.root / "c.bin").u8string(), &ok);
    CHECK(ok);
    CHECK(dec.save_sec(sha, sec).ok());

    json::Value good = json::Value::object();
    good.set("sha", sha);
    good.set("password", "pw");
    const ApiResponse ok_response = f.api->call("POST /music/verify", good);
    CHECK(ok_response.ok());
    CHECK(ok_response.data.get_bool("valid"));

    json::Value bad = good;
    bad.set("password", "nope");
    const ApiResponse bad_response = f.api->call("POST /music/verify", bad);
    CHECK_FALSE(bad_response.ok());
    CHECK(bad_response.code == ErrorCode::UsecError);
    CHECK(bad_response.to_json().get_int("code") == 1003);
}

TEST_CASE(test_api_search_multi_source_folding) {
    Fixture f("search");
    json::Value body = json::Value::object();
    body.set("keyword", "hit");
    const ApiResponse response = f.api->call("POST /music/search", body);
    CHECK(response.ok());
    if (response.ok()) {
        const json::Value& data = response.data;
        CHECK_EQ(data.get_int("total"), 1);                    // 两个源折叠成 1 条
        const json::Value& results = data.get("results");
        CHECK_EQ(results.size(), size_t{1});
        const json::Value& item = results[0];
        CHECK_EQ(item.get_string("best_source"), std::string("radish"));   // 优先级
        CHECK_EQ(item.get("sources").size(), size_t{2});
        // 曲目标识必须保留插件给的真实 id（不能被折叠键顶掉，否则 get_url/download 无法回源）
        for (const auto& source : item.get("sources").as_array()) {
            const std::string id = source.get_string("id");
            const std::string platform = source.get_string("platform");
            CHECK_EQ(id, platform + ":1");
            CHECK_EQ(source.get("qualities").size(), size_t{2});
        }
        const json::Value& status = data.get("sources_status");
        // 缺省 sources = 全部启用的在线源 + 宿主本地曲库（local 状态始终补一条，便于前端展示）
        CHECK_EQ(status.size(), size_t{3});
        bool has_local = false;
        for (const auto& entry : status.as_array()) {
            if (entry.get_string("platform") == "local") {
                has_local = true;
                CHECK_EQ(entry.get_int("code"), 0);
            }
        }
        CHECK(has_local);
    }

    // 空关键字 → 1007
    const ApiResponse empty = f.api->call("POST /music/search", json::Value::object());
    CHECK_FALSE(empty.ok());
    CHECK(empty.code == ErrorCode::InvalidMetadata);

    // 指定单源
    json::Value single = json::Value::object();
    single.set("keyword", "hit");
    json::Value sources = json::Value::array();
    sources.push_back("netease");
    single.set("sources", sources);
    const ApiResponse one = f.api->call("POST /music/search", single);
    CHECK(one.ok());
    if (one.ok()) CHECK_EQ(one.data.get_int("total"), 1);
}

TEST_CASE(test_api_request_capability_routing) {
    Fixture f("request");
    json::Value body = json::Value::object();
    body.set("source", "radish");
    body.set("op", "get_url");
    json::Value params = json::Value::object();
    params.set("id", "radish:1");
    params.set("quality", "lossless");
    body.set("params", params);
    const ApiResponse ok = f.api->call("POST /music/request", body);
    CHECK(ok.ok());
    if (ok.ok()) {
        CHECK_EQ(ok.data.get_string("source"), std::string("radish"));
        CHECK_EQ(ok.data.get_string("op"), std::string("get_url"));
    }

    // 未声明的 op → 1012
    json::Value bad_op = body;
    bad_op.set("op", "delete_everything");
    const ApiResponse undeclared = f.api->call("POST /music/request", bad_op);
    CHECK_FALSE(undeclared.ok());
    CHECK(undeclared.code == ErrorCode::UnsupportedCapability);

    // 未知平台 → 1001
    json::Value unknown = body;
    unknown.set("source", "unknown_platform");
    const ApiResponse platform = f.api->call("POST /music/request", unknown);
    CHECK_FALSE(platform.ok());
    CHECK(platform.code == ErrorCode::PlatformError);

    // 缺参数
    CHECK_FALSE(f.api->call("POST /music/request", json::Value::object()).ok());
}

TEST_CASE(test_api_lyric_local_and_unavailable) {
    Fixture f("lyric");
    const RealTrack track = make_track(f.root, "lyric content");
    const BaseInfo base = local_base(track);
    CHECK(f.meta->put(base).ok());
    write_file(f.paths->lyric_file(track.sha), "[00:01.00]one\n[00:05.00]two\n");

    const ApiResponse response = f.api->call("GET /music/lyric?sha=" + base.sha);
    CHECK(response.ok());
    if (response.ok()) {
        CHECK_EQ(response.data.get_string("origin"), std::string("local"));
        CHECK_EQ(response.data.get_int("line_count"), 2);
        CHECK(response.data.get_bool("timed"));
    }

    // 无歌词 → 1015 非致命（仍返回空结构）
    const RealTrack other = make_track(f.root, "no lyric content");
    BaseInfo other_base = local_base(other);
    other_base.name = "No Lyric Song";
    CHECK(f.meta->put(other_base).ok());
    const ApiResponse none = f.api->call("GET /music/lyric?sha=" + other_base.sha);
    CHECK(none.code == ErrorCode::LyricUnavailable);
    CHECK(is_non_fatal(ErrorCode::LyricUnavailable));
    CHECK_EQ(none.data.get_int("line_count"), 0);

    // 缓存中没有 → 1004
    const ApiResponse missing = f.api->call("GET /music/lyric?sha=" + std::string(64, 'f'));
    CHECK(missing.code == ErrorCode::FileNotFound);
}

TEST_CASE(test_api_quality_levels_and_of) {
    Fixture f("quality");
    const ApiResponse levels = f.api->call("GET /music/quality");
    CHECK(levels.ok());
    if (levels.ok()) {
        const json::Value& sources = levels.data.get("sources");
        CHECK_EQ(sources.size(), size_t{2});
        const json::Value& first = sources[0];
        CHECK_EQ(first.get_string("platform"), std::string("radish"));
        CHECK_EQ(first.get("levels").size(), size_t{2});
        CHECK_EQ(first.get("levels")[0].get_string("id"), std::string("lossless"));
        CHECK_EQ(first.get("levels")[0].get_string("level"), std::string("lossless"));
    }

    const ApiResponse of = f.api->call("GET /music/quality/radish/radish:1");
    CHECK(of.ok());
    if (of.ok()) {
        CHECK_EQ(of.data.get_string("platform"), std::string("radish"));
        CHECK_EQ(of.data.get("qualities").size(), size_t{2});
    }
}

TEST_CASE(test_api_eq_and_dsp) {
    Fixture f("eq");
    const ApiResponse initial = f.api->call("GET /music/eq");
    CHECK(initial.ok());
    CHECK_FALSE(initial.data.get_bool("enabled"));

    json::Value body = json::Value::object();
    body.set("enabled", true);
    body.set("preset", "my_curve");
    body.set("preamp_db", -2.0);
    json::Value bands = json::Value::array();
    json::Value band = json::Value::object();
    band.set("type", "peak");
    band.set("freq", 60.0);
    band.set("gain", 3.0);
    band.set("q", 1.0);
    bands.push_back(band);
    body.set("bands", bands);
    const ApiResponse applied = f.api->call("POST /music/eq", body);
    CHECK(applied.ok());
    if (applied.ok()) CHECK(applied.data.get_bool("applied"));

    // 参数非法 → 1007
    json::Value bad = body;
    json::Value bad_bands = json::Value::array();
    json::Value bad_band = json::Value::object();
    bad_band.set("freq", -5.0);
    bad_band.set("gain", 0.0);
    bad_bands.push_back(bad_band);
    bad.set("bands", bad_bands);
    const ApiResponse invalid = f.api->call("POST /music/eq", bad);
    CHECK_FALSE(invalid.ok());
    CHECK(invalid.code == ErrorCode::InvalidMetadata);

    // 预设已持久化
    CHECK(std::filesystem::exists(f.paths->presets() / "eq" / "my_curve.json"));
    const ApiResponse presets = f.api->call("GET /music/eq/presets");
    CHECK(presets.ok());
    if (presets.ok()) CHECK_EQ(presets.data.get("presets").size(), size_t{1});

    // DSP 节点：只列声明了 audio_dsp 的其他类插件
    const ApiResponse nodes = f.api->call("GET /music/dsp/nodes");
    CHECK(nodes.ok());
    if (nodes.ok()) {
        CHECK_EQ(nodes.data.get("nodes").size(), size_t{1});
        CHECK_EQ(nodes.data.get("nodes")[0].get_string("plugin_id"), std::string("eq.advanced"));
    }
    json::Value bypass = json::Value::object();
    bypass.set("node", "eq.advanced");
    CHECK(f.api->call("POST /music/dsp/bypass", bypass).ok());
    json::Value unknown = json::Value::object();
    unknown.set("node", "nope");
    const ApiResponse bad_node = f.api->call("POST /music/dsp/bypass", unknown);
    CHECK(bad_node.code == ErrorCode::UnsupportedCapability);
}

TEST_CASE(test_api_unwired_modules_return_1011) {
    Fixture f("unwired");
    CHECK(f.api->call("GET /music/cache").code == ErrorCode::RequestFailed);
    CHECK(f.api->call("GET /music/download").code == ErrorCode::RequestFailed);
    CHECK(f.api->call("POST /music/import/scan", json::Value::object()).code == ErrorCode::RequestFailed);
}

TEST_CASE(test_api_plugin_manifest_validation) {
    PluginManifest source_manifest;
    source_manifest.id = "x.source";
    source_manifest.name = "X";
    source_manifest.type = PluginType::Source;
    source_manifest.platforms = {"x"};
    // 播放源类不得有界面
    source_manifest.ui.panel = "ui/panel.html";
    CHECK_FALSE(source_manifest.validate().ok());

    PluginManifest function_manifest;
    function_manifest.id = "x.func";
    function_manifest.name = "X Func";
    function_manifest.type = PluginType::Function;
    function_manifest.ui.panel = "ui/panel.html";
    function_manifest.platforms = {"x"};      // 其他类不得声明 platform
    CHECK_FALSE(function_manifest.validate().ok());

    function_manifest.platforms.clear();
    CHECK(function_manifest.validate().ok());

    // 音质 id 重复 → 非法
    PluginManifest dup;
    dup.id = "y.source";
    dup.name = "Y";
    dup.type = PluginType::Source;
    dup.platforms = {"y"};
    QualityLevelSpec a;
    a.id = "lossless";
    a.spec = lossless();
    dup.quality_levels = {a, a};
    CHECK_FALSE(dup.validate().ok());

    // api_version 不兼容 → 1008
    PluginManifest old = function_manifest;
    old.api_version = "0";
    const Status s = old.validate("1");
    CHECK_FALSE(s.ok());
    CHECK(s.code == ErrorCode::UnsupportedVersion);
}
