// 缓存管理测试（Readme.md 1.9 / 2.11）
//
// 覆盖：各层统计、按层清理、meta 层清理与索引重建、pending eviction（in_use 跳过）、
//      touch_audio（LRU 时间戳）、按 mtime 最旧优先淘汰、配置持久化、清缓存不碰曲库文件。
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "radmusic/cache.hpp"
#include "radmusic/quality.hpp"
#include "radmusic/storage.hpp"
#include "test_framework.hpp"

using namespace radmusic;

namespace {

std::filesystem::path fresh_root(const std::string& name) {
    const auto root = std::filesystem::path("build") / "tmp_cache" / name;
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    return root;
}

void write_file(const std::filesystem::path& path, std::string_view content) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.flush();
}

std::string fake_sha(char seed) {
    std::string s(64, '0');
    for (size_t i = 0; i < 64; ++i) s[i] = static_cast<char>('a' + ((seed + static_cast<char>(i)) % 6));
    return s;
}

Quality local_quality(QualityLevel level, const std::string& codec, int bitrate, bool lossless) {
    Quality q;
    q.level = level;
    q.codec = codec;
    q.lossless = lossless;
    q.bitrate = bitrate;
    q.channels = 2;
    if (lossless) {
        q.sample_rate = 44100;
        q.bit_depth = 16;
    }
    return q;
}

BaseInfo mk_local(const std::string& title, const std::string& artist, int duration, char sha_seed,
                  const Quality& q) {
    BaseInfo b;
    b.platform = "local";
    b.version = "1.0.0";
    b.sha = fake_sha(sha_seed);
    b.location = "tracks/" + b.sha + "/audio.flac";
    b.name = title;
    b.author = artist;
    b.secret = 0;
    b.duration_seconds = duration;
    b.quality = q;
    b.tag_source = "vorbis";
    return b;
}

const CacheLayerStats* layer_of(const CacheStats& stats, const std::string& name) {
    for (const auto& layer : stats.layers) {
        if (layer.name == name) return &layer;
    }
    return nullptr;
}

}  // namespace

TEST_CASE(test_cache_stats_counts_each_layer) {
    const auto root = fresh_root("stats");
    LibraryPaths paths(root);
    CHECK(paths.ensure_layout().ok());
    MetaCache meta(paths);
    LibraryIndex index(paths);
    CacheManager manager(paths, meta, index);

    write_file(paths.audio_cache() / "aaaa.flac", "0123456789");   // 10 字节
    write_file(paths.stream_cache() / "seg_0.bin", "01234");       // 5
    write_file(paths.lyric_cache() / "aaaa.json", "[00:01]hi");    // 9
    write_file(paths.cover_cache() / "hash.jpg", "1234567");       // 7
    write_file(paths.tmp() / "dl_0001.part", "012");               // 3

    const BaseInfo base =
        mk_local("Song", "Artist", 200, 1, local_quality(QualityLevel::Lossless, "flac", 1411, true));
    CHECK(meta.put(base).ok());
    // 损坏条目（1029）计入 broken_meta
    write_file(paths.meta() / "broken_song" / "broken_quality" / "base.json", "{ not json");

    const CacheStats stats = manager.stats();
    CHECK_EQ(stats.layers.size(), size_t{6});
    CHECK_EQ(stats.layers[0].name, std::string("meta"));
    CHECK_EQ(stats.layers[5].name, std::string("tmp"));

    const auto* audio = layer_of(stats, "audio");
    CHECK(audio != nullptr);
    if (audio != nullptr) {
        CHECK_EQ(audio->entries, size_t{1});
        CHECK_EQ(audio->bytes, std::uintmax_t{10});
    }
    const auto* stream = layer_of(stats, "stream");
    CHECK(stream != nullptr);
    if (stream != nullptr) CHECK_EQ(stream->bytes, std::uintmax_t{5});
    const auto* lyric = layer_of(stats, "lyric");
    CHECK(lyric != nullptr);
    if (lyric != nullptr) CHECK_EQ(lyric->bytes, std::uintmax_t{9});
    const auto* cover = layer_of(stats, "cover");
    CHECK(cover != nullptr);
    if (cover != nullptr) CHECK_EQ(cover->bytes, std::uintmax_t{7});
    const auto* tmp = layer_of(stats, "tmp");
    CHECK(tmp != nullptr);
    if (tmp != nullptr) CHECK_EQ(tmp->bytes, std::uintmax_t{3});
    const auto* meta_layer = layer_of(stats, "meta");
    CHECK(meta_layer != nullptr);
    if (meta_layer != nullptr) CHECK_EQ(meta_layer->entries, size_t{2});   // 合法 + 损坏

    std::uintmax_t sum = 0;
    for (const auto& layer : stats.layers) sum += layer.bytes;
    CHECK_EQ(stats.usage_bytes, sum);
    CHECK_EQ(stats.broken_meta, size_t{1});
    CHECK_EQ(stats.pending_eviction, size_t{0});

    const CacheConfig config = manager.config();
    CHECK_EQ(stats.limit_bytes,
             config.limit_meta + config.limit_audio + config.limit_lyric + config.limit_cover);

    manager.mark_in_use("aaaa");
    CHECK_EQ(manager.stats().pending_eviction, size_t{1});
}

TEST_CASE(test_cache_clear_audio_keeps_tracks_and_index) {
    const auto root = fresh_root("clear_audio");
    LibraryPaths paths(root);
    CHECK(paths.ensure_layout().ok());
    MetaCache meta(paths);
    LibraryIndex index(paths);
    CacheManager manager(paths, meta, index);

    write_file(paths.audio_cache() / "aaaa.flac", "0123456789");
    write_file(paths.audio_cache() / "bbbb.flac", "0123456789");

    const BaseInfo base =
        mk_local("Song", "Artist", 200, 2, local_quality(QualityLevel::Lossless, "flac", 1411, true));
    const auto outcome = meta.put(base);
    CHECK(outcome.ok());
    if (outcome.ok()) {
        index.upsert(base, outcome.value().path);
        CHECK(index.save().ok());
    }

    // 曲库文件：音频本体、sec.json、预设、usec、库锁
    write_file(paths.track_dir(base.sha) / "audio.flac", "AUDIO");
    write_file(paths.sec_file(base.sha), "{\"kdf\":\"pbkdf2\"}");
    write_file(paths.presets() / "default.json", "{}");
    write_file(paths.usec_file(), "[]");
    std::error_code ec;
    std::filesystem::create_directories(paths.library_lock(), ec);
    write_file(paths.library_lock() / "owner", "pid=1\n");

    const auto index_size_before = std::filesystem::file_size(paths.index_file(), ec);

    size_t skipped = 77;
    const auto cleared = manager.clear("audio", &skipped);
    CHECK(cleared.ok());
    if (cleared.ok()) CHECK_EQ(cleared.value(), size_t{2});
    CHECK_EQ(skipped, size_t{0});

    CHECK_FALSE(std::filesystem::exists(paths.audio_cache() / "aaaa.flac", ec));
    CHECK_FALSE(std::filesystem::exists(paths.audio_cache() / "bbbb.flac", ec));
    // tracks/ 与 index/ 不受影响
    CHECK(std::filesystem::exists(paths.track_dir(base.sha) / "audio.flac", ec));
    CHECK(std::filesystem::exists(paths.sec_file(base.sha), ec));
    CHECK(std::filesystem::exists(paths.index_file(), ec));
    CHECK_EQ(std::filesystem::file_size(paths.index_file(), ec), index_size_before);
    CHECK(std::filesystem::exists(paths.presets() / "default.json", ec));
    CHECK(std::filesystem::exists(paths.usec_file(), ec));
    CHECK(std::filesystem::exists(paths.library_lock(), ec));
    CHECK_EQ(index.size(), size_t{1});

    // 未知层 → 1007
    const auto bad = manager.clear("no_such_layer");
    CHECK_FALSE(bad.ok());
    CHECK(bad.status().code == ErrorCode::InvalidMetadata);
}

TEST_CASE(test_cache_clear_meta_removes_base_json_and_rebuilds_index) {
    const auto root = fresh_root("clear_meta");
    LibraryPaths paths(root);
    CHECK(paths.ensure_layout().ok());
    MetaCache meta(paths);
    LibraryIndex index(paths);
    CacheManager manager(paths, meta, index);

    const BaseInfo first =
        mk_local("First Song", "Artist A", 200, 3, local_quality(QualityLevel::Lossless, "flac", 1411, true));
    const BaseInfo second =
        mk_local("Second Song", "Artist B", 180, 4, local_quality(QualityLevel::High, "mp3", 320, false));
    const auto first_outcome = meta.put(first);
    const auto second_outcome = meta.put(second);
    CHECK(first_outcome.ok());
    CHECK(second_outcome.ok());
    if (!first_outcome.ok() || !second_outcome.ok()) return;
    const MetaPath first_path = first_outcome.value().path;
    const MetaPath second_path = second_outcome.value().path;

    index.upsert(first, first_path);
    index.upsert(second, second_path);
    CHECK(index.save().ok());
    CHECK(std::filesystem::exists(paths.meta_file(first_path)));

    write_file(paths.track_dir(first.sha) / "audio.flac", "AUDIO");

    LibraryIndex reloaded(paths);
    CHECK(reloaded.load().ok());
    CHECK_EQ(reloaded.size(), size_t{2});

    size_t skipped = 99;
    const auto cleared = manager.clear("meta", &skipped);
    CHECK(cleared.ok());
    if (cleared.ok()) CHECK_EQ(cleared.value(), size_t{2});
    CHECK_EQ(skipped, size_t{0});

    CHECK_FALSE(std::filesystem::exists(paths.meta_file(first_path)));
    CHECK_FALSE(std::filesystem::exists(paths.meta_file(second_path)));
    CHECK_EQ(index.size(), size_t{0});          // 内存索引已清空
    LibraryIndex after(paths);
    CHECK(after.load().ok());
    CHECK_EQ(after.size(), size_t{0});          // index.json 也已刷新

    // 音频本体（tracks/）不受影响
    CHECK(std::filesystem::exists(paths.track_dir(first.sha) / "audio.flac"));
    CHECK_EQ(manager.stats().broken_meta, size_t{0});
}

TEST_CASE(test_cache_clear_meta_skips_in_use_entry) {
    const auto root = fresh_root("clear_meta_in_use");
    LibraryPaths paths(root);
    CHECK(paths.ensure_layout().ok());
    MetaCache meta(paths);
    LibraryIndex index(paths);
    CacheManager manager(paths, meta, index);

    const BaseInfo playing =
        mk_local("Playing Song", "Artist A", 200, 5, local_quality(QualityLevel::Lossless, "flac", 1411, true));
    const BaseInfo idle =
        mk_local("Idle Song", "Artist B", 180, 6, local_quality(QualityLevel::Lossless, "flac", 1411, true));
    const auto playing_outcome = meta.put(playing);
    const auto idle_outcome = meta.put(idle);
    CHECK(playing_outcome.ok());
    CHECK(idle_outcome.ok());
    if (!playing_outcome.ok() || !idle_outcome.ok()) return;
    const MetaPath playing_path = playing_outcome.value().path;
    const MetaPath idle_path = idle_outcome.value().path;

    // 正在播放的条目：以 sha 标记 pending eviction（1.9 规则 2）
    manager.mark_in_use(playing.sha);
    CHECK_EQ(manager.in_use().size(), size_t{1});

    size_t skipped = 0;
    const auto cleared = manager.clear("meta", &skipped);
    CHECK(cleared.ok());
    if (cleared.ok()) CHECK_EQ(cleared.value(), size_t{1});
    CHECK_EQ(skipped, size_t{1});

    CHECK(std::filesystem::exists(paths.meta_file(playing_path)));      // 被跳过，保留
    CHECK_FALSE(std::filesystem::exists(paths.meta_file(idle_path)));
    CHECK_EQ(index.size(), size_t{1});                                  // 由磁盘剩余条目重建

    // 释放后可清理
    manager.release_in_use(playing.sha);
    CHECK_EQ(manager.in_use().size(), size_t{0});
    size_t skipped_after = 0;
    const auto again = manager.clear("meta", &skipped_after);
    CHECK(again.ok());
    if (again.ok()) CHECK_EQ(again.value(), size_t{1});
    CHECK_EQ(skipped_after, size_t{0});
    CHECK_EQ(index.size(), size_t{0});
}

TEST_CASE(test_cache_touch_audio_updates_mtime_and_missing_is_1004) {
    const auto root = fresh_root("touch");
    LibraryPaths paths(root);
    CHECK(paths.ensure_layout().ok());
    MetaCache meta(paths);
    LibraryIndex index(paths);
    CacheManager manager(paths, meta, index);

    const auto cached = paths.audio_cache() / "aaaa.flac";
    write_file(cached, "0123456789");
    std::error_code ec;
    const auto past = std::filesystem::file_time_type::clock::now() - std::chrono::hours(1);
    std::filesystem::last_write_time(cached, past, ec);
    CHECK_FALSE(ec);
    const auto before = std::filesystem::last_write_time(cached, ec);

    CHECK(manager.touch_audio("aaaa").ok());
    const auto after = std::filesystem::last_write_time(cached, ec);
    CHECK(after > before);

    const Status missing = manager.touch_audio("bbbb");
    CHECK_FALSE(missing.ok());
    CHECK(missing.code == ErrorCode::FileNotFound);
}

TEST_CASE(test_cache_evict_audio_to_limit_lru) {
    const auto root = fresh_root("evict_lru");
    LibraryPaths paths(root);
    CHECK(paths.ensure_layout().ok());
    MetaCache meta(paths);
    LibraryIndex index(paths);
    CacheManager manager(paths, meta, index);

    // 先设上限（目录为空，不会触发立即淘汰），再写文件
    CacheConfig config;
    config.limit_audio = 8;
    CHECK(manager.set_config(config).ok());

    const auto oldest = paths.audio_cache() / "aaaa.bin";
    const auto middle = paths.audio_cache() / "bbbb.bin";
    const auto newest = paths.audio_cache() / "cccc.bin";
    write_file(oldest, "aaaa");   // 4
    write_file(middle, "bbbb");   // 4
    write_file(newest, "cccc");   // 4

    std::error_code ec;
    const auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(oldest, now - std::chrono::seconds(300), ec);
    std::filesystem::last_write_time(middle, now - std::chrono::seconds(200), ec);
    std::filesystem::last_write_time(newest, now - std::chrono::seconds(100), ec);

    const auto evicted = manager.evict_audio_to_limit();
    CHECK(evicted.ok());
    if (evicted.ok()) CHECK_EQ(evicted.value(), size_t{1});   // 12 → 8，只淘汰最旧的 1 个
    CHECK_FALSE(std::filesystem::exists(oldest, ec));
    CHECK(std::filesystem::exists(middle, ec));
    CHECK(std::filesystem::exists(newest, ec));

    // 已在上限内时不再淘汰
    const auto again = manager.evict_audio_to_limit();
    CHECK(again.ok());
    if (again.ok()) CHECK_EQ(again.value(), size_t{0});
}

TEST_CASE(test_cache_evict_audio_skips_in_use) {
    const auto root = fresh_root("evict_in_use");
    LibraryPaths paths(root);
    CHECK(paths.ensure_layout().ok());
    MetaCache meta(paths);
    LibraryIndex index(paths);
    CacheManager manager(paths, meta, index);

    CacheConfig config;
    config.limit_audio = 0;
    CHECK(manager.set_config(config).ok());   // 空目录：0 > 0 为假，不淘汰

    const auto playing = paths.audio_cache() / "aaaa.bin";   // 更旧
    const auto idle = paths.audio_cache() / "bbbb.bin";      // 更新
    write_file(playing, "aaaa");
    write_file(idle, "bbbb");

    std::error_code ec;
    const auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(playing, now - std::chrono::seconds(300), ec);
    std::filesystem::last_write_time(idle, now - std::chrono::seconds(100), ec);

    manager.mark_in_use("aaaa");   // 键 = sha（文件名主干）
    const auto evicted = manager.evict_audio_to_limit();
    CHECK(evicted.ok());
    if (evicted.ok()) CHECK_EQ(evicted.value(), size_t{1});
    CHECK(std::filesystem::exists(playing, ec));    // 正在播放：不得淘汰
    CHECK_FALSE(std::filesystem::exists(idle, ec));

    // 全部 in_use 时不再淘汰
    manager.mark_in_use("bbbb");
    const auto blocked = manager.evict_audio_to_limit();
    CHECK(blocked.ok());
    if (blocked.ok()) CHECK_EQ(blocked.value(), size_t{0});
    CHECK(std::filesystem::exists(playing, ec));
}

TEST_CASE(test_cache_config_persists_to_cache_json) {
    const auto root = fresh_root("config");
    LibraryPaths paths(root);
    CHECK(paths.ensure_layout().ok());
    MetaCache meta(paths);
    LibraryIndex index(paths);
    CacheManager manager(paths, meta, index);

    CacheConfig config;
    config.limit_meta = 1024;
    config.limit_audio = 2048;
    config.limit_lyric = 16;
    config.limit_cover = 32;
    config.preload = true;
    CHECK(manager.set_config(config).ok());
    CHECK(std::filesystem::exists(paths.cache() / "cache.json"));

    CacheManager reopened(paths, meta, index);
    const CacheConfig loaded = reopened.config();
    CHECK_EQ(loaded.limit_meta, std::uintmax_t{1024});
    CHECK_EQ(loaded.limit_audio, std::uintmax_t{2048});
    CHECK_EQ(loaded.limit_lyric, std::uintmax_t{16});
    CHECK_EQ(loaded.limit_cover, std::uintmax_t{32});
    CHECK_EQ(loaded.preload, true);
    CHECK_EQ(reopened.stats().limit_bytes, std::uintmax_t{1024 + 2048 + 16 + 32});

    // 缩小上限时按 LRU 立即淘汰（2.11 规则 6）
    const auto old_file = paths.audio_cache() / "oooo.bin";
    write_file(old_file, "0123456789");
    CacheConfig shrink;
    shrink.limit_audio = 4;
    CHECK(manager.set_config(shrink).ok());
    std::error_code ec;
    CHECK_FALSE(std::filesystem::exists(old_file, ec));
}

TEST_CASE(test_cache_clear_all_never_touches_library_files) {
    const auto root = fresh_root("clear_all");
    LibraryPaths paths(root);
    CHECK(paths.ensure_layout().ok());
    MetaCache meta(paths);
    LibraryIndex index(paths);
    CacheManager manager(paths, meta, index);

    const BaseInfo base =
        mk_local("Song", "Artist", 200, 7, local_quality(QualityLevel::Lossless, "flac", 1411, true));
    CHECK(meta.put(base).ok());
    write_file(paths.audio_cache() / "aaaa.flac", "0123456789");
    write_file(paths.stream_cache() / "seg_0.bin", "01234");
    write_file(paths.lyric_cache() / "aaaa.json", "[00:01]hi");
    write_file(paths.cover_cache() / "hash.jpg", "1234567");
    write_file(paths.tmp() / "dl_0001.part", "012");

    write_file(paths.track_dir(base.sha) / "audio.flac", "AUDIO");
    write_file(paths.sec_file(base.sha), "{}");
    write_file(paths.presets() / "default.json", "{}");
    write_file(paths.usec_file(), "[]");
    write_file(paths.index_file(), "{}");
    std::error_code ec;
    std::filesystem::create_directories(paths.library_lock(), ec);

    size_t skipped = 42;
    const auto cleared = manager.clear("all", &skipped);
    CHECK(cleared.ok());
    if (cleared.ok()) CHECK_EQ(cleared.value(), size_t{6});   // 5 个文件 + 1 条 meta
    CHECK_EQ(skipped, size_t{0});
    CHECK_EQ(manager.stats().usage_bytes, std::uintmax_t{0});

    // 1.9 规则 3 / 2.11 规则 1：tracks/、预设、usec、库锁、index/ 一律保留
    CHECK(std::filesystem::exists(paths.track_dir(base.sha) / "audio.flac", ec));
    CHECK(std::filesystem::exists(paths.sec_file(base.sha), ec));
    CHECK(std::filesystem::exists(paths.presets() / "default.json", ec));
    CHECK(std::filesystem::exists(paths.usec_file(), ec));
    CHECK(std::filesystem::exists(paths.index_file(), ec));
    CHECK(std::filesystem::exists(paths.library_lock(), ec));

    // ""/"meta,audio" 等价写法
    write_file(paths.audio_cache() / "cccc.flac", "01");
    write_file(paths.lyric_cache() / "cccc.json", "01");
    const auto partial = manager.clear("meta,audio");
    CHECK(partial.ok());
    if (partial.ok()) CHECK_EQ(partial.value(), size_t{1});
    CHECK_FALSE(std::filesystem::exists(paths.audio_cache() / "cccc.flac", ec));
    CHECK(std::filesystem::exists(paths.lyric_cache() / "cccc.json", ec));
    CHECK_EQ(manager.clear("").ok(), true);
}

TEST_CASE(test_cache_rebuild_meta_rewrites_and_refreshes_index) {
    const auto root = fresh_root("rebuild");
    LibraryPaths paths(root);
    CHECK(paths.ensure_layout().ok());
    MetaCache meta(paths);
    LibraryIndex index(paths);
    CacheManager manager(paths, meta, index);

    const BaseInfo base =
        mk_local("Song", "Artist", 200, 8, local_quality(QualityLevel::Lossless, "flac", 1411, true));
    const auto outcome = meta.put(base);
    CHECK(outcome.ok());
    if (!outcome.ok()) return;
    const MetaPath path = outcome.value().path;

    CHECK_EQ(index.size(), size_t{0});
    CHECK(manager.rebuild_meta().ok());          // 全量：重写现存条目 + 刷新库索引
    CHECK_EQ(index.size(), size_t{1});
    CHECK(std::filesystem::exists(paths.meta_file(path)));

    // 定向重建
    CHECK(manager.rebuild_meta(path.song_key, path.quality_key).ok());
    const Status missing = manager.rebuild_meta("no_such_song", "no_such_quality");
    CHECK_FALSE(missing.ok());
    CHECK(missing.code == ErrorCode::BaseRebuildFailed);
    CHECK_EQ(meta.count(), size_t{1});           // 不产生第二份 base.json
}
