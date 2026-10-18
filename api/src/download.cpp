// Rad music API - 下载实现（Readme.md 1.8 / 2.10）
//
// 不抛异常：文件系统操作用 error_code 重载，失败以 Status / ErrorCode 表达。
// 单线程模型：step() 处理一个排队任务；fetch 实现可在执行中调用 pause()/cancel()
// 中断本次下载（此时保留外部状态，见 run_task 中的 interrupted 处理）。
#include "radmusic/download.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <system_error>

#include "radmusic/crypto.hpp"
#include "radmusic/groupkey.hpp"
#include "radmusic/httpclient.hpp"
#include "radmusic/json.hpp"
#include "radmusic/quality.hpp"

namespace radmusic {
namespace {

const char* const kDefaultTemplate = "{artist} - {title} [{quality}]";
const char* const kDownloadsFile = "downloads.json";

std::string path_to_utf8(const std::filesystem::path& p) {
    const auto u8 = p.u8string();
    return u8.empty() ? std::string() : std::string(reinterpret_cast<const char*>(u8.c_str()));
}

std::string lower_ascii(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(static_cast<char>((c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c));
    return out;
}

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

void replace_all(std::string& text, std::string_view from, const std::string& to) {
    if (from.empty()) return;
    for (size_t pos = text.find(from); pos != std::string::npos; pos = text.find(from, pos + to.size())) {
        text.replace(pos, from.size(), to);
    }
}

std::string quality_placeholder(const Quality& quality) {
    if (!quality.label.empty()) return quality.label;
    if (!quality.quality_id.empty()) return quality.quality_id;
    if (quality.is_known()) return quality_level_id(quality.level);
    return "unknown";
}

// 落盘扩展名：优先 codec（1.1.1 归一化字段），其次 URL 后缀
std::string extension_for(const Quality& quality, const std::string& url) {
    const std::string codec = lower_ascii(quality.codec);
    if (codec == "flac") return "flac";
    if (codec == "mp3") return "mp3";
    if (codec == "aac") return "aac";
    if (codec == "alac") return "m4a";
    if (codec == "opus") return "opus";
    if (codec == "vorbis" || codec == "ogg") return "ogg";
    if (codec == "pcm" || codec == "wav") return "wav";
    if (codec == "ape") return "ape";
    if (!codec.empty()) {
        std::string safe;
        for (char c : codec) {
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) safe.push_back(c);
        }
        if (!safe.empty() && safe.size() <= 8) return safe;
    }
    std::string path = url;
    const size_t cut = path.find_first_of("?#");
    if (cut != std::string::npos) path = path.substr(0, cut);
    std::string ext = lower_ascii(std::filesystem::path(path).extension().string());
    if (!ext.empty() && ext[0] == '.') ext.erase(0, 1);
    std::string safe;
    for (char c : ext) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) safe.push_back(c);
    }
    if (!safe.empty() && safe.size() <= 8) return safe;
    return "bin";
}

// 命名模板（1.8 下载任务字段 filename）：{artist} / {title} / {quality} / {album} / {ext}
std::string format_filename(std::string tmpl, const BaseInfo& base, const Quality& quality,
                            const std::string& ext) {
    if (tmpl.empty()) tmpl = kDefaultTemplate;
    replace_all(tmpl, "{artist}", base.author);
    replace_all(tmpl, "{title}", base.name);
    replace_all(tmpl, "{quality}", quality_placeholder(quality));
    replace_all(tmpl, "{album}", base.album);
    replace_all(tmpl, "{ext}", ext);
    std::string stem = fs_sanitize(tmpl, 120);
    if (ext.empty()) return stem;
    return stem + "." + ext;
}

// 源上报的音质优先（可能是降级结果）；缺失字段用请求音质补齐，保证 quality_key 可用
Quality merge_quality(const std::vector<QualityLevelSpec>& specs, const Quality& requested,
                      const Quality& reported) {
    const bool reported_present =
        reported.is_known() || !reported.codec.empty() || !reported.quality_id.empty();
    if (!reported_present) return requested;

    Quality out = reported;
    if (out.is_known() && requested.is_known() && out.level != requested.level) {
        // 源降级：按实际 level 找到插件声明的对应 quality_id（1.1「quality_key」）
        for (const auto& spec : specs) {
            if (spec.spec.level == out.level && !spec.id.empty()) {
                out.quality_id = spec.id;
                if (out.label.empty()) out.label = spec.label;
                break;
            }
        }
        if (!requested.quality_id.empty() && out.quality_id.empty()) out.quality_id = requested.quality_id;
        return out;
    }
    if (out.quality_id.empty()) out.quality_id = requested.quality_id;
    if (out.label.empty()) out.label = requested.label;
    if (out.codec.empty()) out.codec = requested.codec;
    if (!out.bitrate.has_value()) out.bitrate = requested.bitrate;
    if (!out.sample_rate.has_value()) out.sample_rate = requested.sample_rate;
    if (!out.bit_depth.has_value()) out.bit_depth = requested.bit_depth;
    if (!out.channels.has_value()) out.channels = requested.channels;
    if (!out.is_known()) out.level = requested.level;
    if (!out.lossless) out.lossless = requested.lossless;
    return out;
}

// 磁盘不足 → 1019；其余按下载失败 1018（1.8 流程 6）
ErrorCode classify_fetch_error(const std::string& error) {
    const std::string text = lower_ascii(error);
    if (contains(text, "insufficient space") || contains(text, "no space") || contains(text, "space") ||
        contains(text, "enospc") || contains(text, "disk full") || contains(text, "空间") ||
        contains(text, "1019")) {
        return ErrorCode::InsufficientSpace;
    }
    return ErrorCode::DownloadFailed;
}

// 仅允许删除 tracks/ 下的文件（1.8 / 2.10：需二次确认）
bool under_tracks(const std::filesystem::path& tracks_root, const std::filesystem::path& candidate) {
    std::error_code ec;
    std::filesystem::path root = std::filesystem::absolute(tracks_root, ec);
    if (ec) return false;
    std::filesystem::path target = std::filesystem::absolute(candidate, ec);
    if (ec) return false;
    const std::string r = path_to_utf8(root.lexically_normal());
    const std::string t = path_to_utf8(target.lexically_normal());
    if (t.size() <= r.size()) return false;
    if (t.compare(0, r.size(), r) != 0) return false;
    return t[r.size()] == '/' || t[r.size()] == '\\';
}

// 从 location（tracks/<sha>/<filename>）取文件名
std::string filename_of_location(const std::string& location) {
    const std::size_t slash = location.find_last_of("/\\");
    return slash == std::string::npos ? location : location.substr(slash + 1);
}

DownloadTask task_from_json(const json::Value& v) {
    DownloadTask task;
    if (!v.is_object()) return task;
    task.id = v.get_string("id");
    task.source = v.get_string("source");
    task.item_id = v.get_string("item_id");
    task.quality_request = v.get_string("quality", "auto");
    task.filename = v.get_string("filename");
    task.filename_template = v.get_string("filename_template");
    if (task.filename_template.empty()) {
        // 旧版 downloads.json 把模板写进了 filename：带占位符的就是模板，需要迁移
        if (task.filename.find('{') != std::string::npos) {
            task.filename_template = task.filename;
            task.filename.clear();
        } else {
            task.filename_template = kDefaultTemplate;
        }
    }
    if (task.filename.empty()) {
        task.filename = filename_of_location(v.get_string("path"));
    }
    task.state = download_state_from_id(v.get_string("state", "queued"));
    task.received = static_cast<long long>(v.get_int("received", 0));
    task.total = static_cast<long long>(v.get_int("total", 0));
    task.retry = static_cast<int>(v.get_int("retry", 0));
    task.last_error = static_cast<ErrorCode>(static_cast<int>(v.get_int("last_error", 0)));
    task.message = v.get_string("message");
    if (const json::Value* q = v.find("quality_resolved"); q && q->is_object()) {
        task.quality_resolved = Quality::from_json(*q);
    }
    task.sha = v.get_string("sha");
    task.path = v.get_string("path");
    return task;
}

}  // namespace

// ---------------------------------------------------------------- 状态

const char* download_state_id(DownloadState state) {
    switch (state) {
        case DownloadState::Queued: return "queued";
        case DownloadState::Running: return "running";
        case DownloadState::Paused: return "paused";
        case DownloadState::Done: return "done";
        case DownloadState::Failed: return "failed";
        default: return "canceled";
    }
}

DownloadState download_state_from_id(std::string_view id) {
    const std::string value = lower_ascii(id);
    if (value == "running") return DownloadState::Running;
    if (value == "paused") return DownloadState::Paused;
    if (value == "done") return DownloadState::Done;
    if (value == "failed") return DownloadState::Failed;
    if (value == "canceled" || value == "cancelled") return DownloadState::Canceled;
    return DownloadState::Queued;
}

json::Value DownloadTask::to_json() const {
    json::Value v = json::Value::object();
    v.set("id", id);
    v.set("source", source);
    v.set("item_id", item_id);
    v.set("quality", quality_request);
    // filename 是落盘后的真实文件名，模板单独给出，避免前端把占位符当文件名展示
    v.set("filename", filename);
    v.set("filename_template", filename_template);
    v.set("state", std::string(download_state_id(state)));
    v.set("received", static_cast<long long>(received));
    v.set("total", static_cast<long long>(total));
    v.set("retry", static_cast<long long>(retry));
    v.set("last_error", static_cast<long long>(static_cast<int>(last_error)));
    v.set("error", std::string(error_id(last_error)));
    if (!message.empty()) v.set("message", message);
    if (quality_resolved.is_known() || !quality_resolved.quality_id.empty() ||
        !quality_resolved.codec.empty()) {
        v.set("quality_resolved", quality_resolved.to_json());
    }
    if (!sha.empty()) v.set("sha", sha);
    if (!path.empty()) v.set("path", path);
    return v;
}

// ---------------------------------------------------------------- DownloadManager

DownloadManager::DownloadManager(const LibraryPaths& paths, MetaCache& meta, LibraryIndex& index,
                                 PluginRegistry& plugins)
    : paths_(paths), meta_(meta), index_(index), plugins_(plugins) {}

DownloadManager::~DownloadManager() = default;

DownloadTask* DownloadManager::find_task(std::string_view id) {
    for (auto& task : tasks_) {
        if (task.id == id) return &task;
    }
    return nullptr;
}

const DownloadTask* DownloadManager::find_task(std::string_view id) const {
    for (const auto& task : tasks_) {
        if (task.id == id) return &task;
    }
    return nullptr;
}

std::string DownloadManager::make_task_id() {
    for (;;) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "dl_%04lld", next_id_);
        ++next_id_;
        const std::string id = buffer;
        if (find_task(id) == nullptr) return id;
    }
}

DownloadState DownloadManager::external_state(std::string_view id) const {
    const DownloadTask* task = find_task(id);
    return task ? task->state : DownloadState::Canceled;
}

Result<DownloadTask> DownloadManager::create(const std::string& source, const std::string& item_id,
                                             const std::string& quality,
                                             const std::string& filename_template) {
    // 1.8 / 2.10 规则 1：折叠条目必须显式指定音源
    if (source.empty() || item_id.empty()) {
        return Result<DownloadTask>::failure(ErrorCode::InvalidMetadata,
                                             "必须显式指定 source 与 item_id（1.8 规则 1）");
    }

    const std::string request = quality.empty() ? std::string("auto") : quality;
    const std::string request_lower = lower_ascii(request);

    // 已存在的「已完成」记录只保留一条：避免重复点击「下载」在列表里堆出多条完成项。
    // 命中时复用既有记录（保持其 id / sha / 音质），但对外仍以「已存在」语义返回；否则入列以便下载页可见。
    auto track_existing = [&](DownloadTask task) -> Result<DownloadTask> {
      for (const auto& t : tasks_) {
        if (t.source == task.source && t.item_id == task.item_id &&
            t.quality_request == task.quality_request && t.state == DownloadState::Done) {
          DownloadTask out = t;
          out.message = "already exists";
          return Result<DownloadTask>::success(out);
        }
      }
      tasks_.push_back(task);
      return Result<DownloadTask>::success(task);
    };

    // location 指向真实本地文件时返回其字节数；在线曲目只在缓存里留了元数据时，location 是
    // "http://plugin.invalid/<platform>/<id>" 之类的占位（并没有落盘文件），此时返回 -1。
    auto real_local_size = [this](const std::string& location) -> long long {
      if (location.empty()) return -1;
      if (location.rfind("http://", 0) == 0 || location.rfind("https://", 0) == 0) return -1;
      std::error_code ec;
      const std::filesystem::path p = paths_.root / location;
      if (!std::filesystem::is_regular_file(p, ec) || ec) return -1;
      const auto sz = std::filesystem::file_size(p, ec);
      return ec ? -1 : static_cast<long long>(sz);
    };

    // 本地曲目（source = local，item_id = sha）：文件已在曲库中，直接返回已完成任务，
    // 不再走「解析地址 → 复制文件」的流程（否则会因找不到 local 插件而失败 1001）。
    if (lower_ascii(source) == "local") {
        const auto found = meta_.find_by_sha(item_id);
        if (!found.has_value()) {
            return Result<DownloadTask>::failure(ErrorCode::FileNotFound, "本地曲目不存在: " + item_id);
        }
        DownloadTask existing_task;
        existing_task.id = make_task_id();
        existing_task.source = source;
        existing_task.item_id = item_id;
        existing_task.quality_request = request;
        existing_task.filename_template = filename_template.empty() ? kDefaultTemplate : filename_template;
        existing_task.state = DownloadState::Done;
        existing_task.message = "already exists";
        if (const auto existing = meta_.read(*found); existing.ok()) {
            existing_task.sha = existing.value().sha.empty() ? item_id : existing.value().sha;
            existing_task.quality_resolved = existing.value().quality;
            if (!existing.value().location.empty()) {
                existing_task.path = path_to_utf8(paths_.root / existing.value().location);
                existing_task.filename = filename_of_location(existing.value().location);
                if (const long long sz = real_local_size(existing.value().location); sz >= 0) {
                    existing_task.total = sz;
                    existing_task.received = sz;
                }
            }
        } else {
            existing_task.sha = item_id;
        }
        return track_existing(existing_task);
    }
    const std::vector<QualityLevelSpec> specs = plugins_.quality_levels_of(source);

    // 允许 "<platform>:<quality_id>" 写法（2.10 请求示例）
    std::string bare = request_lower;
    const std::string prefix = lower_ascii(source) + ":";
    if (bare.rfind(prefix, 0) == 0) bare = bare.substr(prefix.size());

    bool declared = false;
    for (const auto& spec : specs) {
        if (lower_ascii(spec.id) == bare) {
            declared = true;
            break;
        }
    }
    const bool is_auto = (bare == "auto");
    const bool is_level = quality_level_from_id(bare) != QualityLevel::Unknown;

    // 2.10 规则 3：音质标识无法识别 → 1027
    if (!is_auto && !is_level && !declared) {
        return Result<DownloadTask>::failure(ErrorCode::QualityUndefined,
                                             "该源未声明音质: " + request);
    }

    Quality resolved;
    if (auto picked = pick_quality(specs, {}, is_auto ? "auto" : bare); picked) {
        resolved = *picked;
    } else if (!is_auto && !specs.empty()) {
        // 2.10 规则 3：该源不支持目标音质 → 1021
        return Result<DownloadTask>::failure(ErrorCode::QualityUnsupported,
                                             "该源不支持音质: " + request);
    }

    // 元数据（同时用于同曲同质去重）：解析失败不阻断创建，step() 时会重试
    const std::string probe_quality =
        resolved.quality_id.empty() ? (is_auto ? std::string("auto") : bare) : resolved.quality_id;
    Result<BaseInfo> meta_result = resolve_meta
                                       ? resolve_meta(source, item_id, probe_quality)
                                       : default_resolve_meta(source, item_id, probe_quality);
    if (meta_result.ok()) {
        BaseInfo base = meta_result.value();
        if (base.platform.empty()) base.platform = source;
        if (base.item_id.empty()) base.item_id = item_id;
        const Quality quality_final = merge_quality(specs, resolved, base.quality);
        if (!base.name.empty()) {
            // 1.8 规则 2：同曲同质已存在 → skip，返回已有记录（不重复下载）
            const std::string group = radmusic::group_key(base.name, base.author, base.duration_seconds);
            const std::string quality_k = radmusic::quality_key(source, quality_final);
            if (const auto existing_path = meta_.find_by_group_quality(group, quality_k);
                existing_path.has_value()) {
                const auto existing = meta_.read(*existing_path);
                // 只有曲库里确有这个文件的真实副本时才算「已存在」；带真实文件名与大小
                const long long existing_size =
                    existing.ok() ? real_local_size(existing.value().location) : -1;
                if (existing_size >= 0) {
                    DownloadTask skipped;
                    skipped.id = make_task_id();
                    skipped.source = source;
                    skipped.item_id = item_id;
                    skipped.quality_request = request;
                    skipped.filename_template =
                        filename_template.empty() ? kDefaultTemplate : filename_template;
                    skipped.state = DownloadState::Done;
                    skipped.message = "already exists";
                    skipped.quality_resolved = quality_final;
                    skipped.sha = existing.value().sha;
                    skipped.quality_resolved = existing.value().quality;
                    skipped.received = existing_size;
                    skipped.total = existing_size;
                    if (!existing.value().location.empty()) {
                        skipped.path = path_to_utf8(paths_.root / existing.value().location);
                        skipped.filename = filename_of_location(existing.value().location);
                    }
                    return track_existing(skipped);
                }
                // 只缓存了元数据、并没有真实文件（在线曲目搜索/播放留下的 plugin.invalid 占位记录）：
                // 清掉这条陈旧记录后走下面的真实下载，避免弹出一条 name=id / 0B 的假完成任务。
                if (existing.ok()) {
                    meta_.erase(*existing_path);
                    index_.remove_by_sha(existing.value().sha);
                }
            }
        }
    }

    DownloadTask task;
    task.id = make_task_id();
    task.source = source;
    task.item_id = item_id;
    task.quality_request = request;
    task.filename_template = filename_template.empty() ? kDefaultTemplate : filename_template;
    task.state = DownloadState::Queued;
    task.quality_resolved = resolved;
    tasks_.push_back(task);
    return Result<DownloadTask>::success(task);
}

std::vector<DownloadTask> DownloadManager::list() const { return tasks_; }

Result<DownloadTask> DownloadManager::get(std::string_view id) const {
    const DownloadTask* task = find_task(id);
    if (!task) return Result<DownloadTask>::failure(ErrorCode::TaskNotFound, "任务不存在: " + std::string(id));
    return Result<DownloadTask>::success(*task);
}

Status DownloadManager::pause(std::string_view id) {
    DownloadTask* task = find_task(id);
    if (!task) return Status(ErrorCode::TaskNotFound, "任务不存在: " + std::string(id));
    switch (task->state) {
        case DownloadState::Running:
        case DownloadState::Queued:
            task->state = DownloadState::Paused;
            task->message = "已暂停";
            if (on_progress) on_progress(*task);
            return Status::success();
        case DownloadState::Paused:
            return Status::success();   // 幂等
        default:
            return Status(ErrorCode::DownloadFailed, "任务状态不允许暂停");
    }
}

Status DownloadManager::resume(std::string_view id) {
    DownloadTask* task = find_task(id);
    if (!task) return Status(ErrorCode::TaskNotFound, "任务不存在: " + std::string(id));
    switch (task->state) {
        case DownloadState::Paused:
        case DownloadState::Failed:
            task->state = DownloadState::Queued;
            task->message.clear();
            task->last_error = ErrorCode::Ok;
            return Status::success();
        case DownloadState::Queued:
            return Status::success();   // 幂等
        default:
            return Status(ErrorCode::DownloadFailed, "任务状态不允许继续");
    }
}

Status DownloadManager::cancel(std::string_view id) {
    DownloadTask* task = find_task(id);
    if (!task) return Status(ErrorCode::TaskNotFound, "任务不存在: " + std::string(id));
    if (task->state == DownloadState::Done) {
        return Status(ErrorCode::DownloadFailed, "已完成的任务不能取消");
    }
    std::error_code ec;
    std::filesystem::remove(paths_.tmp() / (task->id + ".part"), ec);
    task->state = DownloadState::Canceled;
    task->message = "已取消";
    task->last_error = ErrorCode::Ok;
    return Status::success();
}

Result<bool> DownloadManager::remove(std::string_view id, bool delete_file) {
    auto it = std::find_if(tasks_.begin(), tasks_.end(),
                           [&id](const DownloadTask& task) { return task.id == id; });
    if (it == tasks_.end()) {
        return Result<bool>::failure(ErrorCode::TaskNotFound, "任务不存在: " + std::string(id));
    }

    bool deleted = false;
    if (delete_file && !it->path.empty()) {
        // 仅当路径在 tracks/ 下才允许删除（不得触碰曲库以外内容）
        if (!under_tracks(paths_.tracks(), it->path)) {
            return Result<bool>::failure(ErrorCode::InvalidMetadata,
                                         "拒绝删除曲库之外的文件: " + it->path);
        }
        std::error_code ec;
        deleted = std::filesystem::remove(it->path, ec);
        if (ec) {
            return Result<bool>::failure(ErrorCode::RequestFailed,
                                         "删除文件失败: " + it->path + " (" + ec.message() + ")");
        }
    }

    std::error_code ec;
    std::filesystem::remove(paths_.tmp() / (it->id + ".part"), ec);
    tasks_.erase(it);
    return Result<bool>::success(deleted);
}

bool DownloadManager::step() {
    std::string id;
    for (const auto& task : tasks_) {
        if (task.state == DownloadState::Queued) {
            id = task.id;
            break;
        }
    }
    if (id.empty()) return false;

    if (DownloadTask* task = find_task(id); task != nullptr) {
        task->state = DownloadState::Running;
        task->message.clear();
        task->last_error = ErrorCode::Ok;
    }
    run_task(id);
    return true;
}

void DownloadManager::run_task(const std::string& id) {
    DownloadTask work;
    {
        const DownloadTask* current = find_task(id);
        if (current == nullptr) return;
        work = *current;
    }
    work.state = DownloadState::Running;

    // 失败收尾：若 fetch 期间被 pause/cancel 中断，则保留外部状态（1.8 流程 6：保留 .part 供重试）
    auto fail = [&](ErrorCode code, const std::string& message) {
        DownloadTask* current = find_task(id);
        if (current == nullptr) return;
        const DownloadState external = current->state;
        work.state = DownloadState::Failed;
        work.last_error = code;
        work.message = message;
        ++work.retry;
        if (external == DownloadState::Paused || external == DownloadState::Canceled) {
            work.state = external;
            work.last_error = ErrorCode::Ok;
            work.message = (external == DownloadState::Paused) ? std::string("已暂停") : std::string("已取消");
            --work.retry;
        }
        *current = work;
        if (on_progress) on_progress(work);
        if (work.state == DownloadState::Failed && on_failed) on_failed(work);
    };

    // ---- 1) 元数据（1.8 流程 1 / 5）----
    const std::string first_quality = !work.quality_resolved.quality_id.empty()
                                          ? work.quality_resolved.quality_id
                                          : (work.quality_request.empty() ? std::string("auto")
                                                                          : work.quality_request);
    Result<BaseInfo> meta_result = resolve_meta ? resolve_meta(work.source, work.item_id, first_quality)
                                                : default_resolve_meta(work.source, work.item_id, first_quality);
    if (!meta_result.ok()) {
        fail(meta_result.status().code, meta_result.status().response_message());
        return;
    }

    const std::vector<QualityLevelSpec> specs = plugins_.quality_levels_of(work.source);
    Quality requested;
    if (auto picked = pick_quality(specs, {}, work.quality_request.empty() ? "auto" : work.quality_request);
        picked) {
        requested = *picked;
    }
    if (!requested.is_known() && work.quality_resolved.is_known()) requested = work.quality_resolved;

    BaseInfo base = meta_result.value();
    if (base.platform.empty()) base.platform = work.source;
    if (base.item_id.empty()) base.item_id = work.item_id;
    if (base.version.empty()) base.version = "1.0.0";
    if (base.name.empty()) base.name = work.item_id.empty() ? work.source : work.item_id;   // 缺失字段兜底
    if (base.author.empty()) base.author = work.source;
    base.quality = merge_quality(specs, requested, base.quality);
    work.quality_resolved = base.quality;

    const std::string quality_arg = !base.quality.quality_id.empty()
                                        ? base.quality.quality_id
                                        : (work.quality_request.empty() ? std::string("auto")
                                                                        : work.quality_request);

    // ---- 2) 下载地址（1.8 流程 1）----
    Result<std::string> url_result = resolve_url ? resolve_url(work.source, work.item_id, quality_arg)
                                                 : default_resolve_url(work.source, work.item_id, quality_arg);
    if (!url_result.ok()) {
        fail(url_result.status().code, url_result.status().response_message());
        return;
    }
    const std::string url = url_result.value();
    if (url.empty()) {
        fail(ErrorCode::DownloadFailed, "下载地址为空");
        return;
    }

    // ---- 3) 分片下载到 cache/tmp/<task-id>.part（支持断点续传）----
    std::error_code ec;
    std::filesystem::create_directories(paths_.tmp(), ec);
    const std::filesystem::path part = paths_.tmp() / (work.id + ".part");
    long long received = 0;
    if (std::filesystem::exists(part, ec)) {
        const auto size = std::filesystem::file_size(part, ec);
        if (!ec) received = static_cast<long long>(size);   // 续传：把已存在大小传给 fetch
    }
    long long total = 0;
    std::string fetch_error;
    const bool fetched = fetch ? fetch(url, part, &received, &total, &fetch_error)
                               : default_fetch(url, part, &received, &total, &fetch_error);
    work.received = received;
    work.total = total;
    if (on_progress) on_progress(work);
    if (!fetched) {
        fail(classify_fetch_error(fetch_error), fetch_error.empty() ? std::string("下载失败") : fetch_error);
        return;
    }
    if (const DownloadState external = external_state(id);
        external == DownloadState::Paused || external == DownloadState::Canceled) {
        if (DownloadTask* current = find_task(id); current != nullptr) {
            work.state = external;
            work.message = (external == DownloadState::Paused) ? std::string("已暂停") : std::string("已取消");
            work.last_error = ErrorCode::Ok;
            *current = work;
        }
        return;
    }

    // ---- 4) 校验 SHA-256（1.8 流程 3）----
    bool sha_ok = false;
    const std::string actual = crypto::sha256_file_hex(path_to_utf8(part), &sha_ok);
    if (!sha_ok) {
        fail(ErrorCode::DownloadFailed, "无法计算下载文件校验值: " + path_to_utf8(part));
        return;
    }
    // 在线插件常把「sha256(platform:item_id)」当作曲目逻辑 id 填进 base.sha（1.1 要求它是
    // 文件内容 sha，下载场景下该值只能由落盘后算出）。这类逻辑 id 不能用于内容校验，
    // 否则每次下载都会误判 1022。
    const std::string logical_sha = crypto::sha256_hex(work.source + ":" + work.item_id);
    const bool sha_is_logic_id = !base.sha.empty() && lower_ascii(base.sha) == logical_sha;
    if (!base.sha.empty() && !sha_is_logic_id && lower_ascii(base.sha) != lower_ascii(actual)) {
        // 1022：保留 .part 供重试/换源（1.8 流程 3/6）
        fail(ErrorCode::ChecksumMismatch,
             "校验失败：期望 sha=" + lower_ascii(base.sha) + "，实际 sha=" + actual);
        return;
    }

    // ---- 5) 原子移动到 tracks/<sha>/<文件名>（不覆盖用户已有文件）----
    const std::string ext = extension_for(base.quality, url);
    const std::string filename = format_filename(work.filename_template, base, base.quality, ext);
    const std::filesystem::path target = paths_.track_dir(actual) / filename;

    const std::string location = "tracks/" + actual + "/" + filename;   // 相对曲库根（1.1 示例）
    base.sha = actual;
    base.secret = 0;
    base.location = location;

    const MetaPath meta_path = meta_.path_for(base);
    if (!meta_path.valid()) {
        fail(ErrorCode::InvalidMetadata, "无法推导 base.json 缓存路径");
        return;
    }

    std::filesystem::create_directories(target.parent_path(), ec);
    const bool target_exists = std::filesystem::exists(target, ec);
    if (target_exists) {
        // 同一 sha 目录内容寻址：已有同名文件视为同内容，不覆盖、不重复落盘
        std::filesystem::remove(part, ec);
    } else {
        std::filesystem::rename(part, target, ec);
        if (ec) {
            std::error_code cec;
            std::filesystem::copy_file(part, target, std::filesystem::copy_options::overwrite_existing, cec);
            if (cec) {
                fail(ErrorCode::DownloadFailed,
                     "移动下载文件失败: " + path_to_utf8(target) + " (" + ec.message() + ")");
                return;
            }
            std::filesystem::remove(part, ec);
        }
    }

    // 1.8 流程 5：base.json 写入元数据缓存 cache/meta/<song_key>/<quality_key>/base.json
    // 在线曲目播放时引擎会重建占位 base.json（location=http://plugin.invalid/...、
    // sha=sha256(platform:item_id) 逻辑 id），cache/meta 里只有元数据、没有真实文件。
    // 这类占位记录不能以 1023（base conflict）挡下刚下载完的真实文件：同路径已存在
    // 但并无真实本地文件时，先清掉占位记录（含库索引指针）再写入。
    if (const auto existing = meta_.read(meta_path); existing.ok()) {
        const std::string& loc = existing.value().location;
        const bool remote_loc = loc.rfind("http://", 0) == 0 || loc.rfind("https://", 0) == 0;
        bool real_local = false;
        if (!remote_loc && !loc.empty()) {
            std::error_code fec;
            real_local = std::filesystem::is_regular_file(paths_.root / loc, fec);
        }
        const std::string existing_logical =
            crypto::sha256_hex(existing.value().platform + ":" + existing.value().item_id);
        const bool logic_id_sha = !existing.value().sha.empty() &&
                                  lower_ascii(existing.value().sha) == lower_ascii(existing_logical);
        if (!real_local && (remote_loc || loc.empty() || logic_id_sha)) {
            meta_.erase(meta_path);
            index_.remove_by_sha(existing.value().sha);
        }
    }
    const auto outcome = meta_.put(base, ConflictPolicy::Skip);
    if (!outcome.ok()) {
        // 回滚：把音频退回 .part，保留重试能力
        if (!target_exists) {
            std::error_code rec;
            std::filesystem::rename(target, part, rec);
        }
        fail(outcome.status().code, outcome.status().response_message());
        return;
    }

    // 刷新库索引（持久指针在 index/，见 1.10 规则 5）
    index_.upsert(base, outcome.value().path);
    const Status saved = index_.save();

    work.state = DownloadState::Done;
    work.sha = actual;
    work.path = path_to_utf8(target);
    work.filename = filename;
    work.last_error = ErrorCode::Ok;
    work.message = saved.ok() ? std::string("ok") : ("下载完成，但库索引写入失败: " + saved.message);
    if (DownloadTask* current = find_task(id); current != nullptr) {
        *current = work;
    }
    if (on_done) on_done(work);
}

// ---------------------------------------------------------------- 默认实现（插件 / 本地文件）

Result<BaseInfo> DownloadManager::default_resolve_meta(const std::string& source,
                                                       const std::string& item_id,
                                                       const std::string& quality) const {
    auto plugin = plugins_.source_for_platform(source);
    if (!plugin) {
        return Result<BaseInfo>::failure(ErrorCode::PlatformError, "无可用插件适配 platform: " + source);
    }

    Result<BaseInfo> base = plugin->resolve(item_id);
    if (!base.ok()) {
        json::Value params = json::Value::object();
        params.set("id", item_id);
        if (!quality.empty()) params.set("quality", quality);
        auto response = plugin->request("get_song", params);
        if (!response.ok()) return Result<BaseInfo>(response.status());
        const json::Value* node = &response.value();
        if (node->is_object()) {
            if (const json::Value* data = node->find("data"); data && data->is_object()) node = data;
        }
        if (!node->is_object()) {
            return Result<BaseInfo>::failure(ErrorCode::RequestFailed, "get_song 未返回元数据");
        }
        base = Result<BaseInfo>::success(BaseInfo::from_json(*node));
    }

    BaseInfo info = base.value();
    if (info.platform.empty()) info.platform = source;
    if (info.item_id.empty()) info.item_id = item_id;
    if (!info.quality.is_known()) {
        const auto specs = plugins_.quality_levels_of(source);
        if (auto picked = pick_quality(specs, {}, quality.empty() ? "auto" : quality); picked) {
            info.quality = *picked;
        }
    }
    return Result<BaseInfo>::success(info);
}

Result<std::string> DownloadManager::default_resolve_url(const std::string& source,
                                                         const std::string& item_id,
                                                         const std::string& quality) const {
    auto plugin = plugins_.source_for_platform(source);
    if (!plugin) {
        return Result<std::string>::failure(ErrorCode::PlatformError, "无可用插件适配 platform: " + source);
    }

    auto extract = [](const json::Value& value) -> std::string {
        if (value.is_string()) return value.as_string();
        const json::Value* node = &value;
        if (node->is_object()) {
            if (const json::Value* data = node->find("data"); data && data->is_object()) node = data;
        }
        if (!node->is_object()) return std::string();
        for (const char* key : {"url", "location", "link", "src"}) {
            const std::string found = node->get_string(key);
            if (!found.empty()) return found;
        }
        return std::string();
    };

    json::Value params = json::Value::object();
    params.set("id", item_id);
    if (!quality.empty()) params.set("quality", quality);

    const auto response = plugin->request("get_url", params);
    if (response.ok()) {
        const std::string url = extract(response.value());
        if (!url.empty()) return Result<std::string>::success(url);
    }

    // 退回 5.2 的 open(item_id)
    const auto opened = plugin->open(item_id);
    if (opened.ok() && !opened.value().empty()) return Result<std::string>::success(opened.value());
    if (!response.ok()) return Result<std::string>(response.status());
    return Result<std::string>::failure(ErrorCode::RequestFailed, "get_url 未返回可用地址");
}

bool DownloadManager::default_fetch(const std::string& url, const std::filesystem::path& out_path,
                                    long long* received, long long* total, std::string* error) const {
    auto set_error = [error](const std::string& message) {
        if (error) *error = message;
        return false;
    };

    std::string source = url;
    if (source.rfind("file://", 0) == 0) {
        source = source.substr(7);
        if (source.size() > 2 && source[0] == '/' && source[2] == ':') source.erase(0, 1);   // /C:/...
    } else if (is_remote_url(source)) {
        return http_fetch_file(source, out_path, received, total, error);
    } else if (source.rfind("http://", 0) == 0 || source.rfind("https://", 0) == 0) {
        return set_error("未提供网络下载器（宿主需注入 fetch）: " + url);
    }

    std::error_code ec;
    if (!std::filesystem::is_regular_file(source, ec) || ec) {
        return set_error("无法访问下载源: " + source);
    }
    const auto size = std::filesystem::file_size(source, ec);
    if (ec) return set_error("无法读取下载源: " + source);

    long long start = received ? *received : 0;
    if (start < 0 || static_cast<std::uintmax_t>(start) > size) start = 0;

    std::ifstream in(source, std::ios::binary);
    if (!in) return set_error("无法打开下载源: " + source);
    if (start > 0) in.seekg(start);

    std::ofstream out(out_path, std::ios::binary | (start > 0 ? std::ios::app : std::ios::trunc));
    if (!out) return set_error("无法写入临时文件: " + path_to_utf8(out_path));

    std::vector<char> buffer(64 * 1024);
    long long done = start;
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = in.gcount();
        if (got <= 0) break;
        out.write(buffer.data(), got);
        if (!out) return set_error("写入临时文件失败: " + path_to_utf8(out_path));
        done += got;
    }
    if (in.bad()) return set_error("读取下载源失败: " + source);
    out.flush();
    if (!out.good()) return set_error("刷新临时文件失败: " + path_to_utf8(out_path));

    if (received) *received = done;
    if (total) *total = static_cast<long long>(size);
    return true;
}

// ---------------------------------------------------------------- 持久化

Status DownloadManager::save() const {
    json::Value root = json::Value::object();
    root.set("version", static_cast<long long>(1));
    root.set("next_id", static_cast<long long>(next_id_));
    json::Value array = json::Value::array();
    for (const auto& task : tasks_) array.push_back(task.to_json());
    root.set("tasks", array);
    return write_text_file_atomic(paths_.cache(), paths_.cache() / kDownloadsFile, root.dump(2));
}

Status DownloadManager::load() {
    tasks_.clear();
    next_id_ = 1;

    const auto file = paths_.cache() / kDownloadsFile;
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) return Status::success();

    const auto text = read_text_file(file);
    if (!text.ok()) return text.status();
    json::ParseError err;
    const json::Value root = json::Value::parse(text.value(), &err);
    if (!root.is_object()) {
        return Status(ErrorCode::InvalidMetadata,
                      "downloads.json 解析失败: " + err.message + " @" + std::to_string(err.offset));
    }

    next_id_ = root.get_int("next_id", 1);
    const json::Value& array = root.get("tasks");
    for (const auto& item : array.as_array()) {
        DownloadTask task = task_from_json(item);
        if (task.id.empty()) continue;
        // 重启后未完成的任务按 Paused 处理（可 resume 续传）
        if (task.state == DownloadState::Running) {
            task.state = DownloadState::Paused;
            task.message = "已暂停（进程重启）";
        }
        tasks_.push_back(task);
        if (const size_t underscore = task.id.rfind('_'); underscore != std::string::npos) {
            const long long suffix = std::strtoll(task.id.c_str() + underscore + 1, nullptr, 10);
            if (suffix >= next_id_) next_id_ = suffix + 1;
        }
    }
    return Status::success();
}

size_t DownloadManager::running_count() const {
    size_t count = 0;
    for (const auto& task : tasks_) {
        if (task.state == DownloadState::Running) ++count;
    }
    return count;
}

}  // namespace radmusic
