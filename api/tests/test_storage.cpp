// MetaCache / 库索引 / 库锁 测试（Readme 1.1「存放位置」、1.9、1.10）
#include <filesystem>
#include <fstream>
#include <string>

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

// 造一个合法的 64 位小写 sha（由 seed 生成，便于区分不同内容）
std::string fake_sha(char seed) {
    std::string s(64, '0');
    for (size_t i = 0; i < 64; ++i) s[i] = static_cast<char>('a' + ((seed + static_cast<char>(i)) % 6));
    return s;
}

BaseInfo mk_local(const std::string& title, const std::string& artist, int duration,
                  char sha_seed, const Quality& q) {
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

}  // namespace

TEST_CASE(test_storage_path_layout_and_created) {
    const auto root = fresh_root("layout");
    LibraryPaths paths(root);
    CHECK(paths.ensure_layout().ok());
    MetaCache cache(paths);

    const BaseInfo base = mk_local("Example Song", "Example Artist", 215, 1,
                                   local_quality(QualityLevel::Lossless, "flac", 1411, true));
    CHECK(base.validate().ok());

    const MetaPath path = cache.path_for(base);
    CHECK(path.valid());
    CHECK_EQ(path.quality_key, std::string("local.lossless-flac-1411"));
    CHECK(path.song_key.find("Example Song") != std::string::npos);
    // 一首歌一个文件夹 / 每个音质一个文件夹
    CHECK_EQ(paths.meta_file(path).parent_path().filename().string(), path.quality_key);
    CHECK_EQ(paths.meta_file(path).parent_path().parent_path().filename().string(), path.song_key);

    const auto outcome = cache.put(base);
    CHECK(outcome.ok());
    if (outcome.ok()) {
        CHECK(outcome.value().action == WriteAction::Created);
        CHECK_EQ(outcome.value().path, path);
    }
    CHECK(std::filesystem::exists(paths.meta_file(path)));
    CHECK_EQ(cache.count(), size_t{1});

    // 回读一致
    const auto read = cache.read(path);
    CHECK(read.ok());
    if (read.ok()) {
        CHECK_EQ(read.value().sha, base.sha);
        CHECK_EQ(read.value().name, base.name);
        CHECK(read.value().quality.level == QualityLevel::Lossless);
    }
}

TEST_CASE(test_storage_duplicate_same_content_is_skipped) {
    const auto root = fresh_root("dup");
    LibraryPaths paths(root);
    MetaCache cache(paths);
    const BaseInfo base = mk_local("Song", "Artist", 200, 2,
                                   local_quality(QualityLevel::High, "mp3", 320, false));

    CHECK(cache.put(base).ok());
    const auto again = cache.put(base);
    CHECK(again.ok());
    if (again.ok()) CHECK(again.value().action == WriteAction::Skipped);
    CHECK_EQ(cache.count(), size_t{1});   // 不重复建目录
}

TEST_CASE(test_storage_same_song_same_quality_different_content_conflicts) {
    const auto root = fresh_root("conflict");
    LibraryPaths paths(root);
    MetaCache cache(paths);
    const Quality q = local_quality(QualityLevel::Lossless, "flac", 1411, true);

    const BaseInfo first = mk_local("Song", "Artist", 200, 3, q);
    BaseInfo second = mk_local("Song", "Artist", 200, 4, q);   // 同曲同质、不同 sha
    second.location = "tracks/" + second.sha + "/audio.flac";

    CHECK(cache.put(first).ok());
    const auto conflict = cache.put(second);          // 默认 Skip
    CHECK_FALSE(conflict.ok());
    CHECK(conflict.status().code == ErrorCode::BaseConflict);
    CHECK_EQ(cache.count(), size_t{1});               // 既有记录未被覆盖

    // Merge：保留既有
    const auto merged = cache.put(second, ConflictPolicy::Merge);
    CHECK(merged.ok());
    if (merged.ok()) CHECK(merged.value().action == WriteAction::Merged);

    // Overwrite：显式允许时才替换
    const auto overwritten = cache.put(second, ConflictPolicy::Overwrite);
    CHECK(overwritten.ok());
    if (overwritten.ok()) {
        CHECK(overwritten.value().action == WriteAction::Overwritten);
        CHECK_EQ(overwritten.value().base.sha, second.sha);
    }
}

TEST_CASE(test_storage_different_qualities_are_stored_separately) {
    const auto root = fresh_root("quality");
    LibraryPaths paths(root);
    MetaCache cache(paths);

    const BaseInfo flac = mk_local("Song", "Artist", 200, 5,
                                   local_quality(QualityLevel::Lossless, "flac", 1411, true));
    const BaseInfo mp3 = mk_local("Song", "Artist", 200, 6,
                                  local_quality(QualityLevel::High, "mp3", 320, false));

    const auto a = cache.put(flac);
    const auto b = cache.put(mp3);
    CHECK(a.ok());
    CHECK(b.ok());
    CHECK_EQ(cache.count(), size_t{2});                       // 不同音质 → 重复存储
    CHECK(a.value().path.song_key == b.value().path.song_key);  // 同一首歌
    CHECK_NE(a.value().path.quality_key, b.value().path.quality_key);
    CHECK(std::filesystem::exists(paths.meta_file(a.value().path)));
    CHECK(std::filesystem::exists(paths.meta_file(b.value().path)));
}

TEST_CASE(test_storage_platform_quality_key_isolated) {
    const auto root = fresh_root("platform");
    LibraryPaths paths(root);
    MetaCache cache(paths);

    Quality netease_lossless;
    netease_lossless.level = QualityLevel::Lossless;
    netease_lossless.codec = "flac";
    netease_lossless.lossless = true;
    netease_lossless.quality_id = "lossless";

    BaseInfo a = mk_local("Song", "Artist", 200, 7, netease_lossless);
    a.platform = "netease";
    a.item_id = "2002";
    a.location = "netease:2002";
    BaseInfo b = a;
    b.platform = "radish";
    b.item_id = "t_1001";
    b.sha = fake_sha(8);                 // 不同平台的文件内容不同 → sha 不同
    b.location = "radish:t_1001";

    const auto ra = cache.put(a);
    const auto rb = cache.put(b);
    CHECK(ra.ok());
    CHECK(rb.ok());
    // 不同平台的同名音质必须落在不同目录
    CHECK_EQ(ra.value().path.quality_key, std::string("netease.lossless"));
    CHECK_EQ(rb.value().path.quality_key, std::string("radish.lossless"));
    CHECK_NE(ra.value().path.quality_key, rb.value().path.quality_key);
    CHECK_EQ(cache.count(), size_t{2});
}

TEST_CASE(test_storage_location_collision_conflicts) {
    const auto root = fresh_root("location");
    LibraryPaths paths(root);
    MetaCache cache(paths);
    const Quality q = local_quality(QualityLevel::Lossless, "flac", 1411, true);

    BaseInfo first = mk_local("Song A", "Artist", 200, 8, q);
    BaseInfo second = mk_local("Song B", "Artist", 210, 9, q);
    second.location = first.location;   // 两个 base.json 指向同一物理文件

    CHECK(cache.put(first).ok());
    const auto conflict = cache.put(second);
    CHECK_FALSE(conflict.ok());
    CHECK(conflict.status().code == ErrorCode::BaseConflict);
}

TEST_CASE(test_storage_path_content_mismatch_detected) {
    const auto root = fresh_root("mismatch");
    LibraryPaths paths(root);
    MetaCache cache(paths);
    const BaseInfo base = mk_local("Song", "Artist", 200, 10,
                                   local_quality(QualityLevel::Lossless, "flac", 1411, true));
    CHECK(cache.put(base).ok());

    // 手工把 base.json 塞进"错误"的 quality_key 目录 → 必须报 1029
    const MetaPath wrong = cache.path_for_song_quality(cache.path_for(base).song_key, "local.high-mp3-320");
    const auto dir = paths.meta_dir(wrong);
    std::filesystem::create_directories(dir);
    std::ofstream out(paths.meta_file(wrong), std::ios::binary | std::ios::trunc);
    out << base.to_json().dump(2);
    out.close();

    const auto read = cache.read(wrong);
    CHECK_FALSE(read.ok());
    CHECK(read.status().code == ErrorCode::MetaPathMismatch);

    const auto broken = cache.broken_paths();
    CHECK_EQ(broken.size(), size_t{1});
}

TEST_CASE(test_storage_find_and_clear) {
    const auto root = fresh_root("find");
    LibraryPaths paths(root);
    MetaCache cache(paths);
    const BaseInfo base = mk_local("Song", "Artist", 200, 11,
                                   local_quality(QualityLevel::Lossless, "flac", 1411, true));
    CHECK(cache.put(base).ok());

    const auto by_sha = cache.find_by_sha(base.sha);
    CHECK(by_sha.has_value());
    CHECK(cache.find_by_sha(fake_sha(12)).has_value() == false);

    const BaseInfo shrunk = BaseInfo::from_json(base.to_json());
    CHECK_EQ(shrunk.sha, base.sha);

    CHECK_EQ(cache.clear_all(), size_t{1});
    CHECK_EQ(cache.count(), size_t{0});
}

TEST_CASE(test_storage_index_and_rebuild) {
    const auto root = fresh_root("index");
    LibraryPaths paths(root);
    CHECK(paths.ensure_layout().ok());
    MetaCache cache(paths);
    LibraryIndex index(paths);
    CHECK(index.load().ok());

    const BaseInfo base = mk_local("Song", "Artist", 200, 13,
                                   local_quality(QualityLevel::Lossless, "flac", 1411, true));
    const auto outcome = cache.put(base);
    CHECK(outcome.ok());
    index.upsert(base, outcome.value().path);
    CHECK(index.save().ok());
    CHECK_EQ(index.size(), size_t{1});

    LibraryIndex reloaded(paths);
    CHECK(reloaded.load().ok());
    CHECK(reloaded.find_by_sha(base.sha).has_value());

    // 磁盘变了 → 以磁盘为准重建
    LibraryIndex rebuilt(paths);
    CHECK(rebuilt.rebuild_from_disk(cache).ok());
    CHECK_EQ(rebuilt.size(), size_t{1});
}

TEST_CASE(test_storage_library_lock_excludes_second_holder) {
    const auto root = fresh_root("lock");
    LibraryPaths paths(root);
    CHECK(paths.ensure_layout().ok());

    LibraryLock first(paths);
    CHECK(first.acquire(100).ok());
    CHECK(first.held());

    LibraryLock second(paths);
    const Status s = second.acquire(100);
    CHECK_FALSE(s.ok());
    CHECK(s.code == ErrorCode::LibraryLocked);

    first.release();
    CHECK_FALSE(first.held());
    CHECK(second.acquire(100).ok());
}

TEST_CASE(test_storage_rejects_invalid_base) {
    const auto root = fresh_root("invalid");
    LibraryPaths paths(root);
    MetaCache cache(paths);

    BaseInfo bad = mk_local("Song", "Artist", 200, 14,
                            local_quality(QualityLevel::Lossless, "flac", 1411, true));
    bad.sha = "NOT-A-HEX";
    const auto r = cache.put(bad);
    CHECK_FALSE(r.ok());
    CHECK(r.status().code == ErrorCode::InvalidMetadata);

    BaseInfo no_version = bad;
    no_version.sha = fake_sha(15);
    no_version.version.clear();
    CHECK_FALSE(cache.put(no_version).ok());

    BaseInfo bad_secret = bad;
    bad_secret.sha = fake_sha(16);
    bad_secret.secret = 2;
    CHECK_FALSE(cache.put(bad_secret).ok());
}
