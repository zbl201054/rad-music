// Rad music API - 歌单分享 / 曲库导出导入实现（见 share.hpp）
//
// 走查速记：
//   .rdl  = JSON（分享歌单，几十 KB，前端直接用 Blob 下载）
//   .rdm  = zip（导出音频，可能几百 MB，服务端落盘后由前端给出下载/打开目录）
// 导出目录：<root>/exports/<名字>/（常规文件）或 <root>/exports/<名字>.rdm（音频包）。
#include "radmusic/share.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <set>
#include <system_error>
#include <utility>

#include "radmusic/archive.hpp"
#include "radmusic/model.hpp"
#include "radmusic/version.hpp"

namespace radmusic {
namespace {

std::string path_to_utf8(const std::filesystem::path& p) {
    const auto u8 = p.u8string();
    return u8.empty() ? std::string() : std::string(reinterpret_cast<const char*>(u8.c_str()));
}

// 回显给用户的路径统一用原生分隔符（Windows 下是反斜杠），混着 / 与 \ 很难看
std::filesystem::path normalized(std::filesystem::path p) {
    p.make_preferred();
    return p;
}

std::string lower_ascii(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

long long now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// 20261031-1202，导出目录默认名里用（只含数字，避免文件名问题）
std::string timestamp_tag() {
    const std::time_t now = std::time(nullptr);
    std::tm parts{};
#ifdef _WIN32
    localtime_s(&parts, &now);
#else
    localtime_r(&now, &parts);
#endif
    char buffer[32] = {0};
    std::snprintf(buffer, sizeof(buffer), "%04d%02d%02d-%02d%02d", parts.tm_year + 1900,
                  parts.tm_mon + 1, parts.tm_mday, parts.tm_hour, parts.tm_min);
    return std::string(buffer);
}

bool is_remote(const std::string& location) {
    return location.rfind("http://", 0) == 0 || location.rfind("https://", 0) == 0;
}

// location → 真实可读文件；曲库相对路径按曲库根解析。读不到返回空。
std::filesystem::path resolve_audio_file(const LibraryPaths& paths, const BaseInfo& base) {
    if (base.location.empty() || is_remote(base.location)) return {};
    std::filesystem::path file(base.location);
    if (file.is_relative()) file = paths.root / file;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec) || ec) return {};
    return file;
}

std::string file_extension_lower(const std::filesystem::path& file) {
    std::string ext = lower_ascii(path_to_utf8(file.extension()));
    if (ext.empty()) ext = ".mp3";
    return ext;
}

std::string pad_index(size_t index) {
    std::string text = std::to_string(index);
    while (text.size() < 3) text.insert(text.begin(), '0');
    return text;
}

// 目录名不重复：已存在就追加 -2 / -3 …
std::filesystem::path unique_dir(const std::filesystem::path& parent, const std::string& name) {
    std::error_code ec;
    std::filesystem::path candidate = parent / name;
    for (int i = 2; i < 1000; ++i) {
        if (!std::filesystem::exists(candidate, ec)) return candidate;
        candidate = parent / (name + "-" + std::to_string(i));
    }
    return parent / (name + "-" + std::to_string(now_seconds()));
}

std::filesystem::path unique_file(const std::filesystem::path& parent, const std::string& stem,
                                  const std::string& ext) {
    std::error_code ec;
    std::filesystem::path candidate = parent / (stem + ext);
    for (int i = 2; i < 1000; ++i) {
        if (!std::filesystem::exists(candidate, ec)) return candidate;
        candidate = parent / (stem + "-" + std::to_string(i) + ext);
    }
    return parent / (stem + "-" + std::to_string(now_seconds()) + ext);
}

}  // namespace

std::string share_safe_filename(std::string_view name, const std::string& fallback) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|' || u < 0x20 || u == 0x7F) {
            out.push_back('_');
        } else {
            out.push_back(c);
        }
    }
    const size_t begin = out.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        out.clear();
    } else if (begin > 0) {
        out.erase(0, begin);
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '\t' || out.back() == '\r' ||
                            out.back() == '\n' || out.back() == '.')) {
        out.pop_back();
    }
    if (out.size() > 80) {
        size_t cut = 80;
        // 往回退到字符起始字节，避免把 UTF-8 多字节字符切成半个
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
        out.resize(cut);
    }
    if (out.empty() || out == "." || out == "..") return fallback;
    return out;
}

std::string share_kind_of_filename(std::string_view filename) {
    const std::string ext = lower_ascii(path_to_utf8(std::filesystem::path(std::string(filename)).extension()));
    if (ext == ".rdl") return "playlist";
    if (ext == ".rdm") return "package";
    return std::string();
}

// ---------------------------------------------------------------- .rdl

json::Value share_rdl_from_playlist(const Playlist& playlist) {
    json::Value v = json::Value::object();
    v.set("format", kSharePlaylistFormat);
    v.set("version", static_cast<long long>(kShareFormatVersion));
    v.set("generator", std::string("Rad Music ") + version_string());
    v.set("name", playlist.name);
    v.set("description", playlist.description);
    v.set("pic", playlist.pic);
    v.set("owner", playlist.owner);
    v.set("platform", playlist.platform);
    v.set("created_at", playlist.created_at);
    v.set("updated_at", playlist.updated_at);
    v.set("track_count", static_cast<long long>(playlist.tracks.size()));
    v.set("total_duration", playlist.total_duration_seconds());
    json::Value tracks = json::Value::array();
    for (const auto& track : playlist.tracks) tracks.push_back(track.to_json());
    v.set("tracks", tracks);
    return v;
}

Result<Playlist> share_playlist_from_rdl(const json::Value& value) {
    if (!value.is_object()) {
        return Result<Playlist>::failure(ErrorCode::InvalidMetadata, "不是有效的 .rdl 歌单文件（应为 JSON 对象）");
    }
    // 有 format 字段就校验，没写则按「裸歌单」宽容接受
    const std::string format = value.get_string("format");
    if (!format.empty() && format != kSharePlaylistFormat) {
        return Result<Playlist>::failure(ErrorCode::InvalidMetadata, "不支持的文件格式: " + format);
    }
    if (value.get_int("version", kShareFormatVersion) > kShareFormatVersion) {
        return Result<Playlist>::failure(ErrorCode::InvalidMetadata,
                                         "歌单文件版本过新（" + std::to_string(value.get_int("version", 1)) +
                                             "），请升级 Rad Music 后再导入");
    }
    const json::Value* list = value.find("tracks");
    if (list == nullptr || !list->is_array()) {
        return Result<Playlist>::failure(ErrorCode::InvalidMetadata, "歌单文件缺少 tracks 数组");
    }

    Playlist playlist;
    playlist.name = value.get_string("name");
    if (playlist.name.empty()) playlist.name = "导入的歌单";
    playlist.description = value.get_string("description");
    playlist.pic = value.get_string("pic");
    playlist.owner = "user";
    playlist.platform = "local";   // 导入后的歌单是本地的，不再随在线源同步
    for (const auto& item : list->as_array()) {
        if (!item.is_object()) continue;
        PlaylistTrack track = PlaylistTrack::from_json(item);
        if (track.name.empty()) continue;
        if (track.item_id.empty()) track.item_id = track.sha;
        if (track.item_id.empty()) continue;
        if (track.platform.empty()) track.platform = "local";
        if (track.added_at <= 0) track.added_at = now_seconds();
        playlist.tracks.push_back(std::move(track));
    }
    if (playlist.tracks.empty()) {
        return Result<Playlist>::failure(ErrorCode::InvalidMetadata, "歌单文件里没有可导入的曲目");
    }
    return Result<Playlist>::success(std::move(playlist));
}

// ---------------------------------------------------------------- 导出

Result<ShareExportResult> share_export_tracks(const LibraryPaths& paths, const MetaCache& meta,
                                              const std::vector<std::string>& shas,
                                              const ShareExportOptions& options) {
    if (shas.empty()) {
        return Result<ShareExportResult>::failure(ErrorCode::InvalidMetadata, "没有选择要导出的曲目");
    }

    // 先解析出全部可导出的曲目（在线未落盘 / 文件缺失的直接跳过并记原因）
    struct Source {
        BaseInfo base;
        std::filesystem::path file;
    };
    std::vector<Source> sources;
    ShareExportResult result;
    for (const auto& sha : shas) {
        auto path = meta.find_by_sha(sha);
        if (!path.has_value()) {
            result.skipped.push_back(sha + "：曲库里没有该 sha");
            continue;
        }
        auto base = meta.read(*path);
        if (!base.ok()) {
            result.skipped.push_back(sha + "：" + base.status().message);
            continue;
        }
        std::filesystem::path file = resolve_audio_file(paths, base.value());
        if (file.empty()) {
            result.skipped.push_back((base.value().name.empty() ? sha : base.value().name) +
                                     "：没有本地音频文件（在线曲目请先下载）");
            continue;
        }
        sources.push_back(Source{base.value(), file});
    }
    if (sources.empty()) {
        return Result<ShareExportResult>::failure(ErrorCode::FileNotFound,
                                                  "没有可导出的本地音频文件（在线曲目请先下载到曲库）");
    }

    std::error_code ec;
    std::filesystem::create_directories(paths.exports(), ec);
    if (ec) {
        return Result<ShareExportResult>::failure(ErrorCode::RequestFailed,
                                                  "无法创建导出目录: " + path_to_utf8(paths.exports()));
    }

    const std::string stem = share_safe_filename(
        options.name.empty() ? ("曲库导出-" + timestamp_tag()) : options.name, "radmusic-export");

    if (!options.as_package) {
        // 常规文件形式：exports/<名字>/<序号> - <歌名> - <歌手>.<ext>
        const std::filesystem::path dir = unique_dir(paths.exports(), stem);
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            return Result<ShareExportResult>::failure(ErrorCode::RequestFailed,
                                                      "无法创建导出目录: " + path_to_utf8(dir));
        }
        std::set<std::string> used_names;
        size_t index = 0;
        for (const auto& source : sources) {
            ++index;
            const std::string ext = file_extension_lower(source.file);
            std::string file_name = pad_index(index) + " - " +
                                    share_safe_filename(source.base.name, "未命名") + " - " +
                                    share_safe_filename(source.base.author, "未知艺术家") + ext;
            std::string unique = file_name;
            int dup = 1;
            while (!used_names.insert(lower_ascii(unique)).second) {
                ++dup;
                unique = pad_index(index) + " - " + share_safe_filename(source.base.name, "未命名") +
                         " - " + share_safe_filename(source.base.author, "未知艺术家") + " (" +
                         std::to_string(dup) + ")" + ext;
            }
            const std::filesystem::path target = dir / unique;
            std::error_code copy_ec;
            std::filesystem::copy_file(source.file, target,
                                       std::filesystem::copy_options::overwrite_existing, copy_ec);
            if (copy_ec) {
                result.skipped.push_back(source.base.name + "：复制失败（" + copy_ec.message() + "）");
                continue;
            }
            ++result.count;
            std::error_code size_ec;
            result.bytes += static_cast<long long>(std::filesystem::file_size(target, size_ec));
            if (size_ec) result.bytes += static_cast<long long>(std::filesystem::file_size(source.file, size_ec));
            result.files.push_back(path_to_utf8(target));
        }
        result.dir = path_to_utf8(normalized(dir));
        if (result.count == 0) {
            return Result<ShareExportResult>::failure(ErrorCode::RequestFailed, "导出失败：没有文件被写入");
        }
        return Result<ShareExportResult>::success(std::move(result));
    }

    // 音频包（.rdm）：base.json + music/<音频> + covers/<封面>
    const std::filesystem::path target = unique_file(paths.exports(), stem, kSharePackageExt);
    std::vector<archive::ZipWriteItem> items;
    json::Value track_list = json::Value::array();
    std::set<std::string> used_names;
    std::set<std::string> added_covers;
    size_t index = 0;

    for (const auto& source : sources) {
        ++index;
        const std::string ext = file_extension_lower(source.file);
        std::string file_name = pad_index(index) + " - " + share_safe_filename(source.base.name, "未命名") +
                                " - " + share_safe_filename(source.base.author, "未知艺术家") + ext;
        std::string unique = file_name;
        int dup = 1;
        while (!used_names.insert(lower_ascii(unique)).second) {
            ++dup;
            unique = pad_index(index) + " - " + share_safe_filename(source.base.name, "未命名") + " - " +
                     share_safe_filename(source.base.author, "未知艺术家") + " (" + std::to_string(dup) +
                     ")" + ext;
        }
        const std::string arcname = std::string(kShareMusicDir) + "/" + unique;

        json::Value entry = json::Value::object();
        entry.set("platform", source.base.platform);
        entry.set("sha", source.base.sha);
        entry.set("name", source.base.name);
        entry.set("author", source.base.author);
        if (!source.base.album.empty()) entry.set("album", source.base.album);
        entry.set("duration", static_cast<long long>(source.base.duration_seconds));
        if (!source.base.item_id.empty()) entry.set("item_id", source.base.item_id);
        entry.set("quality", source.base.quality.to_json());
        entry.set("file", arcname);
        if (!source.base.pic.empty()) entry.set("pic", source.base.pic);

        // 封面：只带曲库内 covers/ 下的文件（在线 URL / data: 原样留在 pic 里）
        std::filesystem::path cover;
        std::string cover_arc;
        if (source.base.pic.rfind("covers/", 0) == 0) {
            const std::filesystem::path candidate = paths.root / source.base.pic;
            std::error_code cover_ec;
            if (std::filesystem::is_regular_file(candidate, cover_ec) && !cover_ec) {
                cover = candidate;
                cover_arc = source.base.pic;
            }
        }
        if (!cover.empty()) {
            if (added_covers.insert(cover_arc).second) {
                archive::ZipWriteItem cover_item;
                cover_item.name = cover_arc;
                cover_item.path = cover;
                items.push_back(std::move(cover_item));
            }
            entry.set("cover", cover_arc);
        }

        archive::ZipWriteItem item;
        item.name = arcname;
        item.path = source.file;
        items.push_back(std::move(item));
        track_list.push_back(entry);

        std::error_code size_ec;
        result.bytes += static_cast<long long>(std::filesystem::file_size(source.file, size_ec));
        ++result.count;
    }

    json::Value manifest = json::Value::object();
    manifest.set("format", kSharePackageFormat);
    manifest.set("version", static_cast<long long>(kShareFormatVersion));
    manifest.set("generator", std::string("Rad Music ") + version_string());
    manifest.set("created_at", now_seconds());
    manifest.set("name", options.name.empty() ? stem : options.name);
    manifest.set("description", options.playlist_description);
    manifest.set("track_count", static_cast<long long>(result.count));
    manifest.set("total_bytes", result.bytes);
    if (!options.playlist_name.empty()) {
        json::Value playlist = json::Value::object();
        playlist.set("name", options.playlist_name);
        playlist.set("description", options.playlist_description);
        manifest.set("playlist", playlist);
    }
    manifest.set("tracks", track_list);

    archive::ZipWriteItem manifest_item;
    manifest_item.name = kShareManifestName;
    manifest_item.data = manifest.dump(2);
    items.insert(items.begin(), std::move(manifest_item));

    std::string zip_error;
    if (!archive::write_zip(target, items, &zip_error)) {
        std::error_code rm_ec;
        std::filesystem::remove(target, rm_ec);
        return Result<ShareExportResult>::failure(ErrorCode::RequestFailed, "打包失败: " + zip_error);
    }

    std::error_code size_ec;
    const auto packed_size = std::filesystem::file_size(target, size_ec);
    result.file = path_to_utf8(normalized(target));
    result.dir = path_to_utf8(normalized(target.parent_path()));
    result.files.push_back(result.file);
    if (!size_ec) result.bytes = static_cast<long long>(packed_size);
    return Result<ShareExportResult>::success(std::move(result));
}

// ---------------------------------------------------------------- 导入 .rdm

Result<SharePackage> share_open_package(const LibraryPaths& paths, const std::string& zip_bytes) {
    if (zip_bytes.size() < 32) {
        return Result<SharePackage>::failure(ErrorCode::InvalidMetadata, "音频包内容为空或文件已损坏");
    }
    std::error_code ec;
    const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    SharePackage package;
    package.dir = paths.tmp() / ("package_" + std::to_string(stamp));
    std::filesystem::create_directories(package.dir, ec);
    if (ec) {
        return Result<SharePackage>::failure(ErrorCode::RequestFailed, "无法创建临时目录（检查磁盘权限）");
    }
    const std::filesystem::path zip_file = package.dir / "package.rdm";
    {
        std::ofstream out(zip_file, std::ios::binary | std::ios::trunc);
        if (!out) {
            std::filesystem::remove_all(package.dir, ec);
            return Result<SharePackage>::failure(ErrorCode::RequestFailed, "无法写入临时文件");
        }
        out.write(zip_bytes.data(), static_cast<std::streamsize>(zip_bytes.size()));
        out.flush();
        if (!out.good()) {
            std::filesystem::remove_all(package.dir, ec);
            return Result<SharePackage>::failure(ErrorCode::RequestFailed, "临时文件写入失败（磁盘空间不足？）");
        }
    }

    const std::filesystem::path content = package.dir / "content";
    archive::ExtractOptions options;
    options.strip_single_root = true;   // 兼容「整包再套一层目录」的重新打包
    const archive::ExtractResult extracted = archive::extract_zip_streamed(zip_file, content, options);
    if (!extracted.ok()) {
        std::filesystem::remove_all(package.dir, ec);
        std::filesystem::remove(zip_file, ec);
        return Result<SharePackage>::failure(ErrorCode::InvalidMetadata,
                                             "音频包解压失败: " + extracted.error);
    }

    std::filesystem::path manifest_file = content / kShareManifestName;
    std::error_code ex_ec;
    if (!std::filesystem::is_regular_file(manifest_file, ex_ec)) {
        // 容错：清单被改名时，取根目录下第一个 .json
        manifest_file.clear();
        std::filesystem::directory_iterator it(content, ec);
        const std::filesystem::directory_iterator end;
        while (!ec && it != end) {
            const std::filesystem::path candidate = it->path();
            std::error_code fec;
            const bool regular = it->is_regular_file(fec);
            it.increment(ec);
            if (regular && lower_ascii(path_to_utf8(candidate.extension())) == ".json") {
                manifest_file = candidate;
                break;
            }
        }
    }
    if (manifest_file.empty() || !std::filesystem::is_regular_file(manifest_file, ex_ec)) {
        std::filesystem::remove_all(package.dir, ec);
        return Result<SharePackage>::failure(ErrorCode::InvalidMetadata,
                                             "音频包内缺少 base.json 清单，不是有效的 .rdm 文件");
    }

    auto text = read_text_file(manifest_file);
    if (!text.ok()) {
        std::filesystem::remove_all(package.dir, ec);
        return Result<SharePackage>::failure(ErrorCode::InvalidMetadata, "无法读取音频包清单");
    }
    json::ParseError parse_error;
    package.manifest = json::Value::parse(text.value(), &parse_error);
    if (!package.manifest.is_object()) {
        std::filesystem::remove_all(package.dir, ec);
        return Result<SharePackage>::failure(ErrorCode::InvalidMetadata,
                                             "音频包清单不是合法 JSON: " + parse_error.message);
    }
    const std::string format = package.manifest.get_string("format");
    if (!format.empty() && format != kSharePackageFormat) {
        std::filesystem::remove_all(package.dir, ec);
        return Result<SharePackage>::failure(ErrorCode::InvalidMetadata, "不支持的文件格式: " + format);
    }

    package.name = package.manifest.get_string("name");
    package.description = package.manifest.get_string("description");
    const json::Value& playlist = package.manifest.get("playlist");
    if (playlist.is_object()) {
        package.playlist_name = playlist.get_string("name");
        package.playlist_description = playlist.get_string("description");
    }
    const json::Value* list = package.manifest.find("tracks");
    package.declared_tracks = static_cast<int>(list != nullptr && list->is_array() ? list->size() : 0);

    const std::filesystem::path music_dir = content / kShareMusicDir;
    package.audio_dir = std::filesystem::is_directory(music_dir, ex_ec) ? music_dir : content;

    const std::filesystem::path covers_dir = content / "covers";
    if (std::filesystem::is_directory(covers_dir, ex_ec)) {
        std::filesystem::directory_iterator it(covers_dir, ec);
        const std::filesystem::directory_iterator end;
        while (!ec && it != end) {
            const std::filesystem::path candidate = it->path();
            std::error_code fec;
            const bool regular = it->is_regular_file(fec);
            it.increment(ec);
            if (regular) package.covers.push_back(candidate);
        }
    }
    return Result<SharePackage>::success(std::move(package));
}

void share_discard_package(const SharePackage& package) {
    if (package.dir.empty()) return;
    std::error_code ec;
    std::filesystem::remove_all(package.dir, ec);
}

size_t share_import_covers(const LibraryPaths& paths, const SharePackage& package) {
    if (package.covers.empty()) return 0;
    std::error_code ec;
    std::filesystem::create_directories(paths.covers(), ec);
    size_t copied = 0;
    for (const auto& cover : package.covers) {
        const std::filesystem::path target = paths.covers() / cover.filename();
        std::error_code ex_ec;
        if (std::filesystem::exists(target, ex_ec)) continue;
        std::error_code copy_ec;
        std::filesystem::copy_file(cover, target, std::filesystem::copy_options::none, copy_ec);
        if (!copy_ec) ++copied;
    }
    return copied;
}

}  // namespace radmusic
