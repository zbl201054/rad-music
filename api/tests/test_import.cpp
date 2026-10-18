// 音乐导入（Readme 1.10）与导入 API（Readme 2.12）测试
//
// 说明：仓库内没有真实音频文件，因此所有用例都在 build/tmp_import/<case>/ 下
// 自建临时曲库根与源目录，并注入 read_tags_fn / probe_audio_fn / sha_fn 三个桩，
// 不依赖任何真实音频与第三方库。
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "radmusic/crypto.hpp"
#include "radmusic/import.hpp"
#include "radmusic/storage.hpp"
#include "radmusic/tags.hpp"
#include "test_framework.hpp"

using namespace radmusic;
namespace fs = std::filesystem;

namespace {

// 每个用例使用独立的临时目录，结束时由 clean() 清理
struct Temp {
    fs::path root;
    fs::path src;

    explicit Temp(const std::string& name) {
        const fs::path base = fs::path("build") / "tmp_import" / name;
        std::error_code ec;
        fs::remove_all(base, ec);
        root = base / "lib";
        src = base / "src";
        fs::create_directories(root, ec);
        fs::create_directories(src, ec);
    }

    ~Temp() { clean(); }

    void clean() {
        std::error_code ec;
        fs::remove_all(root.parent_path(), ec);
    }

    LibraryPaths paths() const { return LibraryPaths(root); }
};

// 把 text 映射成 64 位小写十六进制（模拟 sha，内容不同则 sha 不同）
std::string imp_fake_hex64(const std::string& text) {
    const std::string digest = crypto::sha256_hex(text);
    std::string out;
    out.reserve(64);
    for (size_t i = 0; i < 64; ++i) {
        const unsigned char c = static_cast<unsigned char>(digest[i % digest.size()]);
        const char* hex = "0123456789abcdef";
        out.push_back(hex[c & 0x0F]);
    }
    return out;
}

std::string imp_sha_of_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return imp_fake_hex64(content + "|" + p.filename().string());
}

void imp_write_file(const fs::path& p, const std::string& content) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

TagData imp_make_tags(const std::string& title, const std::string& artist, const std::string& album,
                  int track_no, int duration) {
    TagData t;
    t.title = title;
    t.artist = artist;
    t.album = album;
    t.track_no = track_no;
    t.duration_seconds = duration;
    t.tag_source = "vorbis";
    return t;
}

AudioStreamInfo imp_flac_1411(long long duration_ms = 215000) {
    AudioStreamInfo info;
    info.codec = "flac";
    info.lossless = true;
    info.bitrate = 1411;
    info.sample_rate = 44100;
    info.bit_depth = 16;
    info.channels = 2;
    info.duration_ms = duration_ms;
    return info;
}

AudioStreamInfo imp_mp3_320(long long duration_ms = 215000) {
    AudioStreamInfo info;
    info.codec = "mp3";
    info.lossless = false;
    info.bitrate = 320;
    info.sample_rate = 44100;
    info.channels = 2;
    info.duration_ms = duration_ms;
    return info;
}

// 默认桩：标签与流信息都成功
struct Stubs {
    TagData tags = imp_make_tags("Example Song", "Example Artist", "Example Album", 3, 215);
    AudioStreamInfo stream = imp_flac_1411();
    bool tags_fail = false;
    ErrorCode tags_error = ErrorCode::TagParseFailed;

    void install(ImportManager& mgr) {
        const TagData t = tags;
        const AudioStreamInfo s = stream;
        const bool fail = tags_fail;
        const ErrorCode code = tags_error;
        mgr.read_tags_fn = [t, fail, code](const std::string&) -> Result<TagData> {
            if (fail) return Result<TagData>::failure(code, "标签缺失或损坏（测试桩）");
            return Result<TagData>::success(t);
        };
        mgr.probe_audio_fn = [s](const std::string&) -> Result<AudioStreamInfo> {
            return Result<AudioStreamInfo>::success(s);
        };
        mgr.sha_fn = [](const std::string& path) { return imp_sha_of_file(fs::path(path)); };
    }
};

json::Value imp_import_request(const fs::path& source, const std::string& mode = "copy",
                           const std::string& conflict = "skip", bool dry_run = false) {
    json::Value req = json::Value::object();
    json::Value paths = json::Value::array();
    paths.push_back(source.string());
    req.set("paths", paths);
    req.set("recursive", true);
    req.set("mode", mode);
    req.set("conflict", conflict);
    req.set("dry_run", dry_run);
    return req;
}

// 统计目录树中的文件数量（用于断言"不写盘"）
size_t imp_count_files(const fs::path& dir) {
    size_t n = 0;
    std::error_code ec;
    if (!fs::exists(dir, ec)) return 0;
    for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
        std::error_code fec;
        if (entry.is_regular_file(fec) && !fec) ++n;
    }
    return n;
}

size_t imp_count_conflicts_of_type(const json::Value& array, const std::string& type) {
    size_t n = 0;
    for (const auto& item : array.as_array()) {
        if (item.get_string("type") == type) ++n;
    }
    return n;
}

// 报告里的路径统一用 '/' 分隔（实现侧 normalize_display）
std::string imp_shown_path(const fs::path& p) {
    std::string s = p.string();
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

}  // namespace

TEST_CASE(test_import_copy_success_writes_tracks_meta_cover_lyric) {
    Temp tmp("copy_success");
    const fs::path source = tmp.src / "example.flac";
    imp_write_file(source, "FAKE-FLAC-CONTENT-A");

    LibraryPaths paths = tmp.paths();
    MetaCache cache(paths);
    LibraryIndex index(paths);
    CHECK(index.load().ok());
    ImportManager mgr(paths, cache, index);

    Stubs stubs;
    stubs.tags.cover = {0xFF, 0xD8, 0xFF, 0xE0, 0x01, 0x02, 0x03};
    stubs.tags.cover_mime = "image/jpeg";
    stubs.tags.lyrics_text = "[00:01.00]Hello\n[00:02.00]World\n";
    stubs.install(mgr);

    const auto result = mgr.run(imp_import_request(source));
    CHECK(result.ok());
    if (!result.ok()) return;

    const json::Value& data = result.value();
    CHECK_EQ(data.get_string("import_id"), std::string("imp_0001"));
    CHECK_EQ(data.get_int("scanned"), int64_t{1});
    CHECK_EQ(data.get_int("imported"), int64_t{1});
    CHECK_EQ(data.get_int("skipped"), int64_t{0});
    CHECK_EQ(data.get_int("failed"), int64_t{0});
    CHECK_EQ(data.get_bool("dry_run"), false);

    const std::string sha = imp_sha_of_file(source);
    // tracks/<sha>/ 下有音频
    CHECK(fs::exists(paths.track_dir(sha)));
    CHECK(fs::exists(paths.track_dir(sha) / "Example Artist - Example Song.flac"));

    // meta 条目存在且 quality_key 正确
    BaseInfo expected;
    expected.platform = "local";
    expected.version = "1.0.0";
    expected.sha = sha;
    expected.name = "Example Song";
    expected.author = "Example Artist";
    expected.duration_seconds = 215;
    expected.quality = quality_from_stream(imp_flac_1411());
    const MetaPath meta_path = cache.path_for(expected);
    CHECK_EQ(meta_path.quality_key, std::string("local.lossless-flac-1411"));
    CHECK(fs::exists(paths.meta_file(meta_path)));

    const auto stored = cache.read(meta_path);
    CHECK(stored.ok());
    if (stored.ok()) {
        const BaseInfo& base = stored.value();
        CHECK_EQ(base.sha, sha);
        CHECK_EQ(base.name, std::string("Example Song"));
        CHECK_EQ(base.author, std::string("Example Artist"));
        CHECK_EQ(base.album, std::string("Example Album"));
        CHECK_EQ(base.track_no, 3);
        CHECK_EQ(base.duration_seconds, 215);
        CHECK_EQ(base.tag_source, std::string("vorbis"));
        CHECK_EQ(base.secret, 0);
        // location 为相对曲库根
        CHECK(base.location.rfind("tracks/" + sha + "/", 0) == 0);
        // pic 指向 covers/
        CHECK(base.pic.rfind("covers/", 0) == 0);
        CHECK(base.pic.size() > std::string("covers/").size());
        CHECK(fs::exists(paths.root / base.pic));
    }

    // 歌词写入 tracks/<sha>/lyric.lrc
    CHECK(fs::exists(paths.lyric_file(sha)));
    const auto lyric = read_text_file(paths.lyric_file(sha));
    CHECK(lyric.ok());
    if (lyric.ok()) CHECK(lyric.value().find("Hello") != std::string::npos);

    // 索引更新
    CHECK_EQ(index.size(), size_t{1});
    CHECK(index.find_by_sha(sha).has_value());

    // 报告：条目 action = created
    CHECK_EQ(data["items"].size(), size_t{1});
    CHECK_EQ(data["items"][size_t{0}].get_string("action"), std::string("created"));
    CHECK_EQ(data["items"][size_t{0}].get_string("sha"), sha);

    // status / report
    const auto st = mgr.status("imp_0001");
    CHECK(st.ok());
    if (st.ok()) {
        CHECK_EQ(st.value().get_int("imported"), int64_t{1});
        CHECK_EQ(st.value().get_int("scanned"), int64_t{1});
    }
    const auto rep = mgr.report("imp_0001");
    CHECK(rep.ok());
    if (rep.ok()) CHECK_EQ(rep.value()["items"].size(), size_t{1});
}

TEST_CASE(test_import_is_idempotent_on_second_run) {
    Temp tmp("idempotent");
    const fs::path source = tmp.src / "song.flac";
    imp_write_file(source, "FAKE-FLAC-CONTENT-B");

    LibraryPaths paths = tmp.paths();
    MetaCache cache(paths);
    LibraryIndex index(paths);
    ImportManager mgr(paths, cache, index);
    Stubs stubs;
    stubs.install(mgr);

    const auto first = mgr.run(imp_import_request(source));
    CHECK(first.ok());
    if (!first.ok()) return;
    CHECK_EQ(first.value().get_int("imported"), int64_t{1});

    const size_t tracks_before = imp_count_files(paths.tracks());
    const size_t meta_before = imp_count_files(paths.meta());
    CHECK_EQ(meta_before, size_t{1});

    const auto second = mgr.run(imp_import_request(source));
    CHECK(second.ok());
    if (!second.ok()) return;
    CHECK_EQ(second.value().get_int("imported"), int64_t{0});
    CHECK_EQ(second.value().get_int("skipped"), int64_t{1});
    CHECK_EQ(second.value().get_int("failed"), int64_t{0});
    CHECK_EQ(second.value()["items"][size_t{0}].get_string("action"), std::string("skipped"));

    // tracks/ 与 meta 都只有一份
    CHECK_EQ(imp_count_files(paths.tracks()), tracks_before);
    CHECK_EQ(imp_count_files(paths.meta()), meta_before);
    CHECK_EQ(cache.count(), size_t{1});
    CHECK_EQ(index.size(), size_t{1});
    // 源文件保留（copy 模式）
    CHECK(fs::exists(source));
}

TEST_CASE(test_import_same_song_same_quality_conflict_skip_and_overwrite) {
    // ---- conflict = skip：1023，计入 failed ----
    {
        Temp tmp("same_song_quality_skip");
        const fs::path source = tmp.src / "a.flac";
        imp_write_file(source, "CONTENT-ONE");
        const fs::path other = tmp.src / "b.flac";
        imp_write_file(other, "CONTENT-TWO");

        LibraryPaths paths = tmp.paths();
        MetaCache cache(paths);
        LibraryIndex index(paths);
        ImportManager mgr(paths, cache, index);
        Stubs stubs;
        stubs.install(mgr);

        const auto first = mgr.run(imp_import_request(source));
        CHECK(first.ok());
        if (first.ok()) CHECK_EQ(first.value().get_int("imported"), int64_t{1});

        const auto second = mgr.run(imp_import_request(other, "copy", "skip"));
        CHECK(second.ok());
        if (second.ok()) {
            const json::Value& data = second.value();
            CHECK_EQ(data.get_int("imported"), int64_t{0});
            CHECK_EQ(data.get_int("failed"), int64_t{1});
            CHECK_EQ(data["items"][size_t{0}].get_int("code"), int64_t{1023});
            CHECK_EQ(data["items"][size_t{0}].get_string("action"), std::string("failed"));
            CHECK(imp_count_conflicts_of_type(data["conflicts"], "same_song_same_quality") >= size_t{1});
        }
        CHECK_EQ(cache.count(), size_t{1});
        // 既有内容未被覆盖
        BaseInfo probe;
        probe.platform = "local";
        probe.version = "1.0.0";
        probe.sha = imp_sha_of_file(source);
        probe.name = "Example Song";
        probe.author = "Example Artist";
        probe.duration_seconds = 215;
        probe.quality = quality_from_stream(imp_flac_1411());
        const auto stored = cache.read(cache.path_for(probe));
        CHECK(stored.ok());
        if (stored.ok()) CHECK_EQ(stored.value().sha, imp_sha_of_file(source));
    }

    // ---- conflict = overwrite：被覆盖 ----
    {
        Temp tmp("same_song_quality_overwrite");
        const fs::path source = tmp.src / "a.flac";
        imp_write_file(source, "CONTENT-ONE");
        const fs::path other = tmp.src / "b.flac";
        imp_write_file(other, "CONTENT-TWO");

        LibraryPaths paths = tmp.paths();
        MetaCache cache(paths);
        LibraryIndex index(paths);
        ImportManager mgr(paths, cache, index);
        Stubs stubs;
        stubs.install(mgr);

        CHECK(mgr.run(imp_import_request(source)).ok());
        const auto second = mgr.run(imp_import_request(other, "copy", "overwrite"));
        CHECK(second.ok());
        if (second.ok()) {
            const json::Value& data = second.value();
            CHECK_EQ(data.get_int("imported"), int64_t{1});
            CHECK_EQ(data.get_int("failed"), int64_t{0});
            CHECK_EQ(data["items"][size_t{0}].get_string("action"), std::string("overwritten"));
        }
        CHECK_EQ(cache.count(), size_t{1});
        BaseInfo probe;
        probe.platform = "local";
        probe.version = "1.0.0";
        probe.sha = imp_sha_of_file(other);   // 新内容
        probe.name = "Example Song";
        probe.author = "Example Artist";
        probe.duration_seconds = 215;
        probe.quality = quality_from_stream(imp_flac_1411());
        const auto stored = cache.read(cache.path_for(probe));
        CHECK(stored.ok());
        if (stored.ok()) CHECK_EQ(stored.value().sha, imp_sha_of_file(other));
    }
}

TEST_CASE(test_import_same_song_different_quality_creates_two_entries) {
    Temp tmp("different_quality");
    const fs::path lossless = tmp.src / "song.flac";
    imp_write_file(lossless, "LOSSLESS-CONTENT");
    const fs::path compressed = tmp.src / "song.mp3";
    imp_write_file(compressed, "LOSSY-CONTENT");

    LibraryPaths paths = tmp.paths();
    MetaCache cache(paths);
    LibraryIndex index(paths);
    ImportManager mgr(paths, cache, index);

    Stubs stubs;
    stubs.install(mgr);
    // 第二个文件返回不同 AudioStreamInfo（不同音质）
    mgr.probe_audio_fn = [](const std::string& path) -> Result<AudioStreamInfo> {
        if (fs::path(path).extension() == ".mp3") {
            return Result<AudioStreamInfo>::success(imp_mp3_320());
        }
        return Result<AudioStreamInfo>::success(imp_flac_1411());
    };

    const auto result = mgr.run(imp_import_request(tmp.src));
    CHECK(result.ok());
    if (!result.ok()) return;
    const json::Value& data = result.value();
    CHECK_EQ(data.get_int("scanned"), int64_t{2});
    CHECK_EQ(data.get_int("imported"), int64_t{2});
    CHECK_EQ(data.get_int("failed"), int64_t{0});
    CHECK_EQ(data.get_int("skipped"), int64_t{0});

    // 两条记录 + 两个 quality_key 目录
    CHECK_EQ(cache.count(), size_t{2});
    std::vector<std::string> quality_keys;
    for (const auto& [path, base] : cache.list_all_with_paths()) {
        (void)base;
        quality_keys.push_back(path.quality_key);
    }
    bool has_lossless = false;
    bool has_high = false;
    for (const auto& key : quality_keys) {
        if (key == "local.lossless-flac-1411") has_lossless = true;
        if (key == "local.high-mp3-320") has_high = true;
    }
    CHECK(has_lossless);
    CHECK(has_high);

    BaseInfo lossless_base;
    lossless_base.platform = "local";
    lossless_base.version = "1.0.0";
    lossless_base.sha = imp_sha_of_file(lossless);
    lossless_base.name = "Example Song";
    lossless_base.author = "Example Artist";
    lossless_base.duration_seconds = 215;
    lossless_base.quality = quality_from_stream(imp_flac_1411());
    BaseInfo mp3_base = lossless_base;
    mp3_base.sha = imp_sha_of_file(compressed);
    mp3_base.quality = quality_from_stream(imp_mp3_320());
    CHECK(fs::exists(paths.meta_file(cache.path_for(lossless_base))));
    CHECK(fs::exists(paths.meta_file(cache.path_for(mp3_base))));
    CHECK_EQ(index.size(), size_t{2});
}

TEST_CASE(test_import_tag_parse_failure_falls_back_to_filename) {
    Temp tmp("tag_fallback");
    const fs::path source = tmp.src / "Fallback Artist - Fallback Title.flac";
    imp_write_file(source, "FALLBACK-CONTENT");

    LibraryPaths paths = tmp.paths();
    MetaCache cache(paths);
    LibraryIndex index(paths);
    ImportManager mgr(paths, cache, index);

    Stubs stubs;
    stubs.tags_fail = true;
    stubs.tags_error = ErrorCode::TagParseFailed;   // 1024
    stubs.install(mgr);

    const auto result = mgr.run(imp_import_request(source));
    CHECK(result.ok());
    if (!result.ok()) return;
    const json::Value& data = result.value();
    CHECK_EQ(data.get_int("imported"), int64_t{1});
    CHECK_EQ(data.get_int("failed"), int64_t{0});
    // 非致命：conflicts 里有 tag_parse 记录
    CHECK_EQ(imp_count_conflicts_of_type(data["conflicts"], "tag_parse"), size_t{1});

    BaseInfo probe;
    probe.platform = "local";
    probe.version = "1.0.0";
    probe.sha = imp_sha_of_file(source);
    probe.name = "Fallback Title";
    probe.author = "Fallback Artist";
    probe.duration_seconds = 215;
    probe.quality = quality_from_stream(imp_flac_1411());
    const auto stored = cache.read(cache.path_for(probe));
    CHECK(stored.ok());
    if (stored.ok()) {
        CHECK_EQ(stored.value().tag_source, std::string("filename"));
        CHECK_EQ(stored.value().name, std::string("Fallback Title"));
        CHECK_EQ(stored.value().author, std::string("Fallback Artist"));
    }
}

TEST_CASE(test_import_unsupported_extension_is_failed_1025) {
    Temp tmp("unsupported");
    imp_write_file(tmp.src / "notes.txt", "not audio");
    imp_write_file(tmp.src / "real.flac", "REAL-CONTENT");

    LibraryPaths paths = tmp.paths();
    MetaCache cache(paths);
    LibraryIndex index(paths);
    ImportManager mgr(paths, cache, index);
    Stubs stubs;
    stubs.install(mgr);

    const auto result = mgr.run(imp_import_request(tmp.src));
    CHECK(result.ok());
    if (!result.ok()) return;
    const json::Value& data = result.value();
    CHECK_EQ(data.get_int("scanned"), int64_t{2});
    CHECK_EQ(data.get_int("failed"), int64_t{1});
    CHECK_EQ(data.get_int("imported"), int64_t{1});

    bool found_1025 = false;
    for (const auto& item : data["items"].as_array()) {
        if (item.get_string("path").find("notes.txt") == std::string::npos) continue;
        found_1025 = true;
        CHECK_EQ(item.get_int("code"), int64_t{1025});
        CHECK_EQ(item.get_string("action"), std::string("failed"));
    }
    CHECK(found_1025);
    CHECK_EQ(cache.count(), size_t{1});
}

TEST_CASE(test_import_scan_and_dry_run_have_no_side_effects) {
    Temp tmp("dry_run");
    const fs::path source = tmp.src / "dry.flac";
    imp_write_file(source, "DRY-CONTENT");

    LibraryPaths paths = tmp.paths();
    MetaCache cache(paths);
    LibraryIndex index(paths);
    ImportManager mgr(paths, cache, index);
    Stubs stubs;
    stubs.tags.cover = {0xFF, 0xD8, 0xFF, 0xE0, 0x11};
    stubs.tags.lyrics_text = "[00:01.00]dry\n";
    stubs.install(mgr);

    const size_t files_before = imp_count_files(tmp.root);

    const auto scanned = mgr.scan(imp_import_request(source, "copy", "skip", true));
    CHECK(scanned.ok());
    if (scanned.ok()) {
        const json::Value& data = scanned.value();
        CHECK_EQ(data.get_bool("dry_run"), true);
        CHECK_EQ(data.get_int("scanned"), int64_t{1});
        CHECK_EQ(data.get_int("imported"), int64_t{1});
        CHECK_EQ(data["items"][size_t{0}].get_string("action"), std::string("would_create"));
    }
    CHECK_EQ(imp_count_files(tmp.root), files_before);

    const auto dry = mgr.run(imp_import_request(source, "copy", "skip", true));
    CHECK(dry.ok());
    if (dry.ok()) {
        const json::Value& data = dry.value();
        CHECK_EQ(data.get_int("imported"), int64_t{1});
        CHECK_EQ(data.get_int("failed"), int64_t{0});
        CHECK_EQ(data["items"][size_t{0}].get_string("action"), std::string("would_create"));
    }
    // 不写任何文件 / 不建目录
    CHECK_EQ(imp_count_files(tmp.root), files_before);
    CHECK_FALSE(fs::exists(paths.tracks()));
    CHECK_FALSE(fs::exists(paths.cache()));
    CHECK_FALSE(fs::exists(paths.covers()));
    CHECK_EQ(cache.count(), size_t{0});
    CHECK_EQ(index.size(), size_t{0});

    // 真正导入一次，验证与 dry_run 结果一致
    const auto real = mgr.run(imp_import_request(source));
    CHECK(real.ok());
    if (real.ok()) CHECK_EQ(real.value().get_int("imported"), int64_t{1});
    CHECK_EQ(cache.count(), size_t{1});
}

TEST_CASE(test_import_run_is_blocked_by_library_lock) {
    Temp tmp("locked");
    const fs::path source = tmp.src / "locked.flac";
    imp_write_file(source, "LOCKED-CONTENT");

    LibraryPaths paths = tmp.paths();
    MetaCache cache(paths);
    LibraryIndex index(paths);
    ImportManager mgr(paths, cache, index);
    Stubs stubs;
    stubs.install(mgr);

    LibraryLock held(paths);
    const Status acquired = held.acquire(100);
    CHECK(acquired.ok());
    if (!acquired.ok()) return;

    const auto blocked = mgr.run(imp_import_request(source));
    CHECK_FALSE(blocked.ok());
    if (!blocked.ok()) CHECK(blocked.status().code == ErrorCode::LibraryLocked);   // 1026

    // 未强行写入
    CHECK_EQ(cache.count(), size_t{0});
    CHECK_FALSE(fs::exists(paths.tracks()));
    CHECK_FALSE(fs::exists(paths.meta()));

    held.release();
    const auto after = mgr.run(imp_import_request(source));
    CHECK(after.ok());
    if (after.ok()) CHECK_EQ(after.value().get_int("imported"), int64_t{1});
}

TEST_CASE(test_import_status_and_report_unknown_id_is_1020) {
    Temp tmp("status_report");
    const fs::path source = tmp.src / "known.flac";
    imp_write_file(source, "KNOWN-CONTENT");

    LibraryPaths paths = tmp.paths();
    MetaCache cache(paths);
    LibraryIndex index(paths);
    ImportManager mgr(paths, cache, index);
    Stubs stubs;
    stubs.install(mgr);

    const auto unknown_status = mgr.status("imp_9999");
    CHECK_FALSE(unknown_status.ok());
    if (!unknown_status.ok()) CHECK(unknown_status.status().code == ErrorCode::TaskNotFound);   // 1020
    const auto unknown_report = mgr.report("imp_9999");
    CHECK_FALSE(unknown_report.ok());
    if (!unknown_report.ok()) CHECK(unknown_report.status().code == ErrorCode::TaskNotFound);

    // 空请求：无候选文件，全部计数为 0
    const auto empty = mgr.run(json::Value::object());
    CHECK(empty.ok());
    if (empty.ok()) {
        CHECK_EQ(empty.value().get_int("scanned"), int64_t{0});
        CHECK_EQ(empty.value().get_int("imported"), int64_t{0});
    }

    const auto result = mgr.run(imp_import_request(source));
    CHECK(result.ok());
    if (!result.ok()) return;
    const std::string id = result.value().get_string("import_id");
    CHECK_FALSE(id.empty());

    const auto st = mgr.status(id);
    CHECK(st.ok());
    if (st.ok()) {
        CHECK_EQ(st.value().get_string("import_id"), id);
        CHECK_EQ(st.value().get_int("scanned"), int64_t{1});
        CHECK_EQ(st.value().get_int("imported"), int64_t{1});
        CHECK_EQ(st.value().get_int("skipped"), int64_t{0});
        CHECK_EQ(st.value().get_int("failed"), int64_t{0});
    }
    const auto rep = mgr.report(id);
    CHECK(rep.ok());
    if (rep.ok()) {
        CHECK_EQ(rep.value().get_string("import_id"), id);
        CHECK_EQ(rep.value()["items"].size(), size_t{1});
        CHECK_EQ(rep.value()["items"][size_t{0}].get_string("path"), imp_shown_path(source));
        CHECK(rep.value()["conflicts"].is_array());
    }
    CHECK_EQ(mgr.reports().size(), size_t{2});   // 空请求 + 本次
}

TEST_CASE(test_import_move_mode_moves_source_and_link_mode_keeps_it) {
    // ---- move ----
    {
        Temp tmp("move_mode");
        const fs::path source = tmp.src / "movable.flac";
        imp_write_file(source, "MOVABLE-CONTENT");

        LibraryPaths paths = tmp.paths();
        MetaCache cache(paths);
        LibraryIndex index(paths);
        ImportManager mgr(paths, cache, index);
        Stubs stubs;
        stubs.install(mgr);

        // sha 桩按文件名参与计算，必须在移动前取到目标 sha
        const std::string sha = imp_sha_of_file(source);
        const auto result = mgr.run(imp_import_request(source, "move"));
        CHECK(result.ok());
        if (result.ok()) {
            CHECK_EQ(result.value().get_int("imported"), int64_t{1});
            CHECK_EQ(result.value()["items"][size_t{0}].get_string("action"), std::string("created"));
        }
        // 源不存在、目标存在
        CHECK_FALSE(fs::exists(source));
        CHECK(fs::exists(paths.track_dir(sha) / "Example Artist - Example Song.flac"));
        CHECK_EQ(cache.count(), size_t{1});
    }

    // ---- link ----
    {
        Temp tmp("link_mode");
        const fs::path source = tmp.src / "linked.flac";
        imp_write_file(source, "LINKED-CONTENT");

        LibraryPaths paths = tmp.paths();
        MetaCache cache(paths);
        LibraryIndex index(paths);
        ImportManager mgr(paths, cache, index);
        Stubs stubs;
        stubs.install(mgr);

        const auto result = mgr.run(imp_import_request(source, "link"));
        CHECK(result.ok());
        if (!result.ok()) return;
        CHECK_EQ(result.value().get_int("imported"), int64_t{1});

        // 源文件保留，tracks/ 下没有该 sha 的音频
        CHECK(fs::exists(source));
        const std::string sha = imp_sha_of_file(source);
        CHECK_FALSE(fs::exists(paths.track_dir(sha) / "Example Artist - Example Song.flac"));

        BaseInfo probe;
        probe.platform = "local";
        probe.version = "1.0.0";
        probe.sha = sha;
        probe.name = "Example Song";
        probe.author = "Example Artist";
        probe.duration_seconds = 215;
        probe.quality = quality_from_stream(imp_flac_1411());
        const auto stored = cache.read(cache.path_for(probe));
        CHECK(stored.ok());
        if (stored.ok()) {
            // link：location 是绝对路径
            CHECK(fs::path(stored.value().location).is_absolute());
            CHECK_EQ(stored.value().location.find("linked.flac") != std::string::npos, true);
            CHECK(fs::exists(fs::path(stored.value().location)));
        }
    }
}

TEST_CASE(test_import_options_json_round_trip) {
    json::Value req = json::Value::object();
    json::Value paths = json::Value::array();
    paths.push_back("D:/Music/Example Artist");
    req.set("paths", paths);
    req.set("recursive", false);
    req.set("mode", "move");
    req.set("conflict", "merge");
    req.set("tag_fallback", "filename");
    req.set("filename_template", "{title} - {artist}");
    req.set("dry_run", true);

    const ImportOptions opt = ImportOptions::from_json(req);
    CHECK_EQ(opt.paths.size(), size_t{1});
    CHECK_EQ(opt.paths[0], std::string("D:/Music/Example Artist"));
    CHECK_EQ(opt.recursive, false);
    CHECK_EQ(opt.mode, std::string("move"));
    CHECK(opt.conflict == ConflictPolicy::Merge);
    CHECK_EQ(opt.filename_template, std::string("{title} - {artist}"));
    CHECK_EQ(opt.dry_run, true);

    const json::Value dumped = opt.to_json();
    CHECK_EQ(dumped.get_string("mode"), std::string("move"));
    CHECK_EQ(dumped.get_string("conflict"), std::string("merge"));
    CHECK_EQ(dumped.get_bool("dry_run"), true);
    CHECK_EQ(dumped["paths"].size(), size_t{1});

    ImportConflict conflict;
    conflict.type = "duplicate_sha";
    conflict.sha = imp_fake_hex64("x");
    conflict.existing = "cache/meta/a/b/base.json";
    conflict.path = "D:/Music/x.flac";
    conflict.action = "skip";
    conflict.code = ErrorCode::Ok;
    CHECK_EQ(conflict.to_json().get_string("type"), std::string("duplicate_sha"));

    ImportItemResult item;
    item.path = "D:/Music/x.flac";
    item.action = "created";
    item.code = ErrorCode::Ok;
    CHECK_EQ(item.to_json().get_string("action"), std::string("created"));

    ImportReport report;
    report.import_id = "imp_0007";
    report.scanned = 3;
    report.conflicts.push_back(conflict);
    report.items.push_back(item);
    const json::Value report_json = report.to_json();
    CHECK_EQ(report_json.get_string("import_id"), std::string("imp_0007"));
    CHECK_EQ(report_json.get_int("scanned"), int64_t{3});
    CHECK_EQ(report_json["conflicts"].size(), size_t{1});
    CHECK_EQ(report_json["items"].size(), size_t{1});
}
