// 本地曲目联网识别（1.10 扩展）测试
//
// 说明：不依赖真实网络与音频文件。用 FakeSource 提供假搜索结果，
// 在 build/tmp_identify/<case>/ 下自建临时曲库根，验证：
//  * 命中后回写 item_id / match_platform / album，并可补封面路径（不触发真实下载）；
//  * 歌名 / 歌手 / 时长任一不满足都判为不匹配；
//  * 已关联过的条目默认不重复识别；force 才重查；
//  * 未知 sha 与非本地曲目返回 failed。
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "radmusic/crypto.hpp"
#include "radmusic/groupkey.hpp"
#include "radmusic/identify.hpp"
#include "radmusic/quality.hpp"
#include "radmusic/storage.hpp"
#include "test_framework.hpp"

using namespace radmusic;
namespace fs = std::filesystem;

namespace {

// 每个用例独立的临时目录，结束即清理
struct Temp {
    fs::path root;
    explicit Temp(const std::string& name) {
        const fs::path base = fs::path("build") / "tmp_identify" / name;
        std::error_code ec;
        fs::remove_all(base, ec);
        root = base / "lib";
        fs::create_directories(root, ec);
    }
    ~Temp() {
        std::error_code ec;
        fs::remove_all(root.parent_path(), ec);
    }
    LibraryPaths paths() const { return LibraryPaths(root); }
};

// 假播放源：返回预设的搜索结果
class FakeSource : public ISourcePlugin {
public:
    FakeSource(std::string platform, std::vector<SearchItem> items)
        : platform_(std::move(platform)), items_(std::move(items)) {
        manifest_.id = "fake_" + platform_;
        manifest_.name = "Fake " + platform_;
        manifest_.type = PluginType::Source;
        manifest_.platforms = {platform_};
        manifest_.capabilities = {"search", "get_url", "get_qualities"};
        QualityLevelSpec level;
        level.id = "lossless";
        level.label = "无损";
        level.spec.level = QualityLevel::Lossless;
        level.spec.codec = "flac";
        level.spec.lossless = true;
        level.spec.bitrate = 999;
        level.spec.quality_id = "lossless";
        manifest_.quality_levels = {level};
    }

    const PluginManifest& manifest() const override { return manifest_; }
    std::vector<std::string> platforms() const override { return {platform_}; }
    bool supports(std::string_view) const override { return true; }
    std::vector<std::string> ops() const override { return manifest_.capabilities; }
    std::vector<QualityLevelSpec> quality_levels() const override {
        return manifest_.quality_levels;
    }
    Result<BaseInfo> resolve(std::string_view) override {
        return Result<BaseInfo>::failure(ErrorCode::UnsupportedCapability, "fake");
    }
    Result<SearchItem> search(std::string_view, const json::Value&) override {
        if (items_.empty()) return Result<SearchItem>::failure(ErrorCode::PlatformError, "empty");
        return Result<SearchItem>::success(items_.front());
    }
    Result<std::vector<SearchItem>> search_all(std::string_view, const json::Value&) override {
        ++calls;
        return Result<std::vector<SearchItem>>::success(items_);
    }
    Result<std::string> open(std::string_view) override {
        return Result<std::string>::failure(ErrorCode::UnsupportedCapability, "fake");
    }
    Result<json::Value> request(std::string_view, const json::Value&) override {
        return Result<json::Value>::failure(ErrorCode::UnsupportedCapability, "fake");
    }
    Result<std::vector<Quality>> qualities(std::string_view) override {
        return Result<std::vector<Quality>>::failure(ErrorCode::UnsupportedCapability, "fake");
    }

    int calls = 0;

private:
    std::string platform_;
    PluginManifest manifest_;
    std::vector<SearchItem> items_;
};

Quality lossless_quality() {
    Quality q;
    q.level = QualityLevel::Lossless;
    q.codec = "flac";
    q.lossless = true;
    q.bitrate = 999;
    q.sample_rate = 44100;
    q.quality_id = "lossless";
    q.label = "无损";
    return q;
}

// 构造一条在线候选（SearchItem，sources[0] 带完整 base）
SearchItem online_item(const std::string& title, const std::vector<std::string>& artists,
                       int duration, const std::string& item_id, const std::string& album = "",
                       const std::string& pic = "") {
    SearchItem item;
    item.title = title;
    item.artists = artists;
    item.album = album;
    item.duration_seconds = duration;
    item.group_key = group_key(title, artists.empty() ? std::string() : artists.front(), duration);
    SourceEntry entry;
    entry.platform = "fake";
    entry.item_id = item_id;
    entry.available = true;
    entry.quality = lossless_quality();
    entry.qualities = {lossless_quality()};
    entry.base.platform = "fake";
    entry.base.item_id = item_id;
    entry.base.name = title;
    entry.base.author = artists.empty() ? std::string() : artists.front();
    entry.base.album = album;
    entry.base.pic = pic;
    entry.base.duration_seconds = duration;
    entry.base.quality = lossless_quality();
    item.sources.push_back(std::move(entry));
    return item;
}

// 造一个本地曲目并写入临时曲库；返回 sha
std::string put_local(MetaCache& meta, const std::string& name, const std::string& author,
                      int duration, const std::string& album = "") {
    BaseInfo base;
    base.platform = "local";
    base.version = "1.0.0";
    base.location = "tracks/" + name + "/audio.flac";   // 每个曲目独立 location，避免互相冲突
    base.sha = crypto::sha256_hex(name + "|" + author + "|" + std::to_string(duration));
    base.name = name;
    base.author = author;
    base.album = album;
    base.duration_seconds = duration;
    base.quality = lossless_quality();
    base.secret = 0;
    auto put = meta.put(base, ConflictPolicy::Skip);
    if (!put.ok()) return std::string();
    return put.value().base.sha;
}

std::string action_of(const json::Value& result, size_t index) {
    const json::Value& results = result.get("results");
    if (!results.is_array() || index >= results.as_array().size()) return std::string();
    return results.as_array()[index].get_string("action");
}

json::Value identify_json(const std::string& sha, bool fetch_pic = false) {
    json::Value body = json::Value::object();
    body.set("sha", sha);
    body.set("fetch_pic", fetch_pic);
    return body;
}

}  // namespace

// ---------------------------------------------------------------- 用例

TEST_CASE(test_identify_exact_match_links_online_version) {
    Temp temp("exact");
    LibraryPaths paths = temp.paths();
    MetaCache meta(paths);
    LibraryIndex index(paths);
    const std::string sha = put_local(meta, "Roar", "Katy Perry", 224);
    CHECK(!sha.empty());

    auto fake = std::make_shared<FakeSource>(
        "fake", std::vector<SearchItem>{online_item("Roar", {"Katy Perry"}, 224, "9001", "Roar")});
    PluginRegistry plugins;
    plugins.add_source(fake);

    IdentifyService service(paths, meta, index, plugins);
    auto result = service.identify(identify_json(sha));
    CHECK(result.ok());
    CHECK_EQ(action_of(result.value(), 0), std::string("linked"));

    // base.json 已回写
    auto path = meta.find_by_sha(sha);
    CHECK(path.has_value());
    auto base = meta.read(*path);
    CHECK(base.ok());
    CHECK_EQ(base.value().item_id, std::string("9001"));
    CHECK_EQ(base.value().match_platform, std::string("fake"));
    CHECK_EQ(base.value().album, std::string("Roar"));
    CHECK(fake->calls == 1);
}

TEST_CASE(test_identify_title_mismatch_is_no_match) {
    Temp temp("title");
    LibraryPaths paths = temp.paths();
    MetaCache meta(paths);
    LibraryIndex index(paths);
    const std::string sha = put_local(meta, "Roar", "Katy Perry", 224);

    auto fake = std::make_shared<FakeSource>(
        "fake", std::vector<SearchItem>{online_item("Dark Horse", {"Katy Perry"}, 224, "9002")});
    PluginRegistry plugins;
    plugins.add_source(fake);

    IdentifyService service(paths, meta, index, plugins);
    auto result = service.identify(identify_json(sha));
    CHECK(result.ok());
    CHECK_EQ(action_of(result.value(), 0), std::string("no_match"));

    auto path = meta.find_by_sha(sha);
    CHECK(path.has_value());
    auto base = meta.read(*path);
    CHECK(base.ok());
    CHECK(base.value().item_id.empty());
    CHECK(base.value().match_platform.empty());
}

TEST_CASE(test_identify_artist_mismatch_is_no_match) {
    Temp temp("artist");
    LibraryPaths paths = temp.paths();
    MetaCache meta(paths);
    LibraryIndex index(paths);
    const std::string sha = put_local(meta, "Roar", "Katy Perry", 224);

    auto fake = std::make_shared<FakeSource>(
        "fake", std::vector<SearchItem>{online_item("Roar", {"Someone Else"}, 224, "9003")});
    PluginRegistry plugins;
    plugins.add_source(fake);

    IdentifyService service(paths, meta, index, plugins);
    auto result = service.identify(identify_json(sha));
    CHECK(result.ok());
    CHECK_EQ(action_of(result.value(), 0), std::string("no_match"));
}

TEST_CASE(test_identify_duration_tolerance) {
    Temp temp("duration");
    LibraryPaths paths = temp.paths();
    MetaCache meta(paths);
    LibraryIndex index(paths);
    const std::string near = put_local(meta, "Near", "Artist", 220);
    const std::string far = put_local(meta, "Far", "Artist", 220);

    auto fake = std::make_shared<FakeSource>(
        "fake", std::vector<SearchItem>{online_item("Near", {"Artist"}, 224, "9101"),
                                        online_item("Far", {"Artist"}, 260, "9102")});
    PluginRegistry plugins;
    plugins.add_source(fake);

    IdentifyService service(paths, meta, index, plugins);
    // ±4 秒在容差内 → 命中
    auto near_result = service.identify(identify_json(near));
    CHECK(near_result.ok());
    CHECK_EQ(action_of(near_result.value(), 0), std::string("linked"));
    // 40 秒差 → 拒绝
    auto far_result = service.identify(identify_json(far));
    CHECK(far_result.ok());
    CHECK_EQ(action_of(far_result.value(), 0), std::string("no_match"));
}

TEST_CASE(test_identify_multi_artist_overlap) {
    Temp temp("multi");
    LibraryPaths paths = temp.paths();
    MetaCache meta(paths);
    LibraryIndex index(paths);
    const std::string sha = put_local(meta, "合唱", "甲 / 乙 / 丙", 200);

    auto fake = std::make_shared<FakeSource>(
        "fake", std::vector<SearchItem>{online_item("合唱", {"乙", "丁"}, 200, "9201")});
    PluginRegistry plugins;
    plugins.add_source(fake);

    IdentifyService service(paths, meta, index, plugins);
    auto result = service.identify(identify_json(sha));
    CHECK(result.ok());
    CHECK_EQ(action_of(result.value(), 0), std::string("linked"));
}

TEST_CASE(test_identify_already_linked_skips_network) {
    Temp temp("already");
    LibraryPaths paths = temp.paths();
    MetaCache meta(paths);
    LibraryIndex index(paths);
    const std::string sha = put_local(meta, "Roar", "Katy Perry", 224);
    CHECK(!sha.empty());

    // 先手动关联一次
    auto path = meta.find_by_sha(sha);
    CHECK(path.has_value());
    auto base = meta.read(*path);
    CHECK(base.ok());
    BaseInfo linked = base.value();
    linked.item_id = "777";
    linked.match_platform = "fake";
    auto put = meta.put(linked, ConflictPolicy::Overwrite);
    CHECK(put.ok());

    auto fake = std::make_shared<FakeSource>(
        "fake", std::vector<SearchItem>{online_item("Roar", {"Katy Perry"}, 224, "9001")});
    PluginRegistry plugins;
    plugins.add_source(fake);

    IdentifyService service(paths, meta, index, plugins);
    auto result = service.identify(identify_json(sha));
    CHECK(result.ok());
    CHECK_EQ(action_of(result.value(), 0), std::string("already"));
    CHECK(fake->calls == 0);   // 不再联网

    // force = true 才重新识别
    json::Value body = identify_json(sha);
    body.set("force", true);
    auto forced = service.identify(body);
    CHECK(forced.ok());
    CHECK_EQ(action_of(forced.value(), 0), std::string("linked"));
    CHECK(fake->calls == 1);
    auto after = meta.read(*meta.find_by_sha(sha));
    CHECK(after.ok());
    CHECK_EQ(after.value().item_id, std::string("9001"));
}

TEST_CASE(test_identify_unknown_sha_and_online_track_fail) {
    Temp temp("fail");
    LibraryPaths paths = temp.paths();
    MetaCache meta(paths);
    LibraryIndex index(paths);
    PluginRegistry plugins;   // 没有任何音源

    // 未知 sha
    {
        IdentifyService service(paths, meta, index, plugins);
        auto result = service.identify(identify_json(std::string(64, 'a')));
        CHECK(result.ok());
        CHECK_EQ(action_of(result.value(), 0), std::string("failed"));
    }
    // 在线曲目不支持识别
    {
        BaseInfo base;
        base.platform = "fake";
        base.version = "1.0.0";
        base.location = "http://plugin.invalid/fake/1";
        base.sha = crypto::sha256_hex("online-1");
        base.name = "Online";
        base.author = "Someone";
        base.item_id = "1";
        base.duration_seconds = 100;
        auto put = meta.put(base, ConflictPolicy::Skip);
        CHECK(put.ok());
        IdentifyService service(paths, meta, index, plugins);
        auto result = service.identify(identify_json(put.value().base.sha));
        CHECK(result.ok());
        CHECK_EQ(action_of(result.value(), 0), std::string("failed"));
    }
}

TEST_CASE(test_identify_batch_and_source_filter) {
    Temp temp("batch");
    LibraryPaths paths = temp.paths();
    MetaCache meta(paths);
    LibraryIndex index(paths);
    const std::string first = put_local(meta, "One", "AA", 100);
    const std::string second = put_local(meta, "Two", "BB", 200);
    CHECK(!first.empty());
    CHECK(!second.empty());

    auto fake = std::make_shared<FakeSource>(
        "fake", std::vector<SearchItem>{online_item("One", {"AA"}, 100, "5001")});
    auto other = std::make_shared<FakeSource>(
        "other", std::vector<SearchItem>{online_item("Two", {"BB"}, 200, "5002")});
    PluginRegistry plugins;
    plugins.add_source(fake);
    plugins.add_source(other);

    IdentifyService service(paths, meta, index, plugins);
    json::Value body = json::Value::object();
    json::Value shas = json::Value::array();
    shas.push_back(first);
    shas.push_back(second);
    body.set("shas", shas);
    auto result = service.identify(body);
    CHECK(result.ok());
    CHECK_EQ(result.value().get_int("total", 0), static_cast<long long>(2));
    CHECK_EQ(result.value().get_int("matched", 0), static_cast<long long>(2));
    CHECK_EQ(action_of(result.value(), 0), std::string("linked"));
    CHECK_EQ(action_of(result.value(), 1), std::string("linked"));

    // 限定 source=fake：other 的曲目不再被识别
    Temp temp2("batch2");
    LibraryPaths paths2 = temp2.paths();
    MetaCache meta2(paths2);
    LibraryIndex index2(paths2);
    const std::string only = put_local(meta2, "Two", "BB", 200);
    IdentifyService service2(paths2, meta2, index2, plugins);
    json::Value filtered = identify_json(only);
    filtered.set("source", std::string("fake"));
    auto filtered_result = service2.identify(filtered);
    CHECK(filtered_result.ok());
    CHECK_EQ(action_of(filtered_result.value(), 0), std::string("no_match"));
}

TEST_CASE(test_identify_missing_request_is_error) {
    Temp temp("empty");
    LibraryPaths paths = temp.paths();
    MetaCache meta(paths);
    LibraryIndex index(paths);
    PluginRegistry plugins;
    IdentifyService service(paths, meta, index, plugins);
    auto result = service.identify(json::Value::object());
    CHECK(!result.ok());
    CHECK_EQ(static_cast<int>(result.status().code), static_cast<int>(ErrorCode::InvalidMetadata));
}
