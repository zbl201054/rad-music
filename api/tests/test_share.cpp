// 歌单分享 / 曲库导出导入测试（share.hpp）
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "radmusic/archive.hpp"
#include "radmusic/collections.hpp"
#include "radmusic/model.hpp"
#include "radmusic/quality.hpp"
#include "radmusic/share.hpp"
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

void write_bytes(const std::filesystem::path& file, size_t size, unsigned char seed) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    for (size_t i = 0; i < size; ++i) {
        out.put(static_cast<char>((seed + i * 31) & 0xFF));
    }
    out.flush();
}

std::string read_bytes(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return data;
}

// 造一条带真实音频文件的曲库条目（location 指向 tracks/<sha>/audio.flac）
BaseInfo mk_local_track(const LibraryPaths& paths, const std::string& title, const std::string& artist,
                        int duration, char seed, size_t file_size) {
    BaseInfo b;
    b.platform = "local";
    b.version = "1.0.0";
    b.sha = fake_sha(seed);
    b.location = "tracks/" + b.sha + "/audio.flac";
    b.name = title;
    b.author = artist;
    b.album = "测试专辑";
    b.duration_seconds = duration;
    b.quality.level = QualityLevel::Lossless;
    b.quality.codec = "flac";
    b.quality.lossless = true;
    b.quality.sample_rate = 44100;
    b.quality.bit_depth = 16;
    b.quality.channels = 2;
    b.tag_source = "vorbis";
    write_bytes(paths.root / b.location, file_size, static_cast<unsigned char>(seed));
    return b;
}

}  // namespace

TEST_CASE(test_share_safe_filename) {
    CHECK_EQ(share_safe_filename("晴天"), std::string("晴天"));
    // Windows 非法字符 → '_'
    CHECK_EQ(share_safe_filename("a/b\\c:d*e?f\"g<h>i|j"), std::string("a_b_c_d_e_f_g_h_i_j"));
    // 首尾空白与结尾点被去掉
    CHECK_EQ(share_safe_filename("  name...  "), std::string("name"));
    // 空 / 全非法 → 回落名
    CHECK_EQ(share_safe_filename(""), std::string("radmusic"));
    CHECK_EQ(share_safe_filename("   "), std::string("radmusic"));
    CHECK_EQ(share_safe_filename("..", std::string("x")), std::string("x"));
    // 长名字截断到 ≤80 字节，且不切坏 UTF-8
    const std::string long_name = std::string(40, 'a') + std::string(60, '\xe6') + "x";
    const std::string cut = share_safe_filename(long_name);
    CHECK(cut.size() <= 80);
    for (size_t i = 0; i < cut.size(); ++i) {
        if ((static_cast<unsigned char>(cut[i]) & 0xC0) != 0x80) continue;
        // 续字节前面必须还有起始字节，简单校验不会以续字节开头
        CHECK(i > 0);
    }
    CHECK_FALSE(cut.empty());
    CHECK_EQ(share_kind_of_filename("a.RDL"), std::string("playlist"));
    CHECK_EQ(share_kind_of_filename("b.rdm"), std::string("package"));
    CHECK_EQ(share_kind_of_filename("c.flac"), std::string());
}

TEST_CASE(test_share_rdl_roundtrip) {
    Playlist playlist;
    playlist.name = "通勤路上";
    playlist.description = "开车听的";
    playlist.created_at = 1700000000;
    playlist.updated_at = 1700000100;

    PlaylistTrack a;
    a.platform = "local";
    a.item_id = fake_sha(1);
    a.sha = a.item_id;
    a.name = "晴天";
    a.author = "周杰伦";
    a.duration_seconds = 269;
    a.added_at = 1700000001;
    PlaylistTrack b;
    b.platform = "netease";
    b.item_id = "123456";
    b.name = "夜曲";
    b.author = "周杰伦";
    b.duration_seconds = 226;
    playlist.tracks = {a, b};

    const json::Value rdl = share_rdl_from_playlist(playlist);
    CHECK_EQ(rdl.get_string("format"), std::string(kSharePlaylistFormat));
    CHECK_EQ(rdl.get_int("version"), int64_t{1});
    CHECK_EQ(rdl.get_int("track_count"), int64_t{2});
    CHECK_EQ(rdl.get_string("name"), std::string("通勤路上"));
    CHECK_FALSE(rdl.get_string("generator").empty());

    // 真实往返：dump → parse → 还原
    const std::string text = rdl.dump(2);
    json::ParseError parse_error;
    const json::Value parsed = json::Value::parse(text, &parse_error);
    CHECK(parsed.is_object());
    auto back = share_playlist_from_rdl(parsed);
    CHECK(back.ok());
    CHECK_EQ(back.value().name, std::string("通勤路上"));
    CHECK_EQ(back.value().description, std::string("开车听的"));
    CHECK_EQ(back.value().tracks.size(), size_t{2});
    CHECK_EQ(back.value().tracks[0].name, std::string("晴天"));
    CHECK_EQ(back.value().tracks[0].item_id, a.item_id);
    CHECK_EQ(back.value().tracks[1].platform, std::string("netease"));
    CHECK_EQ(back.value().tracks[1].item_id, std::string("123456"));
    // 导入进来的歌单一律是本地的（不再跟在线源同步）
    CHECK_EQ(back.value().platform, std::string("local"));
}

TEST_CASE(test_share_rdl_rejects_bad_input) {
    CHECK_FALSE(share_playlist_from_rdl(json::Value::array()).ok());
    CHECK_FALSE(share_playlist_from_rdl(json::Value("text")).ok());

    json::Value wrong_format = json::Value::object();
    wrong_format.set("format", "someone.else");
    wrong_format.set("tracks", json::Value::array());
    CHECK_FALSE(share_playlist_from_rdl(wrong_format).ok());

    json::Value no_tracks = json::Value::object();
    no_tracks.set("name", "空歌单");
    CHECK_FALSE(share_playlist_from_rdl(no_tracks).ok());

    json::Value empty_tracks = json::Value::object();
    empty_tracks.set("tracks", json::Value::array());
    CHECK_FALSE(share_playlist_from_rdl(empty_tracks).ok());

    // 缺 item_id/sha 的曲目会被丢掉；全丢光 → 报错
    json::Value bad_track = json::Value::object();
    json::Value tracks = json::Value::array();
    json::Value item = json::Value::object();
    item.set("name", "只有歌名");
    tracks.push_back(item);
    bad_track.set("tracks", tracks);
    CHECK_FALSE(share_playlist_from_rdl(bad_track).ok());

    // 未来版本：明确拒绝，而不是按旧格式猜
    json::Value future = json::Value::object();
    future.set("version", static_cast<long long>(kShareFormatVersion + 1));
    future.set("tracks", json::Value::array());
    const auto result = share_playlist_from_rdl(future);
    CHECK_FALSE(result.ok());
    CHECK_EQ(result.status().code, ErrorCode::InvalidMetadata);
}

TEST_CASE(test_share_zip_write_and_extract) {
    const auto root = fresh_root("share_zip");
    const std::string text_payload = "{ \"format\": \"radmusic.package\", \"tracks\": [] }";
    const auto audio = root / "input.raw";
    write_bytes(audio, 200 * 1024 + 7, 0x5A);

    std::vector<archive::ZipWriteItem> items;
    archive::ZipWriteItem head;
    head.name = "base.json";
    head.data = text_payload;
    items.push_back(head);
    archive::ZipWriteItem song;
    song.name = "music/001 - 晴天 - 周杰伦.flac";   // 中文 + 空格 + 连字符：检验 UTF-8 名字位
    song.path = audio;
    items.push_back(song);

    const auto zip = root / "pack.rdm";
    std::string error;
    CHECK(archive::write_zip(zip, items, &error));
    CHECK(error.empty());
    CHECK(std::filesystem::file_size(zip) > 200 * 1024);

    // 1) 内存版解压（更新包用的那条路径）也要能读我们自己写的包
    const auto dir_a = root / "out_memory";
    const auto result_a = archive::extract_zip(zip, dir_a);
    CHECK(result_a.ok());
    CHECK_EQ(result_a.files, size_t{2});
    CHECK_EQ(read_bytes(dir_a / "base.json"), text_payload);
    CHECK_EQ(read_bytes(dir_a / "music" / "001 - 晴天 - 周杰伦.flac"), read_bytes(audio));

    // 2) 流式解压（.rdm 导入用的那条路径）
    const auto dir_b = root / "out_stream";
    const auto result_b = archive::extract_zip_streamed(zip, dir_b);
    CHECK(result_b.ok());
    CHECK_EQ(result_b.files, size_t{2});
    CHECK_EQ(read_bytes(dir_b / "base.json"), text_payload);
    CHECK_EQ(read_bytes(dir_b / "music" / "001 - 晴天 - 周杰伦.flac"), read_bytes(audio));

    // 3) 损坏的归档：截断后必须失败而不是写出半截文件
    std::string broken = read_bytes(zip);
    broken.resize(broken.size() / 2);
    const auto broken_path = root / "broken.rdm";
    {
        std::ofstream out(broken_path, std::ios::binary | std::ios::trunc);
        out.write(broken.data(), static_cast<std::streamsize>(broken.size()));
    }
    CHECK_FALSE(archive::extract_zip_streamed(broken_path, root / "out_broken").ok());

    // 4) 非法条目名被拒绝
    std::vector<archive::ZipWriteItem> bad_items;
    archive::ZipWriteItem escape;
    escape.name = "../escape.txt";
    escape.data = "x";
    bad_items.push_back(escape);
    std::string bad_error;
    CHECK_FALSE(archive::write_zip(root / "bad.rdm", bad_items, &bad_error));
    CHECK_FALSE(bad_error.empty());
}

TEST_CASE(test_share_export_folder_and_package) {
    const auto root = fresh_root("share_export");
    LibraryPaths paths(root);
    CHECK(paths.ensure_layout().ok());
    MetaCache meta(paths);

    const BaseInfo a = mk_local_track(paths, "晴天", "周杰伦", 269, 1, 4096);
    const BaseInfo b = mk_local_track(paths, "夜曲", "周杰伦", 226, 2, 8192);
    CHECK(meta.put(a).ok());
    CHECK(meta.put(b).ok());

    // 在线曲目（无本地文件）→ 导出时应被跳过并给出原因
    BaseInfo online;
    online.platform = "netease";
    online.version = "1.0.0";
    online.sha = fake_sha(3);
    online.location = "https://example.com/never.mp3";
    online.name = "在线歌";
    online.author = "某歌手";
    CHECK(meta.put(online).ok());

    const std::vector<std::string> shas = {a.sha, b.sha, online.sha};

    // 1) 常规文件形式
    ShareExportOptions folder_options;
    folder_options.name = "测试导出";
    auto folder = share_export_tracks(paths, meta, shas, folder_options);
    CHECK(folder.ok());
    CHECK_EQ(folder.value().count, 2);
    CHECK_EQ(folder.value().skipped.size(), size_t{1});
    CHECK(folder.value().file.empty());   // 目录模式没有单文件产物
    CHECK(std::filesystem::is_directory(folder.value().dir));
    CHECK_EQ(folder.value().files.size(), size_t{2});
    for (const auto& file : folder.value().files) {
        CHECK(std::filesystem::is_regular_file(file));
        CHECK_EQ(std::filesystem::path(file).extension().u8string(), std::string(".flac"));
    }
    CHECK(folder.value().dir.find("exports") != std::string::npos);

    // 2) 音频包（.rdm）
    ShareExportOptions package_options;
    package_options.name = "测试导出";
    package_options.as_package = true;
    package_options.playlist_name = "测试导出";
    package_options.playlist_description = "两首歌";
    auto package = share_export_tracks(paths, meta, shas, package_options);
    CHECK(package.ok());
    CHECK_EQ(package.value().count, 2);
    CHECK(std::filesystem::is_regular_file(package.value().file));
    CHECK_EQ(std::filesystem::path(package.value().file).extension().u8string(), std::string(".rdm"));
    // 同名不覆盖：文件夹导出已占用「测试导出」，包名自动加后缀，文件仍应存在
    CHECK_EQ(package.value().files.size(), size_t{1});

    // 3) 打开音频包：清单可读、曲目数与歌单信息正确
    auto opened = share_open_package(paths, read_bytes(package.value().file));
    CHECK(opened.ok());
    if (opened.ok()) {
        CHECK_EQ(opened.value().declared_tracks, 2);
        CHECK_EQ(opened.value().playlist_name, std::string("测试导出"));
        CHECK(std::filesystem::is_directory(opened.value().audio_dir));
        const json::Value* tracks = opened.value().manifest.find("tracks");
        CHECK(tracks != nullptr && tracks->is_array());
        if (tracks != nullptr) {
            for (const auto& entry : tracks->as_array()) {
                const std::string file = entry.get_string("file");
                CHECK(file.rfind("music/", 0) == 0);
                CHECK(std::filesystem::is_regular_file(opened.value().dir / "content" / file));
                CHECK_FALSE(entry.get_string("sha").empty());
            }
        }
        share_discard_package(opened.value());
        CHECK_FALSE(std::filesystem::exists(opened.value().dir));
    }

    // 4) 空输入 / 全是在线曲目 → 失败（而不是产出空包）
    ShareExportOptions empty_options;
    CHECK_FALSE(share_export_tracks(paths, meta, {}, empty_options).ok());
    CHECK_FALSE(share_export_tracks(paths, meta, {online.sha}, empty_options).ok());
    CHECK_FALSE(share_export_tracks(paths, meta, {fake_sha(9)}, empty_options).ok());
}
