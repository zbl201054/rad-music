// 下载管理测试（Readme.md 1.8 / 2.10）
//
// 覆盖：create 参数与音质校验（1007/1027/1021）、同曲同质去重、step 成功流程（.part → tracks/ →
//      base.json → index）、校验失败 1022 保留 .part 并可续传、pause/resume/cancel 状态机、
//      remove 仅删 tracks/ 下文件、save/load 回环（running → paused）、默认本地文件 fetch。
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "radmusic/crypto.hpp"
#include "radmusic/download.hpp"
#include "radmusic/plugin.hpp"
#include "radmusic/quality.hpp"
#include "radmusic/storage.hpp"
#include "test_framework.hpp"

using namespace radmusic;

namespace {

std::filesystem::path fresh_root(const std::string& name) {
    const auto root = std::filesystem::path("build") / "tmp_download" / name;
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

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string path_to_utf8(const std::filesystem::path& p) {
    const auto u8 = p.u8string();
    return u8.empty() ? std::string() : std::string(reinterpret_cast<const char*>(u8.c_str()));
}

// 64 位小写 hex（内容无关，用于制造校验不一致）
std::string fake_sha(char seed) {
    std::string s(64, '0');
    for (size_t i = 0; i < 64; ++i) s[i] = static_cast<char>('a' + ((seed + static_cast<char>(i)) % 6));
    return s;
}

// 测试标签一律用 ASCII，避免 Windows 窄字符路径编码干扰断言
std::vector<QualityLevelSpec> radish_specs() {
    std::vector<QualityLevelSpec> specs;
    QualityLevelSpec lossless;
    lossless.id = "lossless";
    lossless.label = "Lossless";
    lossless.spec.level = QualityLevel::Lossless;
    lossless.spec.codec = "flac";
    lossless.spec.lossless = true;
    lossless.spec.bitrate = 1411;
    lossless.spec.sample_rate = 44100;
    lossless.spec.bit_depth = 16;
    specs.push_back(lossless);

    QualityLevelSpec exhigh;
    exhigh.id = "exhigh";
    exhigh.label = "Extreme";
    exhigh.spec.level = QualityLevel::High;
    exhigh.spec.codec = "mp3";
    exhigh.spec.lossless = false;
    exhigh.spec.bitrate = 320;
    specs.push_back(exhigh);
    return specs;
}

Quality spec_quality(const std::string& id) {
    for (const auto& spec : radish_specs()) {
        if (spec.id == id) {
            Quality q = spec.spec;
            q.quality_id = spec.id;
            q.label = spec.label;
            return q;
        }
    }
    return Quality{};
}

PluginManifest radish_manifest() {
    PluginManifest manifest;
    manifest.id = "fake_radish";
    manifest.name = "Fake Radish";
    manifest.type = PluginType::Source;
    manifest.platforms = {"radish"};
    manifest.capabilities = {"search", "get_song", "get_url", "get_qualities"};
    manifest.quality_levels = radish_specs();
    return manifest;
}

class FakeSourcePlugin : public ISourcePlugin {
public:
    FakeSourcePlugin() : manifest_(radish_manifest()) {}

    const PluginManifest& manifest() const override { return manifest_; }
    std::vector<std::string> platforms() const override { return manifest_.platforms; }
    bool supports(std::string_view) const override { return true; }
    std::vector<std::string> ops() const override { return manifest_.capabilities; }
    std::vector<QualityLevelSpec> quality_levels() const override { return manifest_.quality_levels; }

    Result<BaseInfo> resolve(std::string_view) override {
        if (base.name.empty()) {
            return Result<BaseInfo>::failure(ErrorCode::RequestFailed, "resolve 未设置");
        }
        return Result<BaseInfo>::success(base);
    }
    Result<SearchItem> search(std::string_view, const json::Value&) override {
        return Result<SearchItem>::failure(ErrorCode::UnsupportedCapability, "search 未实现");
    }
    Result<std::string> open(std::string_view) override {
        if (url.empty()) return Result<std::string>::failure(ErrorCode::RequestFailed, "open 未设置");
        return Result<std::string>::success(url);
    }
    Result<json::Value> request(std::string_view op, const json::Value&) override {
        return Result<json::Value>::failure(ErrorCode::RequestFailed,
                                            "request 未实现: " + std::string(op));
    }
    Result<std::vector<Quality>> qualities(std::string_view) override {
        std::vector<Quality> out;
        for (const auto& spec : manifest_.quality_levels) out.push_back(spec.spec);
        return Result<std::vector<Quality>>::success(out);
    }

    BaseInfo base;
    std::string url;

private:
    PluginManifest manifest_;
};

struct Fixture {
    std::filesystem::path root;
    LibraryPaths paths;
    MetaCache meta;
    LibraryIndex index;
    PluginRegistry registry;
    std::shared_ptr<FakeSourcePlugin> plugin;
    DownloadManager manager;

    explicit Fixture(const std::string& name)
        : root(fresh_root(name)),
          paths(root),
          meta(paths),
          index(paths),
          registry(),
          manager(paths, meta, index, registry) {
        plugin = std::make_shared<FakeSourcePlugin>();
        registry.add_source(plugin);
        paths.ensure_layout();
    }
};

}  // namespace

TEST_CASE(test_download_create_validates_arguments) {
    Fixture fx("create_validate");

    // 折叠条目必须显式指定音源（1.8 规则 1 / 2.10 规则 1）
    const auto no_source = fx.manager.create("", "radish:t_1");
    CHECK_FALSE(no_source.ok());
    CHECK(no_source.status().code == ErrorCode::InvalidMetadata);
    const auto no_item = fx.manager.create("radish", "");
    CHECK_FALSE(no_item.ok());
    CHECK(no_item.status().code == ErrorCode::InvalidMetadata);

    // 音质标识无法识别 → 1027
    const auto undefined = fx.manager.create("radish", "t_1", "flac24");
    CHECK_FALSE(undefined.ok());
    CHECK(undefined.status().code == ErrorCode::QualityUndefined);

    // 该源不支持目标等级 → 1021
    const auto unsupported = fx.manager.create("radish", "t_1", "low");
    CHECK_FALSE(unsupported.ok());
    CHECK(unsupported.status().code == ErrorCode::QualityUnsupported);

    // 合法：auto / 归一化 level / 平台 quality_id / <platform>:<quality_id>
    const auto automatic = fx.manager.create("radish", "t_1", "auto");
    CHECK(automatic.ok());
    if (automatic.ok()) {
        CHECK(automatic.value().state == DownloadState::Queued);
        CHECK_EQ(automatic.value().source, std::string("radish"));
        CHECK_EQ(automatic.value().item_id, std::string("t_1"));
        CHECK_EQ(automatic.value().quality_request, std::string("auto"));
        CHECK_EQ(automatic.value().filename_template,
                 std::string("{artist} - {title} [{quality}]"));
        CHECK(automatic.value().quality_resolved.is_known());
        CHECK_EQ(automatic.value().quality_resolved.quality_id, std::string("lossless"));   // 最高可用
    }
    const auto by_level = fx.manager.create("radish", "t_1", "lossless");
    CHECK(by_level.ok());
    if (by_level.ok()) CHECK_EQ(by_level.value().quality_resolved.quality_id, std::string("lossless"));

    const auto by_quality_id = fx.manager.create("radish", "t_1", "exhigh");
    CHECK(by_quality_id.ok());
    if (by_quality_id.ok()) {
        CHECK_EQ(by_quality_id.value().quality_resolved.quality_id, std::string("exhigh"));
        CHECK(by_quality_id.value().quality_resolved.level == QualityLevel::High);
    }

    const auto by_prefixed_id = fx.manager.create("radish", "t_1", "radish:exhigh");
    CHECK(by_prefixed_id.ok());
    if (by_prefixed_id.ok()) CHECK_EQ(by_prefixed_id.value().quality_resolved.quality_id, std::string("exhigh"));

    CHECK_EQ(fx.manager.list().size(), size_t{4});
    CHECK_EQ(fx.manager.running_count(), size_t{0});
}

TEST_CASE(test_download_create_dedupes_same_song_and_quality) {
    Fixture fx("dedup");

    // 每个 item_id 一首独立歌曲；同一 item + 同一音质 → 同一 group_key/quality_key
    std::string payload = "LOSSLESS PAYLOAD";
    fx.manager.resolve_meta = [&](const std::string& source, const std::string& item_id,
                                 const std::string& quality) {
        BaseInfo base;
        base.platform = source;
        base.item_id = item_id;
        base.name = "Song " + item_id;
        base.author = "Artist";
        base.duration_seconds = 215;
        base.quality = spec_quality(quality.empty() ? "lossless" : quality);
        base.sha = crypto::sha256_hex(payload);
        return Result<BaseInfo>::success(base);
    };
    fx.manager.resolve_url = [](const std::string&, const std::string& item_id, const std::string&) {
        return Result<std::string>::success("fake://" + item_id);
    };
    int fetch_calls = 0;
    fx.manager.fetch = [&](const std::string&, const std::filesystem::path& out, long long* received,
                           long long* total, std::string*) {
        ++fetch_calls;
        write_file(out, payload);
        *received = static_cast<long long>(payload.size());
        *total = static_cast<long long>(payload.size());
        return true;
    };

    const auto first = fx.manager.create("radish", "t_1001", "lossless");
    CHECK(first.ok());
    if (!first.ok()) return;
    CHECK(fx.manager.step());
    CHECK_EQ(fetch_calls, 1);
    CHECK_EQ(fx.manager.list().size(), size_t{1});
    CHECK(fx.manager.list()[0].state == DownloadState::Done);
    const std::string first_sha = fx.manager.list()[0].sha;
    CHECK_EQ(first_sha, crypto::sha256_hex(payload));

    // 同曲同质 → 返回已有记录并 skip（1.8 规则 2）
    const auto duplicate = fx.manager.create("radish", "t_1001", "lossless");
    CHECK(duplicate.ok());
    if (duplicate.ok()) {
        CHECK(duplicate.value().state == DownloadState::Done);
        CHECK_EQ(duplicate.value().message, std::string("already exists"));
        CHECK_EQ(duplicate.value().sha, first_sha);
        CHECK_EQ(duplicate.value().quality_resolved.quality_id, std::string("lossless"));
    }
    CHECK_EQ(fx.manager.list().size(), size_t{1});   // 未新增任务
    CHECK_FALSE(fx.manager.step());                  // 无排队任务
    CHECK_EQ(fetch_calls, 1);

    // 不同音质 → 独立任务、独立文件（1.8 规则 1）
    payload = "EXHIGH PAYLOAD";
    const auto second = fx.manager.create("radish", "t_1001", "exhigh");
    CHECK(second.ok());
    if (second.ok()) CHECK(second.value().state == DownloadState::Queued);
    CHECK_EQ(fx.manager.list().size(), size_t{2});
    CHECK(fx.manager.step());
    CHECK_EQ(fetch_calls, 2);
    CHECK_EQ(fx.manager.list().size(), size_t{2});
    CHECK(fx.manager.list()[1].state == DownloadState::Done);

    const std::string second_sha = crypto::sha256_hex(payload);
    CHECK_NE(second_sha, first_sha);   // 不同内容 → 不同 tracks/<sha>/
    std::error_code ec;
    CHECK(std::filesystem::exists(fx.paths.track_dir(first_sha), ec));
    CHECK(std::filesystem::exists(fx.paths.track_dir(second_sha), ec));

    // 两条独立记录、不同 quality_key（互不覆写）
    const auto first_path = fx.meta.find_by_sha(first_sha);
    const auto second_path = fx.meta.find_by_sha(second_sha);
    CHECK(first_path.has_value());
    CHECK(second_path.has_value());
    if (first_path.has_value() && second_path.has_value()) {
        CHECK_EQ(first_path->quality_key, std::string("radish.lossless"));
        CHECK_EQ(second_path->quality_key, std::string("radish.exhigh"));
    }
}

TEST_CASE(test_download_step_success_writes_tracks_and_meta) {
    Fixture fx("step_success");
    const std::string payload = "0123456789abcdefghij RADISH AUDIO BYTES";
    const std::string payload_sha = crypto::sha256_hex(payload);

    fx.manager.resolve_meta = [&](const std::string& source, const std::string& item_id,
                                 const std::string& quality) {
        BaseInfo base;
        base.platform = source;
        base.item_id = item_id;
        base.name = "Step Song " + item_id;
        base.author = "Step Artist";
        base.duration_seconds = 200;
        base.quality = spec_quality(quality.empty() ? "lossless" : quality);
        base.sha = payload_sha;   // 源上报校验值
        return Result<BaseInfo>::success(base);
    };
    fx.manager.resolve_url = [](const std::string&, const std::string& item_id, const std::string& quality) {
        return Result<std::string>::success("fake://" + item_id + "?q=" + quality);
    };
    fx.manager.fetch = [&](const std::string&, const std::filesystem::path& out, long long* received,
                           long long* total, std::string*) {
        write_file(out, payload);
        *received = static_cast<long long>(payload.size());
        *total = static_cast<long long>(payload.size());
        return true;
    };
    int progress = 0;
    int done = 0;
    int failed = 0;
    fx.manager.on_progress = [&](const DownloadTask&) { ++progress; };
    fx.manager.on_done = [&](const DownloadTask&) { ++done; };
    fx.manager.on_failed = [&](const DownloadTask&) { ++failed; };

    const auto created = fx.manager.create("radish", "t_9", "lossless");
    CHECK(created.ok());
    if (!created.ok()) return;
    const std::string id = created.value().id;

    CHECK(fx.manager.step());
    CHECK_FALSE(fx.manager.step());   // 无排队任务
    CHECK_EQ(progress, 1);
    CHECK_EQ(done, 1);
    CHECK_EQ(failed, 0);

    const auto task = fx.manager.get(id);
    CHECK(task.ok());
    if (!task.ok()) return;
    CHECK(task.value().state == DownloadState::Done);
    CHECK_EQ(task.value().sha, payload_sha);
    CHECK_EQ(task.value().received, static_cast<long long>(payload.size()));
    CHECK_EQ(task.value().total, static_cast<long long>(payload.size()));
    CHECK_EQ(task.value().last_error, ErrorCode::Ok);
    CHECK(task.value().quality_resolved.is_known());
    CHECK_EQ(task.value().quality_resolved.quality_id, std::string("lossless"));
    CHECK_EQ(fx.manager.running_count(), size_t{0});

    // .part 被消费
    std::error_code ec;
    CHECK_FALSE(std::filesystem::exists(fx.paths.tmp() / (id + ".part"), ec));

    // tracks/<sha>/ 下有落盘文件
    const auto track_dir = fx.paths.track_dir(payload_sha);
    CHECK(std::filesystem::exists(track_dir, ec));
    size_t files = 0;
    std::filesystem::path audio_file;
    for (const auto& entry : std::filesystem::directory_iterator(track_dir, ec)) {
        if (entry.is_regular_file()) {
            ++files;
            audio_file = entry.path();
        }
    }
    CHECK_EQ(files, size_t{1});
    CHECK_EQ(read_file(audio_file), payload);
    CHECK_EQ(path_to_utf8(audio_file), task.value().path);
    CHECK(path_to_utf8(audio_file.filename()).find("Step Artist - Step Song t_9") != std::string::npos);
    // 任务对外输出的 filename 必须是落盘后的真实文件名，不能是命名模板
    CHECK_EQ(task.value().filename, path_to_utf8(audio_file.filename()));
    if (const json::Value task_json = task.value().to_json(); task_json.is_object()) {
        CHECK_EQ(task_json.get_string("filename"), path_to_utf8(audio_file.filename()));
        CHECK(task_json.get_string("filename").find('{') == std::string::npos);
        CHECK_EQ(task_json.get_string("filename_template"), std::string("{artist} - {title} [{quality}]"));
    }

    // base.json 落入 cache/meta/<song_key>/<quality_key>/
    const auto meta_path = fx.meta.find_by_sha(payload_sha);
    CHECK(meta_path.has_value());
    if (meta_path.has_value()) {
        CHECK_EQ(meta_path->quality_key, std::string("radish.lossless"));
        CHECK(meta_path->song_key.find("Step Song t_9") != std::string::npos);
        CHECK(std::filesystem::exists(fx.paths.meta_file(*meta_path), ec));
        CHECK_EQ(fx.paths.meta_file(*meta_path).parent_path().filename().string(),
                 meta_path->quality_key);
        const auto stored = fx.meta.read(*meta_path);
        CHECK(stored.ok());
        if (stored.ok()) {
            CHECK_EQ(stored.value().sha, payload_sha);
            CHECK_EQ(stored.value().secret, 0);
            CHECK_EQ(stored.value().platform, std::string("radish"));
            CHECK_EQ(stored.value().item_id, std::string("t_9"));
            CHECK_EQ(stored.value().location, std::string("tracks/") + payload_sha + "/" +
                                                  path_to_utf8(audio_file.filename()));
            CHECK(std::filesystem::exists(fx.paths.root / stored.value().location, ec));
        }
    }

    // 库索引已更新
    CHECK_EQ(fx.index.size(), size_t{1});
    LibraryIndex reloaded(fx.paths);
    CHECK(reloaded.load().ok());
    CHECK_EQ(reloaded.size(), size_t{1});
    CHECK(reloaded.find_by_sha(payload_sha).has_value());

    // Done 的任务不能再 pause/cancel
    CHECK_FALSE(fx.manager.pause(id).ok());
    CHECK_FALSE(fx.manager.cancel(id).ok());
}

TEST_CASE(test_download_checksum_mismatch_keeps_part_then_resume_succeeds) {
    Fixture fx("checksum");
    const std::string payload = "CHECKSUM PAYLOAD BYTES";
    const std::string good_sha = crypto::sha256_hex(payload);
    bool report_bad_sha = true;
    int calls = 0;
    long long resumed_from = -1;

    fx.manager.resolve_meta = [&](const std::string& source, const std::string& item_id,
                                 const std::string& quality) {
        BaseInfo base;
        base.platform = source;
        base.item_id = item_id;
        base.name = "Checksum Song " + item_id;
        base.author = "Artist";
        base.duration_seconds = 120;
        base.quality = spec_quality(quality.empty() ? "lossless" : quality);
        base.sha = report_bad_sha ? fake_sha('z') : good_sha;   // 先给出错误校验值
        return Result<BaseInfo>::success(base);
    };
    fx.manager.resolve_url = [](const std::string&, const std::string&, const std::string&) {
        return Result<std::string>::success("fake://checksum.flac");
    };
    fx.manager.fetch = [&](const std::string&, const std::filesystem::path& out, long long* received,
                           long long* total, std::string*) {
        ++calls;
        if (calls == 2) resumed_from = *received;   // 续传时应拿到已存在大小
        if (*received == 0) write_file(out, payload);
        *received = static_cast<long long>(payload.size());
        *total = static_cast<long long>(payload.size());
        return true;
    };

    const auto created = fx.manager.create("radish", "t_c1", "lossless");
    CHECK(created.ok());
    if (!created.ok()) return;
    const std::string id = created.value().id;
    const auto part = fx.paths.tmp() / (id + ".part");

    CHECK(fx.manager.step());
    const auto failed = fx.manager.get(id);
    CHECK(failed.ok());
    if (failed.ok()) {
        CHECK(failed.value().state == DownloadState::Failed);
        CHECK(failed.value().last_error == ErrorCode::ChecksumMismatch);   // 1022
        CHECK_EQ(failed.value().retry, 1);
        CHECK(failed.value().message.find(good_sha) != std::string::npos);
    }
    // .part 保留供重试，且未落盘到 tracks/
    std::error_code ec;
    CHECK(std::filesystem::exists(part, ec));
    CHECK_EQ(read_file(part), payload);
    CHECK_FALSE(std::filesystem::exists(fx.paths.track_dir(good_sha), ec));

    // 修正源校验值 → resume → 从 .part 续传完成
    report_bad_sha = false;
    CHECK(fx.manager.resume(id).ok());
    const auto queued = fx.manager.get(id);
    CHECK(queued.ok());
    if (queued.ok()) CHECK(queued.value().state == DownloadState::Queued);
    CHECK(fx.manager.step());
    const auto done_task = fx.manager.get(id);
    CHECK(done_task.ok());
    if (done_task.ok()) {
        CHECK(done_task.value().state == DownloadState::Done);
        CHECK_EQ(done_task.value().sha, good_sha);
    }
    CHECK_EQ(resumed_from, static_cast<long long>(payload.size()));
    CHECK_FALSE(std::filesystem::exists(part, ec));
    CHECK(std::filesystem::exists(fx.paths.track_dir(good_sha), ec));
}

TEST_CASE(test_download_pause_resume_cancel_state_machine) {
    Fixture fx("state_machine");

    const auto first = fx.manager.create("radish", "t_a", "lossless");
    const auto second = fx.manager.create("radish", "t_b", "exhigh");
    CHECK(first.ok());
    CHECK(second.ok());
    if (!first.ok() || !second.ok()) return;
    const std::string a = first.value().id;
    const std::string b = second.value().id;

    // queued → paused → queued
    CHECK(fx.manager.pause(a).ok());
    auto paused = fx.manager.get(a);
    CHECK(paused.ok());
    if (paused.ok()) CHECK(paused.value().state == DownloadState::Paused);
    CHECK(fx.manager.pause(a).ok());   // 幂等
    CHECK(fx.manager.resume(a).ok());
    auto queued = fx.manager.get(a);
    CHECK(queued.ok());
    if (queued.ok()) CHECK(queued.value().state == DownloadState::Queued);

    // cancel：置 canceled 并清理 .part
    std::error_code ec;
    const auto part_b = fx.paths.tmp() / (b + ".part");
    write_file(part_b, "PARTIAL");
    CHECK(fx.manager.cancel(b).ok());
    const auto canceled = fx.manager.get(b);
    CHECK(canceled.ok());
    if (canceled.ok()) CHECK(canceled.value().state == DownloadState::Canceled);
    CHECK_FALSE(std::filesystem::exists(part_b, ec));

    // 取消后不能 resume；只能重新创建任务
    CHECK_FALSE(fx.manager.resume(b).ok());

    // 不存在的任务 → 1020
    CHECK_FALSE(fx.manager.get("dl_9999").ok());
    CHECK(fx.manager.get("dl_9999").status().code == ErrorCode::TaskNotFound);
    CHECK(fx.manager.pause("dl_9999").code == ErrorCode::TaskNotFound);
    CHECK(fx.manager.resume("dl_9999").code == ErrorCode::TaskNotFound);
    CHECK(fx.manager.cancel("dl_9999").code == ErrorCode::TaskNotFound);
    CHECK_FALSE(fx.manager.remove("dl_9999").ok());
}

TEST_CASE(test_download_fetch_interrupt_by_pause_keeps_part) {
    Fixture fx("interrupt");
    const std::string payload = "PARTIAL BYTES";
    fx.manager.resolve_meta = [&](const std::string& source, const std::string& item_id,
                                 const std::string& quality) {
        BaseInfo base;
        base.platform = source;
        base.item_id = item_id;
        base.name = "Interrupt Song " + item_id;
        base.author = "Artist";
        base.duration_seconds = 100;
        base.quality = spec_quality(quality.empty() ? "lossless" : quality);
        base.sha = crypto::sha256_hex(payload);
        return Result<BaseInfo>::success(base);
    };
    fx.manager.resolve_url = [](const std::string&, const std::string&, const std::string&) {
        return Result<std::string>::success("fake://interrupt.flac");
    };

    const auto created = fx.manager.create("radish", "t_i", "lossless");
    CHECK(created.ok());
    if (!created.ok()) return;
    const std::string id = created.value().id;

    fx.manager.fetch = [&](const std::string&, const std::filesystem::path& out, long long* received,
                           long long* total, std::string* error) {
        write_file(out, payload);   // 已写入的部分数据
        *received = static_cast<long long>(payload.size());
        *total = static_cast<long long>(payload.size()) * 4;
        if (error) *error = "interrupted by pause";
        fx.manager.pause(id);       // 模拟 fetch 执行中被暂停
        return false;
    };

    CHECK(fx.manager.step());
    const auto task = fx.manager.get(id);
    CHECK(task.ok());
    if (task.ok()) {
        CHECK(task.value().state == DownloadState::Paused);   // 尊重外部状态，不标记为失败
        CHECK(task.value().last_error == ErrorCode::Ok);
        CHECK_EQ(task.value().received, static_cast<long long>(payload.size()));
    }
    std::error_code ec;
    CHECK(std::filesystem::exists(fx.paths.tmp() / (id + ".part"), ec));   // 断点保留
}

TEST_CASE(test_download_remove_only_deletes_tracks_file) {
    Fixture fx("remove");
    const auto payload_of = [](const std::string& item_id) { return std::string("PAYLOAD-") + item_id; };

    fx.manager.resolve_meta = [&](const std::string& source, const std::string& item_id,
                                 const std::string& quality) {
        BaseInfo base;
        base.platform = source;
        base.item_id = item_id;
        base.name = "Remove Song " + item_id;
        base.author = "Artist";
        base.duration_seconds = 100;
        base.quality = spec_quality(quality.empty() ? "lossless" : quality);
        base.sha = crypto::sha256_hex(payload_of(item_id));
        return Result<BaseInfo>::success(base);
    };
    fx.manager.resolve_url = [](const std::string&, const std::string& item_id, const std::string&) {
        return Result<std::string>::success("fake://" + item_id);
    };
    fx.manager.fetch = [&](const std::string& url, const std::filesystem::path& out, long long* received,
                           long long* total, std::string*) {
        const std::string item = url.rfind("fake://", 0) == 0 ? url.substr(7) : std::string();
        const std::string payload = payload_of(item);
        write_file(out, payload);
        *received = static_cast<long long>(payload.size());
        *total = static_cast<long long>(payload.size());
        return true;
    };

    const auto first = fx.manager.create("radish", "t_r1", "lossless");
    CHECK(first.ok());
    if (!first.ok()) return;
    CHECK(fx.manager.step());
    const auto task = fx.manager.get(first.value().id);
    CHECK(task.ok());
    if (!task.ok()) return;
    const std::string path = task.value().path;
    CHECK(std::filesystem::exists(path));

    // delete_file = false：仅移除记录
    const auto record_only = fx.manager.remove(first.value().id, false);
    CHECK(record_only.ok());
    if (record_only.ok()) CHECK_FALSE(record_only.value());
    CHECK(std::filesystem::exists(path));
    CHECK_FALSE(fx.manager.get(first.value().id).ok());

    // delete_file = true：删掉 tracks/<sha>/ 下的文件
    const auto second = fx.manager.create("radish", "t_r2", "lossless");
    CHECK(second.ok());
    if (!second.ok()) return;
    CHECK(fx.manager.step());
    const auto second_task = fx.manager.get(second.value().id);
    CHECK(second_task.ok());
    if (!second_task.ok()) return;
    const std::string second_path = second_task.value().path;
    CHECK(std::filesystem::exists(second_path));
    const auto deleted = fx.manager.remove(second.value().id, true);
    CHECK(deleted.ok());
    if (deleted.ok()) CHECK(deleted.value());
    CHECK_FALSE(std::filesystem::exists(second_path));

    // 路径不在 tracks/ 下 → 拒绝删除
    std::error_code ec;
    const auto evil = fx.paths.cache() / "tmp" / "evil.bin";
    write_file(evil, "DO NOT DELETE");
    json::Value persisted = json::Value::object();
    persisted.set("version", static_cast<long long>(1));
    persisted.set("next_id", static_cast<long long>(9001));
    json::Value tasks = json::Value::array();
    json::Value crafted = json::Value::object();
    crafted.set("id", "dl_9000");
    crafted.set("source", "radish");
    crafted.set("item_id", "evil");
    crafted.set("quality", "lossless");
    crafted.set("state", "done");
    crafted.set("path", path_to_utf8(evil));
    tasks.push_back(crafted);
    persisted.set("tasks", tasks);
    write_file(fx.paths.cache() / "downloads.json", persisted.dump(2));
    DownloadManager guarded(fx.paths, fx.meta, fx.index, fx.registry);
    CHECK(guarded.load().ok());
    CHECK_EQ(guarded.list().size(), size_t{1});
    const auto refused = guarded.remove("dl_9000", true);
    CHECK_FALSE(refused.ok());
    CHECK(refused.status().code == ErrorCode::InvalidMetadata);
    CHECK(std::filesystem::exists(evil, ec));
    CHECK(guarded.get("dl_9000").ok());   // 拒绝时任务记录保留
}

TEST_CASE(test_download_save_load_roundtrip_running_becomes_paused) {
    Fixture fx("persist");

    const auto first = fx.manager.create("radish", "t_a", "auto");
    const auto second = fx.manager.create("radish", "t_b", "exhigh", "{title}");
    CHECK(first.ok());
    CHECK(second.ok());
    if (!first.ok() || !second.ok()) return;
    CHECK(fx.manager.save().ok());
    CHECK(std::filesystem::exists(fx.paths.cache() / "downloads.json"));

    DownloadManager reloaded(fx.paths, fx.meta, fx.index, fx.registry);
    CHECK(reloaded.load().ok());
    CHECK_EQ(reloaded.list().size(), size_t{2});
    const auto loaded_first = reloaded.get(first.value().id);
    CHECK(loaded_first.ok());
    if (loaded_first.ok()) {
        CHECK_EQ(loaded_first.value().item_id, std::string("t_a"));
        CHECK_EQ(loaded_first.value().quality_request, std::string("auto"));
        CHECK_EQ(loaded_first.value().source, std::string("radish"));
        CHECK(loaded_first.value().state == DownloadState::Queued);
        CHECK_EQ(loaded_first.value().quality_resolved.quality_id, std::string("lossless"));
    }
    const auto loaded_second = reloaded.get(second.value().id);
    CHECK(loaded_second.ok());
    if (loaded_second.ok()) {
        CHECK_EQ(loaded_second.value().filename_template, std::string("{title}"));
        CHECK_EQ(loaded_second.value().quality_request, std::string("exhigh"));
    }
    // 重复 load 不产生重复任务
    CHECK(reloaded.load().ok());
    CHECK_EQ(reloaded.list().size(), size_t{2});

    // 运行中的任务重启后按 Paused 处理（保留断点，可 resume）
    write_file(fx.paths.cache() / "downloads.json",
               "{\"version\":1,\"next_id\":9,\"tasks\":[{\"id\":\"dl_0008\",\"source\":\"radish\","
               "\"item_id\":\"t_c\",\"quality\":\"lossless\",\"filename\":\"{title}\","
               "\"state\":\"running\",\"received\":1024,\"total\":4096,\"retry\":1,"
               "\"last_error\":1022,\"message\":\"x\"}]}");
    DownloadManager restarted(fx.paths, fx.meta, fx.index, fx.registry);
    CHECK(restarted.load().ok());
    CHECK_EQ(restarted.list().size(), size_t{1});
    const std::vector<DownloadTask> restored = restarted.list();
    if (!restored.empty()) {
        const DownloadTask task = restored[0];
        CHECK(task.state == DownloadState::Paused);
        CHECK_EQ(task.id, std::string("dl_0008"));
        CHECK_EQ(task.received, static_cast<long long>(1024));
        CHECK_EQ(task.total, static_cast<long long>(4096));
        CHECK_EQ(task.retry, 1);
        CHECK(task.last_error == ErrorCode::ChecksumMismatch);
        CHECK_EQ(restarted.running_count(), size_t{0});
        // 旧版把模板写进了 filename：迁移后模板归位，filename 不再输出占位符
        CHECK_EQ(task.filename_template, std::string("{title}"));
        CHECK(task.filename.empty());
        CHECK(task.to_json().get_string("filename").empty());
    }
    // next_id 续接，不与已加载任务冲突
    const auto fresh = restarted.create("radish", "t_d", "lossless");
    CHECK(fresh.ok());
    if (fresh.ok()) {
        CHECK_NE(fresh.value().id, std::string("dl_0008"));
        CHECK(fresh.value().state == DownloadState::Queued);
    }
    CHECK_EQ(restarted.list().size(), size_t{2});

    // 完成任务（含落盘路径、sha）也应能回环：反斜杠路径经 JSON 转义后必须一致
    if (fresh.ok()) {
        const std::string payload = "PERSIST PAYLOAD";
        restarted.resolve_meta = [&](const std::string& source, const std::string& item_id,
                                     const std::string& quality) {
            BaseInfo base;
            base.platform = source;
            base.item_id = item_id;
            base.name = "Persist Song " + item_id;
            base.author = "Artist";
            base.duration_seconds = 100;
            base.quality = spec_quality(quality.empty() ? "lossless" : quality);
            base.sha = crypto::sha256_hex(payload);
            return Result<BaseInfo>::success(base);
        };
        restarted.resolve_url = [](const std::string&, const std::string&, const std::string&) {
            return Result<std::string>::success("fake://persist.flac");
        };
        restarted.fetch = [&](const std::string&, const std::filesystem::path& out, long long* received,
                              long long* total, std::string*) {
            write_file(out, payload);
            *received = static_cast<long long>(payload.size());
            *total = static_cast<long long>(payload.size());
            return true;
        };
        CHECK(restarted.step());
        const auto completed = restarted.get(fresh.value().id);
        CHECK(completed.ok());
        if (completed.ok()) {
            CHECK(completed.value().state == DownloadState::Done);
            CHECK(restarted.save().ok());

            DownloadManager reopened(fx.paths, fx.meta, fx.index, fx.registry);
            CHECK(reopened.load().ok());
            const auto round_tripped = reopened.get(fresh.value().id);
            CHECK(round_tripped.ok());
            if (round_tripped.ok()) {
                CHECK(round_tripped.value().state == DownloadState::Done);
                CHECK_EQ(round_tripped.value().path, completed.value().path);
                CHECK_EQ(round_tripped.value().sha, completed.value().sha);
                CHECK_EQ(round_tripped.value().quality_resolved.quality_id, std::string("lossless"));
            }
        }
    }

    // 缺失 downloads.json 时 load 是空成功
    Fixture empty_fx("persist_empty");
    DownloadManager blank(empty_fx.paths, empty_fx.meta, empty_fx.index, empty_fx.registry);
    CHECK(blank.load().ok());
    CHECK_EQ(blank.list().size(), size_t{0});
}

TEST_CASE(test_download_default_fetch_copies_local_file) {
    Fixture fx("local_fetch");
    const std::string payload = "LOCAL FILE PAYLOAD CONTENT";
    const auto source_file = fx.root / "source.bin";
    write_file(source_file, payload);

    fx.manager.resolve_meta = [&](const std::string& source, const std::string& item_id,
                                 const std::string& quality) {
        BaseInfo base;
        base.platform = source;
        base.item_id = item_id;
        base.name = "Local Song " + item_id;
        base.author = "Local Artist";
        base.duration_seconds = 100;
        base.quality = spec_quality(quality.empty() ? "lossless" : quality);
        base.sha = crypto::sha256_hex(payload);
        return Result<BaseInfo>::success(base);
    };
    fx.manager.resolve_url = [&](const std::string&, const std::string&, const std::string&) {
        return Result<std::string>::success(path_to_utf8(source_file));
    };
    // 不注入 fetch → 默认实现按本地文件分片复制（file:// 或本地路径）

    const auto created = fx.manager.create("radish", "t_local", "lossless");
    CHECK(created.ok());
    if (!created.ok()) return;
    CHECK(fx.manager.step());
    const auto task = fx.manager.get(created.value().id);
    CHECK(task.ok());
    if (task.ok()) {
        CHECK(task.value().state == DownloadState::Done);
        CHECK_EQ(task.value().sha, crypto::sha256_hex(payload));
        CHECK_EQ(task.value().received, static_cast<long long>(payload.size()));
        CHECK_EQ(task.value().total, static_cast<long long>(payload.size()));
        CHECK_EQ(read_file(task.value().path), payload);
    }

    // 网络地址：默认 fetch 无网络下载器 → 1018（宿主必须注入 fetch）
    const auto http = fx.manager.create("radish", "t_http", "lossless");
    CHECK(http.ok());
    if (http.ok()) {
        fx.manager.resolve_url = [](const std::string&, const std::string&, const std::string&) {
            return Result<std::string>::success("https://example.invalid/audio.flac");
        };
        // t_http 的 item_id 不同 → 不同 group_key，不会与上面去重
        CHECK(fx.manager.step());
        const auto failed = fx.manager.get(http.value().id);
        CHECK(failed.ok());
        if (failed.ok()) {
            CHECK(failed.value().state == DownloadState::Failed);
            CHECK(failed.value().last_error == ErrorCode::DownloadFailed);
        }
    }
}
