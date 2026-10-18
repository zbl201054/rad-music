// 自定义合并测试（merge.hpp）：把识别错误拆开的多条曲目归一到同一首歌
#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "radmusic/api.hpp"
#include "radmusic/engine.hpp"
#include "radmusic/groupkey.hpp"
#include "radmusic/merge.hpp"
#include "radmusic/model.hpp"
#include "radmusic/plugin.hpp"
#include "radmusic/quality.hpp"
#include "radmusic/storage.hpp"
#include "test_framework.hpp"

using namespace radmusic;

namespace {

std::filesystem::path fresh_root(const std::string& name) {
    const auto root = std::filesystem::path("build") / "tmp_lib" / name;
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    return root;
}

std::string fake_sha(char seed) {
    std::string s(64, '0');
    for (size_t i = 0; i < 64; ++i) s[i] = static_cast<char>('a' + ((seed + static_cast<char>(i)) % 6));
    return s;
}

Quality make_quality(QualityLevel level, const std::string& codec, int bitrate) {
    Quality q;
    q.level = level;
    q.codec = codec;
    q.bitrate = bitrate;
    q.lossless = level == QualityLevel::Lossless || level == QualityLevel::HiRes;
    q.channels = 2;
    if (q.lossless) {
        q.sample_rate = 44100;
        q.bit_depth = 16;
    }
    return q;
}

BaseInfo mk_track(const std::string& title, const std::string& artist, int duration, char seed,
                  QualityLevel level, const std::string& codec, int bitrate) {
    BaseInfo b;
    b.platform = "local";
    b.version = "1.0.0";
    b.sha = fake_sha(seed);
    b.location = "tracks/" + b.sha + "/audio." + codec;
    b.name = title;
    b.author = artist;
    b.duration_seconds = duration;
    b.quality = make_quality(level, codec, bitrate);
    b.tag_source = "vorbis";
    return b;
}

struct Fixture {
    std::unique_ptr<LibraryPaths> paths;
    std::unique_ptr<PluginRegistry> plugins;
    std::unique_ptr<CoreEngine> engine;
    std::unique_ptr<MetaCache> meta;
    std::unique_ptr<LibraryIndex> index;
    std::unique_ptr<MergeStore> merges;
    std::unique_ptr<ApiDispatcher> api;

    explicit Fixture(const std::string& name) {
        paths = std::make_unique<LibraryPaths>(fresh_root(name));
        paths->ensure_layout();
        plugins = std::make_unique<PluginRegistry>();
        engine = std::make_unique<CoreEngine>(*paths, *plugins);
        meta = std::make_unique<MetaCache>(*paths);
        index = std::make_unique<LibraryIndex>(*paths);
        merges = std::make_unique<MergeStore>(*paths);
        CHECK(merges->load().ok());
        meta->set_merge_store(merges.get());
        api = std::make_unique<ApiDispatcher>(*paths, *plugins, *engine, *meta, *index);
        api->set_merge(merges.get());
    }
};

std::vector<std::pair<MetaPath, BaseInfo>> entries_of(MetaCache& cache) {
    return cache.list_all_with_paths();
}

}  // namespace

TEST_CASE(test_merge_store_roundtrip_and_normalize) {
    const auto root = fresh_root("merge_store");
    LibraryPaths paths(root);
    CHECK(paths.ensure_layout().ok());

    MergeStore store(paths);
    CHECK(store.load().ok());
    CHECK(store.empty());

    const std::string canon = song_key("晴天", "周杰伦", 269);
    const std::string key_live = song_key("晴天(Live)", "周杰伦", 271);
    const std::string key_jay = song_key("晴天", "Jay Chou", 269);
    CHECK(key_live != canon);
    CHECK(key_jay != canon);

    MergeGroup group;
    group.song_key = canon;
    group.title = "晴天";
    group.artist = "周杰伦";
    group.duration_seconds = 269;
    MergeMember a;
    a.song_key = key_live;
    a.title = "晴天(Live)";
    a.artist = "周杰伦";
    a.duration_seconds = 271;
    a.shas.push_back(fake_sha(1));
    MergeMember b;
    b.song_key = key_jay;
    b.title = "晴天";
    b.artist = "Jay Chou";
    b.duration_seconds = 269;
    b.shas.push_back(fake_sha(2));
    group.members = {a, b};
    CHECK(store.add_or_update(group).ok());

    // 重新加载：持久化生效
    MergeStore reloaded(paths);
    CHECK(reloaded.load().ok());
    CHECK_EQ(reloaded.groups().size(), size_t{1});
    const MergeGroup* found = reloaded.find(key_live);
    CHECK(found != nullptr);
    if (found) {
        CHECK_EQ(found->song_key, canon);
        CHECK_EQ(found->members.size(), size_t{2});
    }
    auto canonical = reloaded.canonical_song_key(key_jay);
    CHECK(canonical.has_value());
    if (canonical) CHECK_EQ(*canonical, canon);
    CHECK(!reloaded.canonical_song_key(canon).has_value());
    CHECK(!reloaded.canonical_song_key("unrelated_key").has_value());

    // 规范化：成员身份改写为规范值，原始值进 orig_*
    BaseInfo base = mk_track("晴天(Live)", "周杰伦", 271, 1, QualityLevel::Lossless, "flac", 1411);
    CHECK(reloaded.normalize(base));
    CHECK_EQ(base.name, std::string("晴天"));
    CHECK_EQ(base.author, std::string("周杰伦"));
    CHECK_EQ(base.duration_seconds, 269);
    CHECK_EQ(base.orig_name, std::string("晴天(Live)"));
    CHECK_EQ(base.orig_duration, 271);
    // 幂等：第二次不再改写，也不覆盖 orig_*
    CHECK_FALSE(reloaded.normalize(base));

    // 还原：orig_* 回写
    MergeStore::restore(base, "晴天", "周杰伦", 269);
    CHECK_EQ(base.name, std::string("晴天(Live)"));
    CHECK_EQ(base.duration_seconds, 271);
    CHECK(base.orig_name.empty());

    // 整组撤销（按成员 key 也能命中）
    CHECK(reloaded.remove(key_live));
    CHECK(reloaded.empty());
}

TEST_CASE(test_library_merge_two_tracks) {
    Fixture f("merge_lib");
    // 同一首歌，因识别误差成了两条：标题多了 (Live)、时长差 2 秒
    const BaseInfo a = mk_track("晴天", "周杰伦", 269, 1, QualityLevel::Lossless, "flac", 1411);
    const BaseInfo b = mk_track("晴天 (Live)", "周杰伦", 271, 2, QualityLevel::High, "mp3", 320);
    CHECK(f.meta->put(a).ok());
    CHECK(f.meta->put(b).ok());
    const std::string key_a = song_key(a.name, a.author, a.duration_seconds);
    const std::string key_b = song_key(b.name, b.author, b.duration_seconds);
    CHECK(key_a != key_b);
    CHECK_EQ(f.meta->count(), size_t{2});

    json::Value body = json::Value::object();
    body.set("target", key_a);
    json::Value sources = json::Value::array();
    sources.push_back(key_b);
    body.set("sources", sources);
    const ApiResponse merged = f.api->call("POST /music/library/merge", body);
    CHECK(merged.ok());
    if (!merged.ok()) CHECK_EQ(merged.message, std::string("merge failed"));

    const std::string canon_key = merged.data.get_string("song_key");
    CHECK_EQ(canon_key, key_a);   // 以 target 的身份为准
    CHECK_EQ(merged.data.get_int("moved"), 2);

    // 两条现在都在同一个 song_key 下（曲库里表现为音质堆叠）
    const auto entries = entries_of(*f.meta);
    CHECK_EQ(entries.size(), size_t{2});
    for (const auto& entry : entries) CHECK_EQ(entry.first.song_key, canon_key);
    CHECK(entries[0].first.quality_key != entries[1].first.quality_key);

    // 被合并进来的那条保留了原始标签
    const BaseInfo* live = nullptr;
    for (const auto& entry : entries) {
        if (entry.second.sha == b.sha) live = &entry.second;
    }
    CHECK(live != nullptr);
    if (live) {
        CHECK_EQ(live->name, std::string("晴天"));
        CHECK_EQ(live->orig_name, std::string("晴天 (Live)"));
        CHECK_EQ(live->orig_duration, 271);
    }

    // 列表接口标出合并信息
    const ApiResponse list = f.api->call("GET /music/library");
    CHECK(list.ok());
    const auto& tracks = list.data.get("tracks").as_array();
    CHECK_EQ(tracks.size(), size_t{2});
    CHECK(tracks[0].get_bool("merged"));
    CHECK_EQ(tracks[0].get_int("merge_count"), 2);

    // 合并记录可查询
    const ApiResponse groups = f.api->call("GET /music/library/merges");
    CHECK(groups.ok());
    CHECK_EQ(groups.data.get_int("total"), 1);
}

TEST_CASE(test_merge_redirects_new_imports) {
    Fixture f("merge_redirect");
    // 歌手写法不同（周杰伦 / Jay Chou）导致拆成两条
    const BaseInfo a = mk_track("稻香", "周杰伦", 223, 1, QualityLevel::Lossless, "flac", 1411);
    const BaseInfo b = mk_track("稻香", "Jay Chou", 223, 2, QualityLevel::High, "mp3", 320);
    CHECK(f.meta->put(a).ok());
    CHECK(f.meta->put(b).ok());
    const std::string key_a = song_key("稻香", "周杰伦", 223);
    const std::string key_b = song_key("稻香", "Jay Chou", 223);

    json::Value body = json::Value::object();
    body.set("target", key_a);
    json::Value sources = json::Value::array();
    sources.push_back(key_b);
    body.set("sources", sources);
    CHECK(f.api->call("POST /music/library/merge", body).ok());

    // 之后再导入「和 b 写法一样」的第三首：自动落进同一个 song_key
    const BaseInfo c = mk_track("稻香", "Jay Chou", 223, 3, QualityLevel::Medium, "mp3", 128);
    const auto put = f.meta->put(c);
    CHECK(put.ok());
    if (put.ok()) CHECK_EQ(put.value().path.song_key, key_a);

    const auto entries = entries_of(*f.meta);
    CHECK_EQ(entries.size(), size_t{3});
    for (const auto& entry : entries) CHECK_EQ(entry.first.song_key, key_a);
}

TEST_CASE(test_library_unmerge_restores_originals) {
    Fixture f("merge_unmerge");
    const BaseInfo a = mk_track("七里香", "周杰伦", 298, 1, QualityLevel::Lossless, "flac", 1411);
    const BaseInfo b = mk_track("七里香（Remaster）", "周杰伦", 300, 2, QualityLevel::High, "mp3", 320);
    CHECK(f.meta->put(a).ok());
    CHECK(f.meta->put(b).ok());
    const std::string key_a = song_key(a.name, a.author, a.duration_seconds);
    const std::string key_b = song_key(b.name, b.author, b.duration_seconds);

    json::Value merge_body = json::Value::object();
    merge_body.set("target", key_a);
    json::Value sources = json::Value::array();
    sources.push_back(key_b);
    merge_body.set("sources", sources);
    CHECK(f.api->call("POST /music/library/merge", merge_body).ok());

    json::Value body = json::Value::object();
    body.set("song_key", key_a);
    const ApiResponse unmerged = f.api->call("POST /music/library/unmerge", body);
    CHECK(unmerged.ok());
    if (!unmerged.ok()) CHECK_EQ(unmerged.message, std::string("unmerge failed"));
    CHECK_EQ(unmerged.data.get_int("restored"), 2);
    CHECK(unmerged.data.get_bool("group_removed"));

    const auto entries = entries_of(*f.meta);
    CHECK_EQ(entries.size(), size_t{2});
    std::vector<std::string> keys;
    for (const auto& entry : entries) keys.push_back(entry.first.song_key);
    CHECK(keys[0] != keys[1]);
    // 恢复成合并前的两个 song_key
    CHECK(std::find(keys.begin(), keys.end(), key_a) != keys.end());
    CHECK(std::find(keys.begin(), keys.end(), key_b) != keys.end());

    const ApiResponse groups = f.api->call("GET /music/library/merges");
    CHECK_EQ(groups.data.get_int("total"), 0);
}

TEST_CASE(test_merge_same_quality_becomes_variant) {
    Fixture f("merge_variant");
    // 两条同名同歌手但内容不同、音质也相同（常见于两个不同在线 id）
    const BaseInfo a = mk_track("夜曲", "周杰伦", 255, 1, QualityLevel::Lossless, "flac", 1411);
    const BaseInfo b = mk_track("夜曲", "Jay Chou", 255, 2, QualityLevel::Lossless, "flac", 1411);
    CHECK(f.meta->put(a).ok());
    CHECK(f.meta->put(b).ok());
    const std::string key_a = song_key(a.name, a.author, 255);
    const std::string key_b = song_key(b.name, b.author, 255);

    json::Value body = json::Value::object();
    body.set("target", key_a);
    json::Value sources = json::Value::array();
    sources.push_back(key_b);
    body.set("sources", sources);
    const ApiResponse merged = f.api->call("POST /music/library/merge", body);
    CHECK(merged.ok());
    CHECK_EQ(merged.data.get_int("moved"), 2);
    CHECK_EQ(merged.data.get("conflicts").as_array().size(), size_t{1});

    const auto entries = entries_of(*f.meta);
    CHECK_EQ(entries.size(), size_t{2});   // 两条都还在（变体目录），不丢数据
    CHECK(entries[0].first.quality_key != entries[1].first.quality_key);
    CHECK(entries[1].first.quality_key.find("~2") != std::string::npos);
}

TEST_CASE(test_merge_rejects_bad_requests) {
    Fixture f("merge_bad");
    const BaseInfo a = mk_track("简单爱", "周杰伦", 265, 1, QualityLevel::Lossless, "flac", 1411);
    CHECK(f.meta->put(a).ok());
    const std::string key_a = song_key(a.name, a.author, a.duration_seconds);

    // 只给一条：没有合并对象
    json::Value single = json::Value::object();
    single.set("target", key_a);
    const ApiResponse one = f.api->call("POST /music/library/merge", single);
    CHECK_FALSE(one.ok());

    // 不存在的 song_key
    json::Value missing = json::Value::object();
    missing.set("target", key_a);
    json::Value sources = json::Value::array();
    sources.push_back(std::string("no_such_song_key_000000"));
    missing.set("sources", sources);
    CHECK_FALSE(f.api->call("POST /music/library/merge", missing).ok());

    // 未合并过的曲目取消合并
    json::Value plain = json::Value::object();
    plain.set("song_key", key_a);
    CHECK_FALSE(f.api->call("POST /music/library/unmerge", plain).ok());
}
