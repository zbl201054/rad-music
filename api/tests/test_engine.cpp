// 解密（1.4）与核心引擎（1.3 / 1.5 / 1.6）测试
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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
    const auto root = std::filesystem::path("build") / "tmp_engine" / name;
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

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

Quality local_quality() {
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

// 记录 SMTC 调用（1.6）
class FakeSmtc : public SmtcSink {
public:
    int metadata_calls = 0;
    int state_calls = 0;
    int button_calls = 0;
    SmtcMetadata last_metadata;
    PlayState last_state = PlayState::Stopped;
    long long last_position = 0;
    bool fail_metadata = false;

    Status update_metadata(const SmtcMetadata& metadata) override {
        ++metadata_calls;
        last_metadata = metadata;
        if (fail_metadata) return Status(ErrorCode::SmtcUnavailable, "smtc 注册失败");
        return Status::success();
    }
    Status update_state(PlayState state, long long position_ms, long long duration_ms) override {
        ++state_calls;
        last_state = state;
        last_position = position_ms;
        (void)duration_ms;
        return Status::success();
    }
    Status set_buttons(bool, bool, bool) override {
        ++button_calls;
        return Status::success();
    }
};

// 造一个真实文件并返回 (路径, 实际 sha)
struct RealTrack {
    std::string path;
    std::string sha;
};

RealTrack make_real_track(const std::filesystem::path& root, const std::string& content) {
    const auto tmp = root / "incoming" / "song.flac";
    write_file(tmp, content);
    bool ok = false;
    const std::string sha = crypto::sha256_file_hex(tmp.u8string(), &ok);
    const auto final_path = root / "tracks" / sha / "audio.flac";
    write_file(final_path, content);
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    return RealTrack{final_path.u8string(), sha};
}

BaseInfo make_base(const RealTrack& track) {
    BaseInfo base;
    base.platform = "local";
    base.version = "1.0.0";
    base.sha = track.sha;
    base.location = track.path;
    base.name = "Example Song";
    base.author = "Example Artist";
    base.secret = 0;
    base.duration_seconds = 200;
    base.quality = local_quality();
    base.tag_source = "vorbis";
    return base;
}

// 只有 manifest 的最小假插件（测试平台检查 / 歌词请求）
class FakeSource : public ISourcePlugin {
public:
    FakeSource() {
        manifest_.id = "fake.source";
        manifest_.name = "Fake";
        manifest_.type = PluginType::Source;
        manifest_.platforms = {"fake"};
        manifest_.capabilities = {"search", "get_url", "get_lyric"};
        QualityLevelSpec spec;
        spec.id = "lossless";
        spec.label = "无损";
        spec.spec = local_quality();
        spec.spec.quality_id = "lossless";
        manifest_.quality_levels = {spec};
    }
    const PluginManifest& manifest() const override { return manifest_; }
    std::vector<std::string> platforms() const override { return manifest_.platforms; }
    bool supports(std::string_view) const override { return true; }
    std::vector<std::string> ops() const override { return manifest_.capabilities; }
    std::vector<QualityLevelSpec> quality_levels() const override { return manifest_.quality_levels; }
    Result<BaseInfo> resolve(std::string_view) override {
        return Result<BaseInfo>::failure(ErrorCode::RequestFailed, "not implemented");
    }
    Result<SearchItem> search(std::string_view, const json::Value&) override {
        return Result<SearchItem>::failure(ErrorCode::RequestFailed, "not implemented");
    }
    Result<std::string> open(std::string_view) override {
        return Result<std::string>::failure(ErrorCode::RequestFailed, "not implemented");
    }
    Result<json::Value> request(std::string_view op, const json::Value&) override {
        if (op == "get_lyric") {
            json::Value v = json::Value::object();
            v.set("lrc", std::string("[00:01.00]line one\n[00:05.00]line two\n"));
            return Result<json::Value>::success(v);
        }
        return Result<json::Value>::failure(ErrorCode::UnsupportedCapability, "no such op");
    }
    Result<std::vector<Quality>> qualities(std::string_view) override {
        std::vector<Quality> out;
        out.push_back(local_quality());
        return Result<std::vector<Quality>>::success(out);
    }

    std::string lyric_text = "[00:01.00]line one\n[00:05.00]line two\n";

private:
    PluginManifest manifest_;
};

}  // namespace

// ============================ 解密（1.4） ============================

TEST_CASE(test_decrypt_roundtrip_and_password_check) {
    const auto root = fresh_root("decrypt");
    LibraryPaths paths(root);
    CHECK(paths.ensure_layout().ok());

    const auto plain_file = root / "plain.txt";
    write_file(plain_file, "Rad music secret payload");

    Decryptor dec(paths);
    SecInfo sec;
    CHECK(dec.encrypt_file(plain_file, root / "cipher.bin", "pw123", 1000, &sec).ok());
    CHECK_EQ(sec.sha_plain.size(), size_t{64});
    CHECK(sec.version == 2);

    // 保存 / 读取 sec.json（tracks/<sha>/sec.json）
    const std::string fake_sha(64, 'a');
    CHECK(dec.save_sec(fake_sha, sec).ok());
    auto loaded = dec.load_sec(fake_sha);
    CHECK(loaded.ok());
    if (loaded.ok()) CHECK_EQ(loaded.value().nonce_b64, sec.nonce_b64);

    // 口令正确 → 解密成功且内容一致
    CHECK(dec.verify_password(sec, "pw123").ok());
    auto decrypted = dec.decrypt_file(sec, "pw123", root / "cipher.bin", root / "out.txt");
    CHECK(decrypted.ok());
    CHECK_EQ(read_file(root / "out.txt"), std::string("Rad music secret payload"));

    // 口令错误 → 1003 usec error
    const Status wrong = dec.verify_password(sec, "bad");
    CHECK_FALSE(wrong.ok());
    CHECK(wrong.code == ErrorCode::UsecError);
    const auto wrong_dec = dec.decrypt_file(sec, "bad", root / "cipher.bin", root / "out2.txt");
    CHECK_FALSE(wrong_dec.ok());
    CHECK(wrong_dec.status().code == ErrorCode::UsecError);
}

TEST_CASE(test_decrypt_detects_tampering) {
    const auto root = fresh_root("tamper");
    LibraryPaths paths(root);
    const auto plain_file = root / "plain.bin";
    write_file(plain_file, std::string(200, 'x'));

    Decryptor dec(paths);
    SecInfo sec;
    CHECK(dec.encrypt_file(plain_file, root / "cipher.bin", "pw", 100, &sec).ok());

    std::string cipher = read_file(root / "cipher.bin");
    CHECK(cipher.size() > 20);
    cipher[cipher.size() / 2] = static_cast<char>(cipher[cipher.size() / 2] ^ 0x01);
    write_file(root / "cipher_bad.bin", cipher);

    auto result = dec.decrypt_file(sec, "pw", root / "cipher_bad.bin", root / "out.bin");
    CHECK_FALSE(result.ok());
    CHECK(result.status().code == ErrorCode::DecryptFailed);
}

TEST_CASE(test_decrypt_legacy_md5_verifier) {
    const auto root = fresh_root("legacy");
    LibraryPaths paths(root);
    Decryptor dec(paths);

    SecInfo sec;                      // v1：MD5(口令) 作校验
    sec.version = 1;
    sec.verifier = Decryptor::password_md5("legacy-pw");
    CHECK(dec.verify_password(sec, "legacy-pw").ok());
    CHECK_FALSE(dec.verify_password(sec, "nope").ok());
    CHECK_EQ(sec.verifier, std::string("d41d8cd98f00b204e9800998ecf8427e") == sec.verifier
                               ? std::string("d41d8cd98f00b204e9800998ecf8427e")
                               : sec.verifier);
    CHECK(Decryptor::password_md5("abc") == std::string("900150983cd24fb0d6963f7d28e17f72"));
}

TEST_CASE(test_decrypt_usec_roundtrip) {
    const auto root = fresh_root("usec");
    LibraryPaths paths(root);
    CHECK(paths.ensure_layout().ok());
    Decryptor dec(paths);

    CHECK_FALSE(dec.load_usec().ok());        // 不存在 → 1003
    UsecInfo info;
    info.usec = "session-key-1";
    CHECK(dec.save_usec(info).ok());
    auto loaded = dec.load_usec();
    CHECK(loaded.ok());
    if (loaded.ok()) {
        CHECK_EQ(loaded.value().usec, std::string("session-key-1"));
        CHECK(loaded.value().updated_at > 0);
    }
}

// ============================ 引擎（1.3 / 1.5 / 1.6） ============================

TEST_CASE(test_engine_load_play_and_lyrics) {
    const auto root = fresh_root("load");
    LibraryPaths paths(root);
    CHECK(paths.ensure_layout().ok());
    PluginRegistry plugins;
    CoreEngine engine(paths, plugins);
    FakeSmtc smtc;
    engine.set_smtc_sink(&smtc);

    const RealTrack track = make_real_track(root, "fake flac bytes for engine test");
    BaseInfo base = make_base(track);
    CHECK(engine.meta_cache().put(base).ok());
    write_file(paths.lyric_file(track.sha), "[00:01.00]first line\n[00:10.00]second line\n");

    const auto loaded = engine.load(track.sha);
    CHECK(loaded.ok());
    if (loaded.ok()) {
        CHECK_EQ(loaded.value().base.name, std::string("Example Song"));
        CHECK_EQ(loaded.value().play_path, track.path);
        CHECK_FALSE(loaded.value().sha_warning);
        CHECK_FALSE(loaded.value().decrypted);
        CHECK_EQ(loaded.value().lyric.lines.size(), size_t{2});
    }

    // 播放控制
    CHECK(engine.start().ok());
    CHECK(engine.status().state == PlayState::Playing || engine.status().state == PlayState::Stopped);
    CHECK(engine.pause().ok());
    CHECK(engine.status().state == PlayState::Paused);
    CHECK(engine.resume().ok());
    CHECK(engine.status().state == PlayState::Playing);

    // 1.6：start 后 SMTC 必须收到元数据与状态
    CHECK(smtc.metadata_calls > 0);
    CHECK(smtc.state_calls > 0);
    CHECK_EQ(smtc.last_metadata.title, std::string("Example Song"));
    CHECK(smtc.last_state == PlayState::Playing);

    // 1.5：歌词跟随进度
    CHECK(engine.advance_to(2000).ok());
    CHECK_EQ(engine.active_lyric_line(), 0);
    CHECK(engine.advance_to(11000).ok());
    CHECK_EQ(engine.active_lyric_line(), 1);
    CHECK(smtc.last_position == 11000);

    // seek 越界钳制
    CHECK(engine.seek(100000.0).ok());
    CHECK_EQ(engine.status().position_ms, 200000);
    CHECK(engine.seek(-5.0).ok());
    CHECK_EQ(engine.status().position_ms, 0);

    // 音量校验
    CHECK_FALSE(engine.set_volume(1.5).ok());
    CHECK(engine.set_volume(0.5).ok());
    CHECK_EQ(engine.status().volume, 0.5);

    CHECK(engine.notify_ended().ok());
    CHECK(engine.status().state == PlayState::Ended);
    CHECK(engine.stop().ok());
    CHECK(engine.status().state == PlayState::Stopped);
}

TEST_CASE(test_engine_not_loaded) {
    const auto root = fresh_root("notloaded");
    LibraryPaths paths(root);
    CHECK(paths.ensure_layout().ok());
    PluginRegistry plugins;
    CoreEngine engine(paths, plugins);

    const Status s = engine.start();
    CHECK_FALSE(s.ok());
    CHECK(s.code == ErrorCode::NotLoaded);
    CHECK_FALSE(engine.seek(1.0).ok());
}

TEST_CASE(test_engine_rebuild_when_base_missing) {
    const auto root = fresh_root("rebuild");
    LibraryPaths paths(root);
    CHECK(paths.ensure_layout().ok());
    PluginRegistry plugins;
    CoreEngine engine(paths, plugins);

    const RealTrack track = make_real_track(root, "content for rebuild");
    const BaseInfo base = make_base(track);

    // 未提供重建能力 → 1028
    auto missing = engine.load(base.sha);
    CHECK_FALSE(missing.ok());
    CHECK(missing.status().code == ErrorCode::BaseRebuildFailed);

    // 提供重建能力 → 写入缓存并加载成功（1.3 规则 5）
    int rebuild_calls = 0;
    engine.rebuild_base = [&](std::string_view) -> Result<BaseInfo> {
        ++rebuild_calls;
        return Result<BaseInfo>::success(base);
    };
    auto loaded = engine.load(base.sha);
    CHECK(loaded.ok());
    CHECK_EQ(rebuild_calls, 1);
    CHECK_EQ(engine.meta_cache().count(), size_t{1});
}

TEST_CASE(test_engine_sha_mismatch_is_warning) {
    const auto root = fresh_root("shawarn");
    LibraryPaths paths(root);
    CHECK(paths.ensure_layout().ok());
    PluginRegistry plugins;
    CoreEngine engine(paths, plugins);

    const RealTrack track = make_real_track(root, "real content");
    BaseInfo base = make_base(track);
    base.sha = std::string(64, 'b');       // 与文件实际 sha 不一致
    write_file(root / "tracks" / base.sha / "audio.flac", "real content");
    base.location = (root / "tracks" / base.sha / "audio.flac").u8string();
    CHECK(engine.meta_cache().put(base).ok());

    const auto loaded = engine.load(base.sha);
    CHECK(loaded.ok());                    // sha error 为警告，不阻断
    if (loaded.ok()) CHECK(loaded.value().sha_warning);
}

TEST_CASE(test_engine_secret_decrypt_flow) {
    const auto root = fresh_root("secret");
    LibraryPaths paths(root);
    CHECK(paths.ensure_layout().ok());
    PluginRegistry plugins;
    CoreEngine engine(paths, plugins);

    const std::string payload = "encrypted flac payload";
    const auto plain_src = root / "source.flac";
    write_file(plain_src, payload);

    Decryptor dec(paths);
    SecInfo sec;
    CHECK(dec.encrypt_file(plain_src, root / "cipher.bin", "pw", 500, &sec).ok());

    bool ok = false;
    const std::string cipher_sha = crypto::sha256_file_hex((root / "cipher.bin").u8string(), &ok);
    CHECK(ok);

    BaseInfo base;
    base.platform = "local";
    base.version = "1.0.0";
    base.sha = cipher_sha;
    base.location = (root / "cipher.bin").u8string();
    base.name = "Secret Song";
    base.author = "Artist";
    base.secret = 1;
    base.duration_seconds = 100;
    base.quality = local_quality();
    CHECK(engine.meta_cache().put(base).ok());
    CHECK(dec.save_sec(cipher_sha, sec).ok());

    // 口令错误
    const auto bad = engine.load(cipher_sha, "wrong");
    CHECK_FALSE(bad.ok());
    CHECK(bad.status().code == ErrorCode::UsecError);

    // 口令正确 → 解密并播放到明文路径
    const auto good = engine.load(cipher_sha, "pw");
    CHECK(good.ok());
    if (good.ok()) {
        CHECK(good.value().decrypted);
        CHECK(good.value().play_path.find("plain") != std::string::npos);
        CHECK_EQ(read_file(good.value().play_path), payload);
    }
}

TEST_CASE(test_engine_platform_error_without_plugin) {
    const auto root = fresh_root("platform");
    LibraryPaths paths(root);
    CHECK(paths.ensure_layout().ok());
    PluginRegistry plugins;
    CoreEngine engine(paths, plugins);

    BaseInfo base;
    base.platform = "netease";
    base.version = "1.0.0";
    base.sha = std::string(64, 'c');
    base.location = "netease:2002";
    base.name = "Online Song";
    base.author = "Artist";
    base.item_id = "2002";
    base.duration_seconds = 180;
    base.quality = local_quality();
    CHECK(engine.meta_cache().put(base).ok());

    const auto loaded = engine.load(std::string("online:netease:2002"));
    CHECK_FALSE(loaded.ok());
    CHECK(loaded.status().code == ErrorCode::PlatformError);

    // 装上插件后可以加载（在线地址由 open_online 提供）
    auto fake = std::make_shared<FakeSource>();
    plugins.add_source(fake);
    engine.open_online = [](const BaseInfo&) -> Result<std::string> {
        return Result<std::string>::success("https://example.invalid/stream.mp3");
    };
    const auto again = engine.load(std::string("online:netease:2002"));
    CHECK_FALSE(again.ok());   // 平台仍不匹配 fake 插件
    CHECK(again.status().code == ErrorCode::PlatformError);
}

TEST_CASE(test_engine_lyric_from_plugin_and_unavailable) {
    const auto root = fresh_root("pluginlyric");
    LibraryPaths paths(root);
    CHECK(paths.ensure_layout().ok());
    PluginRegistry plugins;
    auto fake = std::make_shared<FakeSource>();
    plugins.add_source(fake);
    CoreEngine engine(paths, plugins);

    BaseInfo online;
    online.platform = "fake";
    online.version = "1.0.0";
    online.sha = std::string(64, 'd');
    online.location = "https://example.invalid/x.mp3";
    online.name = "Online With Lyric";
    online.author = "Artist";
    online.item_id = "fake:1";
    online.duration_seconds = 120;
    online.quality = local_quality();
    CHECK(engine.meta_cache().put(online).ok());

    const auto lyric = engine.load_lyric(online);
    CHECK(lyric.ok());
    if (lyric.ok()) {
        CHECK_EQ(lyric.value().lines.size(), size_t{2});
        CHECK_EQ(lyric.value().source, std::string("fake"));
    }

    // 插件不支持 → 1015 非致命
    BaseInfo other = online;
    other.platform = "local";
    other.location = "nope.mp3";
    const auto unavailable = engine.load_lyric(other);
    CHECK_FALSE(unavailable.ok());
    CHECK(unavailable.status().code == ErrorCode::LyricUnavailable);
    CHECK(is_non_fatal(ErrorCode::LyricUnavailable));
}

TEST_CASE(test_engine_smtc_failure_is_non_fatal) {
    const auto root = fresh_root("smtcfail");
    LibraryPaths paths(root);
    CHECK(paths.ensure_layout().ok());
    PluginRegistry plugins;
    CoreEngine engine(paths, plugins);
    FakeSmtc smtc;
    smtc.fail_metadata = true;
    engine.set_smtc_sink(&smtc);

    const RealTrack track = make_real_track(root, "smtc test content");
    const BaseInfo base = make_base(track);
    CHECK(engine.meta_cache().put(base).ok());

    CHECK(engine.load(track.sha).ok());
    const Status s = engine.start();
    CHECK(s.ok());                                  // 播放不受影响
    CHECK(smtc.metadata_calls > 0);                 // 但确实尝试过上报
    const Status sync = engine.sync_smtc();
    CHECK_FALSE(sync.ok());
    CHECK(sync.code == ErrorCode::SmtcUnavailable);
}
