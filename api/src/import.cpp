// Rad music API - 音乐导入（Readme.md 1.10）与导入 API 服务端（Readme.md 2.12）
//
// 设计要点：
//  * 严格按 1.10 的流程：扫描 → 解析（标签/文件名回退）→ 音质与时长 → SHA-256 →
//    构造 base.json → 冲突预检 → 落盘（tracks/、covers/、歌词）→ 写 cache/meta → 更新索引；
//  * base.json 唯一性（sha / location / group_key+音质）由 MetaCache::put 兜底保证，
//    本模块负责把策略与结果映射成冲突报告；
//  * scan() 与 dry_run 完全无副作用（不建目录、不写文件、不改索引）；
//  * 所有文件操作都做防御性检查，任何失败都以 Result/ErrorCode 表达，不抛异常；
//  * 只依赖标准库与项目内模块，零第三方依赖。
#include "radmusic/import.hpp"

#include <algorithm>
#include <fstream>
#include <system_error>
#include <utility>

#include "radmusic/crypto.hpp"
#include "radmusic/groupkey.hpp"
#include "radmusic/quality.hpp"

namespace radmusic {
namespace {

// ---------------------------------------------------------------- 路径工具

std::string path_utf8(const std::filesystem::path& p) {
    const std::string bytes = p.u8string();   // C++17: u8string() 为 std::string（UTF-8）
    return bytes;
}

std::string generic_utf8(const std::filesystem::path& p) { return path_utf8(p.generic_u8string()); }

std::string lower_ascii(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

std::string format_import_id(int value) {
    std::string digits = std::to_string(value < 0 ? 0 : value);
    if (digits.size() < 4) digits.insert(0, 4 - digits.size(), '0');
    return "imp_" + digits;
}

bool file_exists(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::exists(p, ec) && !ec;
}

bool is_readable(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return static_cast<bool>(in);
}

bool path_is_regular_file(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::is_regular_file(p, ec) && !ec;
}

// ---------------------------------------------------------------- 选项

std::string request_string(const json::Value& v, const char* key, const std::string& def) {
    if (!v.is_object()) return def;
    const json::Value* found = v.find(key);
    if (found == nullptr || !found->is_string()) return def;
    return found->as_string(def);
}

ImportOptions parse_options(const json::Value& request) {
    ImportOptions opt;
    if (!request.is_object()) return opt;

    if (const json::Value* paths = request.find("paths"); paths != nullptr) {
        if (paths->is_array()) {
            for (const auto& item : paths->as_array()) {
                if (item.is_string()) opt.paths.push_back(item.as_string());
            }
        } else if (paths->is_string() && !paths->as_string().empty()) {
            opt.paths.push_back(paths->as_string());   // 允许单个字符串
        }
    }

    opt.recursive = request.get_bool("recursive", opt.recursive);
    const std::string mode = request_string(request, "mode", opt.mode);
    if (mode == "copy" || mode == "move" || mode == "link") opt.mode = mode;
    const std::string conflict = request_string(request, "conflict", std::string());
    if (!conflict.empty()) opt.conflict = conflict_policy_from_id(conflict);
    opt.tag_fallback = request_string(request, "tag_fallback", opt.tag_fallback);
    opt.filename_template = request_string(request, "filename_template", opt.filename_template);
    opt.dry_run = request.get_bool("dry_run", opt.dry_run);
    return opt;
}

// ---------------------------------------------------------------- 扫描

struct FoundFile {
    std::filesystem::path path;
    std::string display;    // 原样保留调用方给的写法（报告里更可读）
};

// 不支持/不可用的候选（计入 scanned / failed；扩展名不支持 → 1025）
struct RejectedFile {
    std::string path;
    std::string reason;
    ErrorCode code = ErrorCode::UnsupportedFormat;
};

struct ScanResult {
    std::vector<FoundFile> files;
    std::vector<RejectedFile> rejected;
};

// 规范化显示路径：统一分隔符并去掉 "./" 前缀
std::string normalize_display(std::string text) {
    std::replace(text.begin(), text.end(), '\\', '/');
    while (text.rfind("./", 0) == 0) text.erase(0, 2);
    return text;
}

// 返回 false 表示目录无法遍历
bool collect_dir_entries(const std::filesystem::path& dir, bool recursive, ScanResult* out) {
    std::error_code ec;
    const auto accept = [out](const std::filesystem::directory_entry& entry) {
        const std::string shown = path_utf8(entry.path());
        if (!audio_container_supported(audio_container_from_extension(shown))) {
            out->rejected.push_back(RejectedFile{shown, "不支持的音频格式", ErrorCode::UnsupportedFormat});
            return;
        }
        out->files.push_back(FoundFile{entry.path(), shown});
    };

    if (recursive) {
        std::filesystem::recursive_directory_iterator it(
            dir, std::filesystem::directory_options::skip_permission_denied, ec);
        if (ec) return false;
        for (const auto& entry : it) {
            std::error_code type_ec;
            if (!entry.is_regular_file(type_ec) || type_ec) continue;
            accept(entry);
        }
        return true;
    }
    std::filesystem::directory_iterator it(
        dir, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) return false;
    for (const auto& entry : it) {
        std::error_code type_ec;
        if (!entry.is_regular_file(type_ec) || type_ec) continue;
        accept(entry);
    }
    return true;
}

void collect_path(const std::filesystem::path& p, const std::string& display, bool recursive,
                  ScanResult* out) {
    std::error_code ec;
    const std::string shown = display.empty() ? path_utf8(p) : display;
    if (!std::filesystem::exists(p, ec) || ec) {
        out->rejected.push_back(RejectedFile{shown, "路径不存在", ErrorCode::FileNotFound});
        return;
    }
    if (path_is_regular_file(p)) {
        if (!audio_container_supported(audio_container_from_extension(shown))) {
            out->rejected.push_back(RejectedFile{shown, "不支持的音频格式", ErrorCode::UnsupportedFormat});
            return;
        }
        out->files.push_back(FoundFile{p, shown});
        return;
    }
    if (!std::filesystem::is_directory(p, ec) || ec) {
        out->rejected.push_back(RejectedFile{shown, "不是文件或目录", ErrorCode::UnsupportedFormat});
        return;
    }
    if (!collect_dir_entries(p, recursive, out)) {
        out->rejected.push_back(RejectedFile{shown, "无法遍历目录", ErrorCode::RequestFailed});
    }
}

ScanResult enumerate(const ImportOptions& opt) {
    ScanResult out;
    for (const auto& raw : opt.paths) {
        if (raw.empty()) continue;
        collect_path(std::filesystem::path(raw), normalize_display(raw), opt.recursive, &out);
    }
    // 稳定顺序，保证可重复执行（1.10 规则 8）
    std::sort(out.files.begin(), out.files.end(), [](const FoundFile& a, const FoundFile& b) {
        return path_utf8(a.path) < path_utf8(b.path);
    });
    return out;
}

// ---------------------------------------------------------------- 文件名 / 封面

std::string extension_with_dot(const std::filesystem::path& p) {
    std::string ext = path_utf8(p.extension());
    if (!ext.empty() && ext[0] != '.') ext.insert(ext.begin(), '.');
    return ext;
}

// 目标音频文件名：优先 "<artist> - <title>"，其次源文件名，最后 audio<ext>
std::string audio_filename(const BaseInfo& base, const std::filesystem::path& src,
                           const std::string& ext) {
    std::string candidate;
    if (!base.author.empty() && !base.name.empty()) {
        candidate = base.author + " - " + base.name;
    } else if (!base.name.empty()) {
        candidate = base.name;
    }
    if (candidate.empty()) candidate = path_utf8(src.stem());
    std::string safe = fs_sanitize(candidate, 80);
    if (safe.empty() || safe == "unknown") safe = fs_sanitize(path_utf8(src.stem()), 80);
    if (safe.empty() || safe == "unknown") return "audio" + ext;
    return safe + ext;
}

std::string mime_extension(std::string_view mime) {
    const std::string m = lower_ascii(mime);
    if (m.find("png") != std::string::npos) return ".png";
    if (m.find("webp") != std::string::npos) return ".webp";
    if (m.find("gif") != std::string::npos) return ".gif";
    if (m.find("bmp") != std::string::npos) return ".bmp";
    if (m.find("tiff") != std::string::npos) return ".tiff";
    return ".jpg";
}

// 先按魔数判断，无法识别时再用 mime（mime 仅作提示）
std::string cover_extension(const std::vector<uint8_t>& data, std::string_view mime) {
    if (data.size() >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') {
        return ".png";
    }
    if (data.size() >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) return ".jpg";
    if (data.size() >= 6 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F') return ".gif";
    if (data.size() >= 12 && data[8] == 'W' && data[9] == 'E' && data[10] == 'B' && data[11] == 'P') {
        return ".webp";
    }
    if (data.size() >= 2 && data[0] == 'B' && data[1] == 'M') return ".bmp";
    return mime_extension(mime);
}

std::string cover_relative_path(const std::vector<uint8_t>& data, std::string_view mime) {
    const std::string digest =
        crypto::sha256_hex(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
    std::string hash16 = digest.substr(0, 16);
    if (hash16.size() < 16) hash16.append(16 - hash16.size(), '0');
    return "covers/" + hash16 + cover_extension(data, mime);
}

// ---------------------------------------------------------------- 文件写入

bool write_bytes_file(const std::filesystem::path& dir, const std::filesystem::path& target,
                      const std::vector<uint8_t>& data) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return false;
    const std::filesystem::path tmp = target.parent_path() / (path_utf8(target.filename()) + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        if (!data.empty()) {
            out.write(reinterpret_cast<const char*>(data.data()),
                      static_cast<std::streamsize>(data.size()));
        }
        out.flush();
        if (!out.good()) {
            out.close();
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    std::filesystem::rename(tmp, target, ec);
    if (!ec) return true;
    ec.clear();
    std::filesystem::remove(target, ec);
    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

bool copy_file_into(const std::filesystem::path& src, const std::filesystem::path& dir,
                    const std::filesystem::path& target) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return false;
    std::filesystem::copy_file(src, target, std::filesystem::copy_options::overwrite_existing, ec);
    return !ec;
}

bool file_same_size(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code ec1;
    std::error_code ec2;
    const auto sa = std::filesystem::file_size(a, ec1);
    const auto sb = std::filesystem::file_size(b, ec2);
    return !ec1 && !ec2 && sa == sb;
}

// ---------------------------------------------------------------- 单条计划

struct ItemPlan {
    bool fatal = false;
    ErrorCode code = ErrorCode::Ok;
    std::string message;

    std::string src_display;
    std::string src_native;         // 传给注入函数与文件 API 的本地路径字符串
    BaseInfo base;
    MetaPath meta_path;

    std::string action;             // 正式运行：created/merged/skipped/overwritten/failed
    std::string dry_action;         // dry_run：would_create/would_merge/would_skip/would_overwrite/would_fail
    std::string outcome_action;     // execute() 实际得到的动作
    bool copy_audio = false;        // 需要复制/移动音频
    std::filesystem::path audio_target;
    bool write_cover = false;
    std::vector<uint8_t> cover;
    bool write_lyric = false;
    std::string lyric_text;

    std::vector<ImportConflict> conflicts;
};

std::string map_outcome_action(WriteAction action) {
    switch (action) {
        case WriteAction::Created: return "created";
        case WriteAction::Merged: return "merged";
        case WriteAction::Skipped: return "skipped";
        default: return "overwritten";
    }
}

std::string map_conflict_action(WriteAction action) {
    switch (action) {
        case WriteAction::Merged: return "merge";
        case WriteAction::Overwritten: return "overwrite";
        default: return "skip";
    }
}

class ImportRun {
public:
    ImportRun(const LibraryPaths& paths, MetaCache& meta, ImportManager& owner, const ImportOptions& opt)
        : paths_(paths), meta_(meta), owner_(owner), opt_(opt) {}

    // 解析单个文件 → 计划（只读，不写盘）。scan 与正式运行共用。
    ItemPlan plan(const std::filesystem::path& path, const std::string& display) {
        ItemPlan out;
        out.src_display = display.empty() ? path_utf8(path) : display;
        out.src_native = path_utf8(path);

        std::error_code ec;
        if (!std::filesystem::exists(path, ec) || ec) {
            out.fatal = true;
            out.code = ErrorCode::FileNotFound;
            out.message = "源文件不存在: " + out.src_display;
            return out;
        }
        if (!path_is_regular_file(path)) {
            out.fatal = true;
            out.code = ErrorCode::UnsupportedFormat;
            out.message = "不是普通文件: " + out.src_display;
            return out;
        }
        const AudioContainer container = audio_container_from_extension(out.src_display);
        if (!audio_container_supported(container)) {
            out.fatal = true;
            out.code = ErrorCode::UnsupportedFormat;
            out.message = "不支持的音频格式: " + out.src_display;
            return out;
        }
        if (!is_readable(path)) {
            out.fatal = true;
            out.code = ErrorCode::FileNotFound;
            out.message = "源文件不可读: " + out.src_display;
            return out;
        }

        // ---- 2) 标签解析：失败按错误码分流，解析失败回退文件名（1024 非致命） ----
        TagData tags;
        const Result<TagData> tag_result =
            owner_.read_tags_fn ? owner_.read_tags_fn(out.src_native) : read_tags(out.src_native);
        if (tag_result.ok()) {
            tags = tag_result.value();
        } else {
            const ErrorCode code = tag_result.status().code;
            if (code == ErrorCode::FileNotFound) {
                out.fatal = true;
                out.code = ErrorCode::FileNotFound;
                out.message = "读取标签失败（文件不存在）: " + out.src_display;
                return out;
            }
            const bool fallback_allowed = opt_.tag_fallback.empty() || opt_.tag_fallback == "filename";
            if (!fallback_allowed) {
                out.fatal = true;
                out.code = code;
                out.message = "标签解析失败且未启用文件名回退: " + out.src_display;
                return out;
            }
            tags = tags_from_filename(out.src_native, opt_.filename_template);
            tags.tag_source = "filename";
            ImportConflict c;
            c.type = "tag_parse";
            c.path = out.src_display;
            c.code = code;
            c.action = "fallback";
            c.message = tag_result.status().response_message();
            out.conflicts.push_back(std::move(c));
        }

        // ---- 3) 音质与时长 ----
        AudioStreamInfo info;
        const Result<AudioStreamInfo> probe_result =
            owner_.probe_audio_fn ? owner_.probe_audio_fn(out.src_native) : probe_audio(out.src_native);
        if (probe_result.ok()) info = probe_result.value();

        const Quality quality = quality_from_stream(info);
        int duration_seconds = tags.duration_seconds;
        if (info.duration_ms > 0) {
            duration_seconds = static_cast<int>((info.duration_ms + 500) / 1000);   // 四舍五入成秒
        }
        if (duration_seconds < 0) duration_seconds = 0;

        // ---- 4) SHA-256 ----
        std::string sha;
        if (owner_.sha_fn) {
            sha = owner_.sha_fn(out.src_native);
        } else {
            bool ok = false;
            sha = crypto::sha256_file_hex(out.src_native, &ok);
            if (!ok) sha.clear();
        }
        sha = lower_ascii(sha);
        if (sha.size() != 64) {
            out.fatal = true;
            out.code = ErrorCode::RequestFailed;
            out.message = "无法计算文件 SHA-256: " + out.src_display;
            return out;
        }

        // ---- 5) base.json 草案 ----
        const std::string ext = extension_with_dot(path);
        BaseInfo base;
        base.platform = "local";
        base.version = "1.0.0";
        base.sha = sha;
        base.name = tags.title;
        base.author = tags.artist;
        base.album = tags.album;
        base.track_no = tags.track_no;
        base.tag_source = tags.tag_source.empty() ? "filename" : tags.tag_source;
        base.duration_seconds = duration_seconds;
        base.quality = quality;
        base.secret = 0;

        const std::string audio_name = audio_filename(base, path, ext);
        const std::filesystem::path track_dir = paths_.track_dir(sha);
        const std::filesystem::path audio_target = track_dir / audio_name;

        // ---- 6) 导入模式与 location ----
        if (opt_.mode == "link") {
            // 原地引用：location 写源文件的绝对路径
            std::error_code abs_ec;
            const std::filesystem::path abs = std::filesystem::absolute(path, abs_ec);
            base.location = generic_utf8(abs_ec ? path : abs);
        } else {
            // copy / move：location 写相对曲库根的路径
            base.location = generic_utf8(std::filesystem::path("tracks") / sha / audio_name);
        }
        out.audio_target = audio_target;

        // ---- 7) 封面 ----
        if (!tags.cover.empty()) {
            base.pic = cover_relative_path(tags.cover, tags.cover_mime);
            out.cover = tags.cover;
            out.write_cover = true;
        }
        // ---- 8) 歌词 ----
        if (!tags.lyrics_text.empty()) {
            out.write_lyric = true;
            out.lyric_text = tags.lyrics_text;
        }
        out.base = base;

        // ---- 冲突预检（只读） ----
        const MetaPath target_path = meta_.path_for(base);
        out.meta_path = target_path;
        if (!target_path.valid()) {
            out.fatal = true;
            out.code = ErrorCode::MetaPathMismatch;
            out.message = "无法推导 cache/meta 路径: " + out.src_display;
            return out;
        }

        const std::optional<MetaPath> by_sha = meta_.find_by_sha(sha);
        const std::optional<MetaPath> by_location = find_by_location(base.location);
        const std::optional<MetaPath> by_song_quality =
            meta_.find_by_group_quality(base.group_key(), target_path.quality_key);

        ImportConflict pre;
        if (by_sha.has_value()) {
            pre.type = "duplicate_sha";
            pre.action = "skip";
            pre.code = ErrorCode::Ok;
            pre.message = "同内容已入库（sha 全局唯一）";
            pre.existing = generic_utf8(paths_.meta_file(by_sha.value()));
        } else if (by_location.has_value()) {
            pre.type = "location";
            pre.action = (opt_.conflict == ConflictPolicy::Overwrite) ? "overwrite" : "skip";
            pre.code = ErrorCode::BaseConflict;
            pre.message = "location 已被其它 base.json 占用";
            pre.existing = generic_utf8(paths_.meta_file(by_location.value()));
        } else if (by_song_quality.has_value()) {
            pre.type = "same_song_same_quality";
            pre.action = conflict_action_id();
            pre.code = ErrorCode::BaseConflict;
            pre.message = "同一首歌同一音质已存在不同内容（sha 不同）";
            pre.existing = generic_utf8(paths_.meta_file(by_song_quality.value()));
        }
        const bool has_conflict =
            by_sha.has_value() || by_location.has_value() || by_song_quality.has_value();
        if (has_conflict) {
            pre.sha = sha;
            pre.path = out.src_display;
            out.conflicts.push_back(pre);
        }

        // ---- 决策：本次会做什么（正式运行与 dry_run 共用） ----
        const bool same_song_quality =
            !by_sha.has_value() && !by_location.has_value() && by_song_quality.has_value();

        if (by_sha.has_value()) {
            // 同内容重复导入：不重复建目录、不重复复制（1.10 冲突表第 1 行）
            out.action = "skipped";
            out.dry_action = "would_skip";
        } else if (same_song_quality && opt_.conflict == ConflictPolicy::Skip) {
            // 同曲同质不同内容：默认拒绝覆写（1023，非致命，计入 failed）
            out.fatal = true;
            out.code = ErrorCode::BaseConflict;
            out.message = "同一首歌同一音质已存在不同内容（1023）";
            out.action = "failed";
            out.dry_action = "would_fail";
        } else if (by_location.has_value() && opt_.conflict == ConflictPolicy::Skip) {
            // 同一物理文件不得有第二条记录（1.10 规则 1）
            out.fatal = true;
            out.code = ErrorCode::BaseConflict;
            out.message = "location 已被其它 base.json 占用且策略为 skip（1023）";
            out.action = "failed";
            out.dry_action = "would_fail";
        } else {
            out.action = same_song_quality
                             ? (opt_.conflict == ConflictPolicy::Overwrite ? "overwritten" : "merged")
                             : "created";
            out.dry_action = same_song_quality
                                 ? (opt_.conflict == ConflictPolicy::Overwrite ? "would_overwrite"
                                                                               : "would_merge")
                                 : "would_create";
            out.copy_audio = (opt_.mode != "link");
            // 幂等：tracks/<sha>/ 已有同名同大小文件就不再复制
            if (out.copy_audio && file_exists(audio_target) && file_same_size(path, audio_target)) {
                out.copy_audio = false;
            }
        }
        return out;
    }

    // 执行计划（仅在 dry_run=false 时调用）；结果写入 plan.outcome_action
    void execute(ItemPlan& plan) {
        if (plan.fatal) return;
        std::error_code ec;

        // 音频落盘：copy / move
        if (plan.copy_audio && opt_.mode != "link") {
            const std::filesystem::path target = plan.audio_target;
            const std::filesystem::path dir = target.parent_path();
            if (opt_.mode == "move") {
                std::filesystem::create_directories(dir, ec);
                if (ec) {
                    fail(plan, ErrorCode::RequestFailed,
                         "无法创建目录: " + generic_utf8(dir) + " (" + ec.message() + ")");
                    return;
                }
                bool moved = false;
                if (!file_exists(target)) {
                    std::filesystem::rename(std::filesystem::path(plan.src_native), target, ec);
                    moved = !ec;
                    ec.clear();   // 跨卷等失败：退化为复制 + 删除源
                }
                if (!moved) {
                    if (!copy_file_into(std::filesystem::path(plan.src_native), dir, target)) {
                        fail(plan, ErrorCode::RequestFailed, "无法移动到曲库: " + generic_utf8(target));
                        return;
                    }
                    // 只有内容进入曲库后才删除源文件；删除失败不致命
                    std::error_code rm_ec;
                    std::filesystem::remove(std::filesystem::path(plan.src_native), rm_ec);
                    if (rm_ec) plan.message = "源文件删除失败（内容已进入曲库）: " + plan.src_display;
                }
            } else {
                if (!copy_file_into(std::filesystem::path(plan.src_native), dir, target)) {
                    fail(plan, ErrorCode::RequestFailed, "无法复制到曲库: " + generic_utf8(target));
                    return;
                }
            }
        }

        // 封面：covers/<sha256 前 16 hex>.<ext>
        if (plan.write_cover && !plan.base.pic.empty()) {
            const std::filesystem::path cover_path = paths_.root / plan.base.pic;
            if (!write_bytes_file(cover_path.parent_path(), cover_path, plan.cover)) {
                plan.base.pic.clear();   // 非致命：清空 pic，避免指向不存在的文件
                plan.message = "封面写入失败: " + generic_utf8(cover_path);
            }
        }

        // 歌词：tracks/<sha>/lyric.lrc
        if (plan.write_lyric) {
            const std::filesystem::path lyric = paths_.lyric_file(plan.base.sha);
            const Status st = write_text_file_atomic(lyric.parent_path(), lyric, plan.lyric_text);
            if (!st.ok()) plan.message = "歌词写入失败: " + st.response_message();
        }

        // 入库：唯一性与防覆写由 MetaCache::put 兜底
        const Result<PutOutcome> put = meta_.put(plan.base, opt_.conflict);
        if (!put.ok()) {
            const ErrorCode code = put.status().code;
            ImportConflict c;
            c.type = (code == ErrorCode::BaseConflict) ? "same_song_same_quality" : "meta_write";
            c.sha = plan.base.sha;
            c.path = plan.src_display;
            c.existing = generic_utf8(paths_.meta_file(plan.meta_path));
            c.action = "failed";
            c.code = code;
            c.message = put.status().response_message();
            plan.conflicts.push_back(std::move(c));
            fail(plan, code, put.status().response_message());
            return;
        }

        const PutOutcome& outcome = put.value();
        plan.base = outcome.base;
        plan.meta_path = outcome.path;
        plan.outcome_action = map_outcome_action(outcome.action);

        if (outcome.action != WriteAction::Created) {
            ImportConflict c;
            c.type = (outcome.action == WriteAction::Skipped) ? "duplicate_sha" : "same_song_same_quality";
            c.sha = outcome.base.sha;
            c.path = plan.src_display;
            c.existing = generic_utf8(paths_.meta_file(outcome.path));
            c.action = map_conflict_action(outcome.action);
            c.code = ErrorCode::Ok;
            c.message = (outcome.action == WriteAction::Skipped)
                            ? std::string("同内容已入库，未重复建目录")
                            : std::string("同曲同质条目已按 conflict 策略处理");
            plan.conflicts.push_back(std::move(c));
        }
    }

private:
    static void fail(ItemPlan& plan, ErrorCode code, const std::string& message) {
        plan.fatal = true;
        plan.code = code;
        plan.message = message;
        plan.outcome_action = "failed";
    }

    std::string conflict_action_id() const {
        switch (opt_.conflict) {
            case ConflictPolicy::Merge: return "merge";
            case ConflictPolicy::Overwrite: return "overwrite";
            default: return "skip";
        }
    }

    std::optional<MetaPath> find_by_location(std::string_view location) const {
        if (location.empty()) return std::nullopt;
        for (const auto& [path, base] : meta_.list_all_with_paths()) {
            if (base.location == location) return path;
        }
        return std::nullopt;
    }

    const LibraryPaths& paths_;
    MetaCache& meta_;
    ImportManager& owner_;
    ImportOptions opt_;
};

// ---------------------------------------------------------------- 报告辅助

void push_rejected(ImportReport* report, const RejectedFile& rejected) {
    ++report->scanned;
    ++report->failed;
    ImportItemResult item;
    item.path = rejected.path;
    item.action = "failed";
    item.code = rejected.code;
    item.message = rejected.reason;
    report->items.push_back(std::move(item));

    ImportConflict c;
    c.type = "scan";
    c.path = rejected.path;
    c.action = "failed";
    c.code = rejected.code;
    c.message = rejected.reason;
    report->conflicts.push_back(std::move(c));
}

}  // namespace

// ---------------------------------------------------------------- JSON

ImportOptions ImportOptions::from_json(const json::Value& v) { return parse_options(v); }

json::Value ImportOptions::to_json() const {
    json::Value out = json::Value::object();
    json::Value paths_value = json::Value::array();
    for (const auto& p : paths) paths_value.push_back(p);
    out.set("paths", paths_value);
    out.set("recursive", recursive);
    out.set("mode", mode);
    out.set("conflict", std::string(conflict_policy_id(conflict)));
    out.set("tag_fallback", tag_fallback);
    out.set("filename_template", filename_template);
    out.set("dry_run", dry_run);
    return out;
}

json::Value ImportConflict::to_json() const {
    json::Value out = json::Value::object();
    out.set("type", type);
    if (!sha.empty()) out.set("sha", sha);
    if (!existing.empty()) out.set("existing", existing);
    out.set("path", path);
    out.set("action", action);
    out.set("code", static_cast<long long>(static_cast<int>(code)));
    if (!message.empty()) out.set("message", message);
    return out;
}

json::Value ImportItemResult::to_json() const {
    json::Value out = json::Value::object();
    out.set("path", path);
    if (!sha.empty()) out.set("sha", sha);
    if (!song_key.empty()) out.set("song_key", song_key);
    if (!quality_key.empty()) out.set("quality_key", quality_key);
    out.set("action", action);
    out.set("code", static_cast<long long>(static_cast<int>(code)));
    if (!message.empty()) out.set("message", message);
    return out;
}

json::Value ImportReport::to_json() const {
    json::Value out = json::Value::object();
    out.set("import_id", import_id);
    out.set("scanned", static_cast<long long>(scanned));
    out.set("imported", static_cast<long long>(imported));
    out.set("skipped", static_cast<long long>(skipped));
    out.set("failed", static_cast<long long>(failed));
    out.set("total", static_cast<long long>(scanned));
    out.set("dry_run", dry_run);
    json::Value conflicts_value = json::Value::array();
    for (const auto& c : conflicts) conflicts_value.push_back(c.to_json());
    out.set("conflicts", conflicts_value);
    json::Value items_value = json::Value::array();
    for (const auto& i : items) items_value.push_back(i.to_json());
    out.set("items", items_value);
    return out;
}

// ---------------------------------------------------------------- ImportManager

ImportManager::ImportManager(const LibraryPaths& paths, MetaCache& meta, LibraryIndex& index)
    : paths_(paths), meta_(meta), index_(index) {
    read_tags_fn = [](const std::string& path) { return read_tags(path); };
    probe_audio_fn = [](const std::string& path) { return probe_audio(path); };
    sha_fn = [](const std::string& path) {
        bool ok = false;
        const std::string sha = crypto::sha256_file_hex(path, &ok);
        return ok ? sha : std::string();
    };
}

Result<json::Value> ImportManager::scan(const json::Value& request) {
    const ImportOptions opt = parse_options(request);
    ImportReport report;
    report.import_id = format_import_id(next_id_++);
    report.dry_run = true;   // scan 一律不写盘

    const ScanResult found = enumerate(opt);
    ImportRun runner(paths_, meta_, *this, opt);

    for (const auto& rejected : found.rejected) push_rejected(&report, rejected);

    for (const auto& file : found.files) {
        ++report.scanned;
        ItemPlan plan = runner.plan(file.path, file.display);

        ImportItemResult item;
        item.path = plan.src_display;
        item.sha = plan.base.sha;
        item.song_key = plan.meta_path.song_key;
        item.quality_key = plan.meta_path.quality_key;

        for (auto& c : plan.conflicts) report.conflicts.push_back(std::move(c));

        if (plan.fatal) {
            ++report.failed;
            item.action = "failed";
            item.code = plan.code;
            item.message = plan.message;
        } else {
            item.action = plan.dry_action;
            item.code = ErrorCode::Ok;
            if (plan.dry_action == "would_skip") {
                ++report.skipped;
            } else if (plan.dry_action == "would_create" || plan.dry_action == "would_merge" ||
                       plan.dry_action == "would_overwrite") {
                ++report.imported;
            }
        }
        report.items.push_back(std::move(item));
    }

    reports_.push_back(std::move(report));
    return Result<json::Value>::success(reports_.back().to_json());
}

Result<json::Value> ImportManager::run(const json::Value& request) {
    const ImportOptions opt = parse_options(request);

    // 12) 库锁：串行化提交（扫描阶段不需要锁）
    if (!opt.dry_run) {
        LibraryLock lock(paths_);
        const Status locked = lock.acquire(2000);
        if (!locked.ok()) {
            return Result<json::Value>::failure(locked.code, locked.response_message());
        }
        const Status layout = paths_.ensure_layout();
        if (!layout.ok()) return Result<json::Value>::failure(layout.code, layout.response_message());
        return run_locked(opt);
    }
    return run_locked(opt);
}

Result<json::Value> ImportManager::run_locked(const ImportOptions& opt) {
    ImportReport report;
    report.import_id = format_import_id(next_id_++);
    report.dry_run = opt.dry_run;

    const ScanResult found = enumerate(opt);
    ImportRun runner(paths_, meta_, *this, opt);
    std::vector<PutOutcome> indexed;

    for (const auto& rejected : found.rejected) push_rejected(&report, rejected);

    for (const auto& file : found.files) {
        ++report.scanned;
        ItemPlan plan = runner.plan(file.path, file.display);

        ImportItemResult item;
        item.path = plan.src_display;

        if (!opt.dry_run && !plan.fatal) {
            runner.execute(plan);
            item.sha = plan.base.sha;
            item.song_key = plan.meta_path.song_key;
            item.quality_key = plan.meta_path.quality_key;
            if (!plan.fatal) {
                item.action = plan.outcome_action.empty() ? plan.action : plan.outcome_action;
                PutOutcome outcome;
                outcome.action = WriteAction::Created;
                if (item.action == "skipped") outcome.action = WriteAction::Skipped;
                else if (item.action == "merged") outcome.action = WriteAction::Merged;
                else if (item.action == "overwritten") outcome.action = WriteAction::Overwritten;
                outcome.base = plan.base;
                outcome.path = plan.meta_path;
                indexed.push_back(outcome);
            } else {
                item.action = "failed";
            }
        } else {
            item.sha = plan.base.sha;
            item.song_key = plan.meta_path.song_key;
            item.quality_key = plan.meta_path.quality_key;
            item.action = plan.fatal ? "failed" : plan.dry_action;
        }

        for (auto& c : plan.conflicts) report.conflicts.push_back(std::move(c));

        if (plan.fatal) {
            ++report.failed;
            item.action = "failed";
            item.code = plan.code;
            item.message = plan.message;
        } else if (item.action == "skipped") {
            ++report.skipped;
            item.code = ErrorCode::Ok;
            item.message = plan.message;
        } else {
            ++report.imported;
            item.code = ErrorCode::Ok;
            item.message = plan.message;
        }
        report.items.push_back(std::move(item));
    }

    // 10) 索引：成功条目 upsert，全部结束后 save（dry_run 不写盘）
    if (!opt.dry_run) {
        for (const auto& outcome : indexed) index_.upsert(outcome.base, outcome.path);
        const Status saved = index_.save();
        if (!saved.ok()) {
            reports_.push_back(std::move(report));
            return Result<json::Value>::failure(saved.code, saved.response_message());
        }
    }

    reports_.push_back(std::move(report));
    return Result<json::Value>::success(reports_.back().to_json());
}

Result<json::Value> ImportManager::status(std::string_view import_id) {
    for (const auto& r : reports_) {
        if (r.import_id != import_id) continue;
        json::Value out = json::Value::object();
        out.set("import_id", r.import_id);
        out.set("scanned", static_cast<long long>(r.scanned));
        out.set("imported", static_cast<long long>(r.imported));
        out.set("skipped", static_cast<long long>(r.skipped));
        out.set("failed", static_cast<long long>(r.failed));
        out.set("total", static_cast<long long>(r.scanned));
        out.set("done", static_cast<long long>(r.scanned));
        out.set("conflicts", static_cast<long long>(r.conflicts.size()));
        out.set("dry_run", r.dry_run);
        return Result<json::Value>::success(out);
    }
    return Result<json::Value>::failure(ErrorCode::TaskNotFound,
                                        "导入任务不存在: " + std::string(import_id));
}

Result<json::Value> ImportManager::report(std::string_view import_id) {
    for (const auto& r : reports_) {
        if (r.import_id != import_id) continue;
        json::Value out = json::Value::object();
        out.set("import_id", r.import_id);
        out.set("scanned", static_cast<long long>(r.scanned));
        out.set("imported", static_cast<long long>(r.imported));
        out.set("skipped", static_cast<long long>(r.skipped));
        out.set("failed", static_cast<long long>(r.failed));
        out.set("dry_run", r.dry_run);
        json::Value conflicts_value = json::Value::array();
        for (const auto& c : r.conflicts) conflicts_value.push_back(c.to_json());
        out.set("conflicts", conflicts_value);
        json::Value items_value = json::Value::array();
        for (const auto& i : r.items) items_value.push_back(i.to_json());
        out.set("items", items_value);
        return Result<json::Value>::success(out);
    }
    return Result<json::Value>::failure(ErrorCode::TaskNotFound,
                                        "导入任务不存在: " + std::string(import_id));
}

}  // namespace radmusic
