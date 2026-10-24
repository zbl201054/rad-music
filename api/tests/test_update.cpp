// 检查更新测试：版本比较 / inflate / zip 与 tar.gz 解压 / 归档路径安全
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "radmusic/archive.hpp"
#include "radmusic/update.hpp"
#include "test_framework.hpp"

using namespace radmusic;

namespace {

std::filesystem::path fresh_dir(const std::string& name) {
    const auto dir = std::filesystem::path("build") / "tmp_update" / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::vector<unsigned char> from_hex(const std::string& hex) {
    std::vector<unsigned char> out;
    out.reserve(hex.size() / 2);
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<unsigned char>((nibble(hex[i]) << 4) | nibble(hex[i + 1])));
    }
    return out;
}

void write_binary(const std::filesystem::path& path, const std::vector<unsigned char>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::string read_all(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string payload() {
    std::string text;
    for (int i = 0; i < 30; ++i) text += "Rad Music update payload line. ";
    return text;
}

// zlib 生成的 raw deflate（raw deflate of payload()）
const char* kRawDeflateHex =
    "0b4a4c51f02d2dce4c56282d48492c49552848acccc9070ae664e6a5ea29048d4a8f4a8f4a0fa43400";

// 含顶层目录 rad-music-v9.9.9-win64/ 的 zip（deflate），另有一个 ../ 穿越条目
const char* kZipHex =
    "504b030414000000000000002100000000000000000000000000170000007261642d6d757369632d76392e392e392d77696e"
    "36342f504b03041400000008005c48585d1a206f262a000000a40300002a0000007261642d6d757369632d76392e392e392d"
    "77696e36342f7261646d757369635f7365727665722e657865f38d0a4a4c51f02d2dce4c56282d48492c49552848acccc9"
    "070ae664e6a5ea298c4a8f4a8f4a0fa83400504b03041400000008005c48585d68fe0143100000000e00000021000000"
    "7261642d6d757369632d76392e392e392d77696e36342f7765622f6170702e6a734bcecf2bcecf49d5cbc94fd730d40400"
    "504b03041400000008005c48585d2c12c4610500000003000000220000007261642d6d757369632d76392e392e392d77"
    "696e36342f2e2e2f6576696c2e7478742b28cf0300504b01021400140000000000000021000000000000000000000000"
    "001700000000000000000000008001000000007261642d6d757369632d76392e392e392d77696e36342f504b01021400"
    "1400000008005c48585d1a206f262a000000a40300002a00000000000000000000008001350000007261642d6d7573"
    "69632d76392e392e392d77696e36342f7261646d757369635f7365727665722e657865504b01021400140000000800"
    "5c48585d68fe0143100000000e0000002100000000000000000000008001a70000007261642d6d757369632d76392e"
    "392e392d77696e36342f7765622f6170702e6a73504b010214001400000008005c48585d2c12c4610500000003000000"
    "2200000000000000000000008001f60000007261642d6d757369632d76392e392e392d77696e36342f2e2e2f657669"
    "6c2e747874504b050600000000040004003c0100003b0100000000";

// 同一份内容的 tar.gz（GNU 格式），同样带一个 ../../ 穿越条目
const char* kTarGzHex =
    "1f8b0800c003dc6a02ffedd5418e82301805e01ea54b5d50a9fe143d80ae9ccd5cc0546926980a4d0bc8dc7e8a4b169a38"
    "9149c6f741d2a46fc1a27d3f5e17c9a50de529e936223e892dabb64ffab53a285a785ddcc24330be339e3d278d14d16d"
    "8dc6ab54943149d992f23ca5615f66f992184fd904dad068cf397b53dbfdee5317fc633865deba4237863bfd6debb819"
    "af82111c3162c47f1abfb0fffeeefcbf9ae3423b27cee137df7830ff873fc068fe132985f93f85535d85da1a61ebaf99"
    "9c337833f7fb2fc4f09aaeb4a2e99b97f53f5d8dfb9fcb0cfd9f82bb562801000000000000000000000000c03ff103f2"
    "2c0a6600280000";

}  // namespace

TEST_CASE(update_version_compare) {
    CHECK_EQ(compare_version("0.11.0", "0.11.0"), 0);
    CHECK_EQ(compare_version("v0.12.0", "0.11.0"), 1);
    CHECK_EQ(compare_version("0.9.9", "0.10.0"), -1);
    CHECK_EQ(compare_version("1.0", "1.0.0"), 0);
    CHECK_EQ(compare_version("v1.0.0", "1.0.0"), 0);
    CHECK_EQ(compare_version("0.11.0-dev", "0.11.0"), 0);
    CHECK_EQ(compare_version("", "0.1.0"), -1);
    CHECK_EQ(compare_version("abc", "0.0.1"), -1);

    int parts[3] = {0, 0, 0};
    CHECK(parse_version("v0.12.3", parts));
    CHECK_EQ(parts[0], 0);
    CHECK_EQ(parts[1], 12);
    CHECK_EQ(parts[2], 3);
    CHECK_FALSE(parse_version("dev", parts));
}

TEST_CASE(update_inflate_raw) {
    const std::vector<unsigned char> packed = from_hex(kRawDeflateHex);
    std::vector<unsigned char> out;
    std::string error;
    CHECK(archive::inflate_raw(packed.data(), packed.size(), out, &error));
    CHECK_EQ(std::string(out.begin(), out.end()), payload());
}

TEST_CASE(update_extract_zip) {
    const auto dir = fresh_dir("zip");
    const auto zip_path = dir / "rad-music-v9.9.9-win64.zip";
    write_binary(zip_path, from_hex(kZipHex));

    const auto target = dir / "out";
    const auto result = archive::extract_zip(zip_path, target);
    CHECK(result.ok());
    if (!result.ok()) radtest::note("  解压失败: " + result.error);
    CHECK_EQ(result.files, 2u);       // 顶层目录条目不算文件；穿越条目被跳过
    CHECK_EQ(result.skipped, 1u);
    CHECK(std::filesystem::exists(target / "radmusic_server.exe"));
    CHECK(std::filesystem::exists(target / "web" / "app.js"));
    CHECK_FALSE(std::filesystem::exists(target / "evil.txt"));
    CHECK_FALSE(std::filesystem::exists(dir / "evil.txt"));
    // 顶层目录被剥掉，可执行文件内容完整
    const std::string exe = read_all(target / "radmusic_server.exe");
    CHECK_EQ(exe, "MZ" + payload());
}

TEST_CASE(update_extract_tar_gz) {
    const auto dir = fresh_dir("tgz");
    const auto tgz_path = dir / "rad-music-v9.9.9-linux-x86_64.tar.gz";
    write_binary(tgz_path, from_hex(kTarGzHex));

    const auto target = dir / "out";
    const auto result = archive::extract_tar_gz(tgz_path, target);
    CHECK(result.ok());
    if (!result.ok()) radtest::note("  解压失败: " + result.error);
    CHECK_EQ(result.files, 2u);
    CHECK(std::filesystem::exists(target / "radmusic_server"));
    CHECK(std::filesystem::exists(target / "web" / "app.js"));
    CHECK_FALSE(std::filesystem::exists(target / "evil.txt"));
    CHECK_EQ(read_all(target / "radmusic_server"), "ELF" + payload());
}

TEST_CASE(update_rejects_corrupt_archive) {
    const auto dir = fresh_dir("corrupt");
    const auto zip_path = dir / "broken.zip";
    write_binary(zip_path, {'P', 'K', 3, 4, 0, 0, 0, 0});
    const auto result = archive::extract_zip(zip_path, dir / "out");
    CHECK_FALSE(result.ok());

    const auto tgz = dir / "broken.tar.gz";
    write_binary(tgz, {'n', 'o', 't', 'g', 'z'});
    CHECK_FALSE(archive::extract_tar_gz(tgz, dir / "out2").ok());
}

TEST_CASE(update_detects_corrupt_zip) {
    // 改掉中央目录里第一个「文件」条目的 CRC：数据仍能解出，但完整性校验必须拒绝
    // （第一条是目录条目，CRC 为 0，解压时会被跳过，所以要跳过它）
    const auto dir = fresh_dir("crc");
    std::vector<unsigned char> bytes = from_hex(kZipHex);
    size_t cd = std::string::npos;
    for (size_t i = 0; i + 20 <= bytes.size(); ++i) {
        if (!(bytes[i] == 'P' && bytes[i + 1] == 'K' && bytes[i + 2] == 1 && bytes[i + 3] == 2)) continue;
        const bool zero_crc = bytes[i + 16] == 0 && bytes[i + 17] == 0 && bytes[i + 18] == 0 && bytes[i + 19] == 0;
        if (!zero_crc) {
            cd = i;
            break;
        }
    }
    CHECK(cd != std::string::npos);
    if (cd == std::string::npos) return;
    bytes[cd + 16] ^= 0xFF;   // CRC-32 字段
    const auto zip_path = dir / "damaged.zip";
    write_binary(zip_path, bytes);
    const auto result = archive::extract_zip(zip_path, dir / "out");
    CHECK_FALSE(result.ok());
    if (!result.ok()) radtest::note("  拒绝原因: " + result.error);
}

TEST_CASE(update_segment_plan) {
    // 4 线程、10MB、每段至少 512KB → 4 段，首尾相接且覆盖完整
    const auto segments = plan_update_segments(10 * 1024 * 1024, 4);
    CHECK_EQ(segments.size(), 4u);
    if (segments.size() == 4) {
        CHECK_EQ(segments.front().start, 0LL);
        CHECK_EQ(segments.back().end, 10LL * 1024 * 1024 - 1);
        for (size_t i = 1; i < segments.size(); ++i) {
            CHECK_EQ(segments[i].start, segments[i - 1].end + 1);
        }
        long long sum = 0;
        for (const auto& segment : segments) sum += segment.size();
        CHECK_EQ(sum, 10LL * 1024 * 1024);
    }

    // 包太小（< 512KB）不分段，退回单线程
    CHECK_EQ(plan_update_segments(300 * 1024, 8).size(), 1u);
    // 总数不是线程数整数倍时，余数并入最后一段
    const auto uneven = plan_update_segments(1000, 3, 10);
    CHECK_EQ(uneven.size(), 3u);
    if (uneven.size() == 3) {
        CHECK_EQ(uneven[0].size() + uneven[1].size() + uneven[2].size(), 1000LL);
        CHECK_EQ(uneven.back().end, 999LL);
    }
    // 非法入参：total<=0 返回空；threads 超过 16 被收敛
    CHECK_EQ(plan_update_segments(0, 4).size(), 0u);
    CHECK_EQ(plan_update_segments(-5, 4).size(), 0u);
    CHECK(plan_update_segments(64LL * 1024 * 1024, 999).size() <= 16u);
    CHECK_EQ(plan_update_segments(64LL * 1024 * 1024, 0).size(), 1u);
}

TEST_CASE(update_threads_clamped) {
    UpdateService service(std::filesystem::path("build") / "tmp_update" / "threads", "0.11.0",
                          "zbl201054/rad-music", UpdateService::detect_platform_slug());
    service.set_threads(8);
    CHECK_EQ(service.threads(), 8);
    service.set_threads(0);
    CHECK_EQ(service.threads(), 1);
    service.set_threads(999);
    CHECK_EQ(service.threads(), 16);
    CHECK_EQ(service.status().get_int("threads", 0), 16LL);
}

TEST_CASE(update_auto_apply_flag) {
    UpdateService service(std::filesystem::path("build") / "tmp_update" / "auto_apply", "0.11.0",
                          "zbl201054/rad-music", UpdateService::detect_platform_slug());
    // 默认关闭：由宿主按设置显式打开（避免未配置的构建擅自重启）
    CHECK_FALSE(service.auto_apply());
    CHECK_EQ(service.status().get_bool("auto_apply"), false);

    service.set_auto_apply(true);
    CHECK(service.auto_apply());
    CHECK_EQ(service.status().get_bool("auto_apply"), true);
    // 没有下载好的更新包：倒计时不应出现
    CHECK_FALSE(service.status().contains("auto_apply_in"));

    // 未排倒计时时取消也必须安全（幂等）
    service.cancel_auto_apply();
    CHECK_FALSE(service.status().contains("auto_apply_in"));

    // 关设置即取消（防「都关了还重启」）
    service.set_auto_apply(false);
    CHECK_FALSE(service.auto_apply());
    CHECK_EQ(service.status().get_bool("auto_apply"), false);

    // 开启状态下重新检查会作废待执行的自动安装
    service.set_auto_apply(true);
    CHECK(service.begin_check().ok());
    CHECK_FALSE(service.status().contains("auto_apply_in"));
}

TEST_CASE(update_unsafe_paths) {
    CHECK_FALSE(archive::is_safe_relative_path("../evil.txt"));
    CHECK_FALSE(archive::is_safe_relative_path("a/../../evil.txt"));
    CHECK_FALSE(archive::is_safe_relative_path("/etc/passwd"));
    CHECK_FALSE(archive::is_safe_relative_path("C:/Windows/system32/x.dll"));
    CHECK_FALSE(archive::is_safe_relative_path(""));
    CHECK(archive::is_safe_relative_path("web/app.js"));
    CHECK(archive::is_safe_relative_path("./web/app.js"));
    // 策略：任何一段是 ".." 都拒绝（即使归一化后仍在根内），避免平台间归一化差异
    CHECK_FALSE(archive::is_safe_relative_path("web/../app.js"));
}

TEST_CASE(update_service_flow) {
    // 没有注入安装目录时，下载与更新都应明确拒绝，而不是静默成功
    UpdateService service(std::filesystem::path("build") / "tmp_update" / "svc", "0.11.0",
                          "zbl201054/rad-music", UpdateService::detect_platform_slug());
    CHECK_FALSE(service.busy());
    CHECK_EQ(std::string(update_state_id(UpdateState::Idle)), std::string("idle"));

    const json::Value before = service.status();
    CHECK_EQ(before.get_string("current"), std::string("0.11.0"));
    CHECK_EQ(before.get_bool("has_update"), false);
    CHECK_EQ(before.get_bool("can_apply"), false);
    CHECK_FALSE(service.begin_download().ok());   // 还没检查/没有新版本
    CHECK_FALSE(service.apply().ok());            // 没有下载好的包

    // 触发检查后进入 checking（真正的网络请求由 step() 在宿主空闲时执行）
    CHECK(service.begin_check().ok());
    CHECK(service.busy());
    CHECK_EQ(std::string(service.status().get_string("state")), std::string("checking"));
    CHECK_FALSE(service.begin_check().ok());   // 同一时刻只允许一个任务
}

TEST_CASE(update_channel_classify) {
    // 正式版：无预发布后缀且 GitHub 没标 prerelease
    CHECK_EQ(classify_update_channel("v0.12.0", false), std::string("release"));
    CHECK_EQ(classify_update_channel("0.12.0", false), std::string("release"));
    // 后缀优先于 prerelease 标记：dev/canary 各自归位
    CHECK_EQ(classify_update_channel("v0.12.0-dev.3", false), std::string("dev"));
    CHECK_EQ(classify_update_channel("v0.12.0-beta.1", true), std::string("dev"));
    CHECK_EQ(classify_update_channel("v0.12.0-rc.2", false), std::string("dev"));
    CHECK_EQ(classify_update_channel("v0.12.0-canary.7", true), std::string("canary"));
    // 无后缀但标了 prerelease → dev
    CHECK_EQ(classify_update_channel("v0.12.0", true), std::string("dev"));
    // 频道只看 '-' 之后的词，产品名里带 canary 不算
    CHECK_EQ(classify_update_channel("canary-0.12.0", false), std::string("release"));

    // 当前构建属于哪个频道
    CHECK_EQ(channel_of_version("0.11.0"), std::string("release"));
    CHECK_EQ(channel_of_version("0.11.0-dev"), std::string("dev"));
    CHECK_EQ(channel_of_version("v0.12.0-canary.1"), std::string("canary"));

    // 归一化：别名互通，未知值回落 release
    CHECK_EQ(normalize_update_channel("CANARY"), std::string("canary"));
    CHECK_EQ(normalize_update_channel("beta"), std::string("dev"));
    CHECK_EQ(normalize_update_channel("nightly"), std::string("canary"));
    CHECK_EQ(normalize_update_channel("whatever"), std::string("release"));
    CHECK_EQ(normalize_update_channel(""), std::string("release"));
    CHECK_EQ(std::string(update_channel_label("canary")), std::string("金丝雀"));
}

TEST_CASE(update_version_full_compare) {
    // 基础三段相同时比较预发布后缀
    CHECK_EQ(compare_version_full("0.12.0", "0.12.0-canary.2"), 1);
    CHECK_EQ(compare_version_full("0.12.0-canary.2", "0.12.0"), -1);
    CHECK_EQ(compare_version_full("0.12.0-canary.10", "0.12.0-canary.9"), 1);
    CHECK_EQ(compare_version_full("0.12.0-dev.1", "0.12.0-rc.1"), -1);
    CHECK_EQ(compare_version_full("0.12.0-canary.3", "0.12.0-canary.3"), 0);
    CHECK_EQ(compare_version_full("0.13.0-canary.1", "0.12.9"), 1);
    CHECK_EQ(compare_version_full("0.11.0-dev", "0.11.0"), -1);
    // 与旧接口保持一致的场景
    CHECK_EQ(compare_version_full("v0.12.0", "0.11.0"), 1);
    CHECK_EQ(compare_version_full("1.0", "1.0.0"), 0);
}

TEST_CASE(update_channel_switch) {
    UpdateService service(std::filesystem::path("build") / "tmp_update" / "channel", "0.11.0-dev",
                          "zbl201054/rad-music", UpdateService::detect_platform_slug());
    CHECK_EQ(service.channel(), std::string("release"));   // 默认正式版
    const json::Value idle = service.status();
    CHECK_EQ(idle.get_string("channel"), std::string("release"));
    CHECK_EQ(idle.get_string("build_channel"), std::string("dev"));   // 构建本身是 dev
    CHECK_EQ(idle.get_int("has_release", 1), 0LL);                    // 还没检查过
    CHECK(idle["channels"].is_array());
    CHECK_EQ(idle["channels"].size(), 3u);
    CHECK(idle["releases"].is_array());

    CHECK(service.set_channel("canary"));            // 真的变了
    CHECK_EQ(service.channel(), std::string("canary"));
    CHECK_FALSE(service.set_channel("CANARY"));      // 归一化后相同 → 没变
    CHECK_EQ(service.status().get_string("channel"), std::string("canary"));
    CHECK(service.set_channel("nonsense"));          // 未知值回落 release，同样算变化
    CHECK_EQ(service.channel(), std::string("release"));
    CHECK_FALSE(service.set_channel("release"));     // 已经是 release → 没变
}

TEST_CASE(update_platform_slug_detected) {
    const std::string slug = UpdateService::detect_platform_slug();
    CHECK(!slug.empty());
#if defined(_WIN32)
    CHECK_EQ(slug, std::string("win64"));
#elif defined(__linux__)
    CHECK(slug.find("linux") == 0);
#endif
}
