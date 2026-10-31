// Rad music API - 歌单分享 / 曲库导出导入（v0.13）
//
// 两种对外文件格式（都自描述、不依赖本机路径，便于在别的机器上还原）：
//   .rdl  Rad music pLaylist —— JSON：歌单元数据 + 曲目引用（不含音频）
//   .rdm  Rad music Media    —— zip：base.json（清单）+ music/<音频> + covers/<封面>
// 另有「常规文件形式」：直接把音频复制成普通的 mp3 / flac 文件，任何播放器都能放。
//
// 设计约束：
//  * 只依赖 collections / storage / archive（零第三方依赖，单线程）；
//  * 导出产物统一落在 <root>/exports/ 下，导入则只读不写曲库以外的地方；
//  * 所有文件名都经过 share_safe_filename，避免 Windows 非法字符与目录穿越。
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "radmusic/collections.hpp"
#include "radmusic/error.hpp"
#include "radmusic/json.hpp"
#include "radmusic/storage.hpp"

namespace radmusic {

// 文件格式标识（写入 JSON 头，导入时用于识别）
constexpr const char* kSharePlaylistExt = ".rdl";
constexpr const char* kSharePackageExt = ".rdm";
constexpr const char* kSharePlaylistFormat = "radmusic.playlist";
constexpr const char* kSharePackageFormat = "radmusic.package";
constexpr const char* kShareManifestName = "base.json";
constexpr const char* kShareMusicDir = "music";
constexpr int kShareFormatVersion = 1;

// 文件名安全化：Windows 非法字符 → '_'，去首尾空白与结尾的点，最多 80 字节且不切坏 UTF-8
std::string share_safe_filename(std::string_view name, const std::string& fallback = "radmusic");

// 按扩展名判断分享文件类型：返回 "playlist"（.rdl）/ "package"（.rdm）/ ""（普通音频或未知）
std::string share_kind_of_filename(std::string_view filename);

// 歌单 → .rdl（format/version 头 + 歌单元数据 + 曲目列表）
json::Value share_rdl_from_playlist(const Playlist& playlist);
// .rdl（或一份歌单 JSON）→ Playlist；结构不可识别 / 没有可用曲目 → 1007
Result<Playlist> share_playlist_from_rdl(const json::Value& value);

struct ShareExportOptions {
    std::string name;                    // 输出名（不含扩展名）；空则用 "曲库导出-<时间>"
    bool as_package = false;             // true: 单个 .rdm；false: exports/<name>/ 下的常规音频文件
    std::string playlist_name;           // 非空时写进包里，导入侧据此建歌单
    std::string playlist_description;
};

struct ShareExportResult {
    std::string dir;                     // 产物目录（绝对路径）
    std::string file;                    // .rdm 路径；目录模式为空
    std::vector<std::string> files;      // 已导出的文件（绝对路径）
    std::vector<std::string> skipped;    // 被跳过的曲目（在线未落盘 / 文件缺失）
    int count = 0;                       // 成功导出的曲目数
    long long bytes = 0;
};

// 导出曲目（shas 为曲库里的 sha）。在线且未落盘的曲目会被跳过并记入 skipped。
Result<ShareExportResult> share_export_tracks(const LibraryPaths& paths, const class MetaCache& meta,
                                              const std::vector<std::string>& shas,
                                              const ShareExportOptions& options);

// 打开 .rdm：解压到 cache/tmp 下的临时目录并读取 base.json。
// 调用方负责在导入完成后删除 package.dir（share_discard_package）。
struct SharePackage {
    json::Value manifest;                       // base.json 原文
    std::filesystem::path dir;                  // 临时目录（整包根）
    std::filesystem::path audio_dir;            // 音频根目录（music/，缺失时回落到 dir）
    std::vector<std::filesystem::path> covers;  // 包内封面
    std::string name;
    std::string description;
    std::string playlist_name;                  // 包内歌单名（可为空）
    std::string playlist_description;
    int declared_tracks = 0;                    // 清单声明的曲目数
};

Result<SharePackage> share_open_package(const LibraryPaths& paths, const std::string& zip_bytes);
void share_discard_package(const SharePackage& package);

// 把包内封面（covers/<name>.<ext>）补进曲库 covers/（已存在则跳过）
size_t share_import_covers(const LibraryPaths& paths, const SharePackage& package);

}  // namespace radmusic
