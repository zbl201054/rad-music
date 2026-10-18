// Rad music API - 本地曲目联网识别实现（1.10 扩展，见 identify.hpp）
#include "radmusic/identify.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <system_error>
#include <vector>

#include "radmusic/crypto.hpp"
#include "radmusic/groupkey.hpp"
#include "radmusic/httpclient.hpp"
#include "radmusic/log.hpp"
#include "radmusic/quality.hpp"

namespace radmusic {
namespace {

// ---------------------------------------------------------------- 小工具

std::string lower_ascii(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

std::string trim_ascii(std::string_view s) {
    size_t begin = 0;
    size_t end = s.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(s[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return std::string(s.substr(begin, end - begin));
}

std::string generic_utf8(const std::filesystem::path& p) { return p.generic_u8string(); }

std::string path_utf8(const std::filesystem::path& p) {
    const std::string bytes = p.u8string();   // C++17: u8string() 为 std::string（UTF-8）
    return bytes;
}

// 多位歌手拆分："A / B"、"A/B"、"A、B"、"A, B"、"A & B"、"A feat. B"、"A ft. B"
std::vector<std::string> split_artists(std::string_view author) {
    std::vector<std::string> out;
    std::string current;
    const auto flush = [&out, &current]() {
        const std::string piece = trim_ascii(current);
        if (!piece.empty()) out.push_back(piece);
        current.clear();
    };
    size_t i = 0;
    while (i < author.size()) {
        // 中文顿号 "、"（UTF-8: E3 80 81）
        if (i + 2 < author.size() && static_cast<unsigned char>(author[i]) == 0xE3 &&
            static_cast<unsigned char>(author[i + 1]) == 0x80 &&
            static_cast<unsigned char>(author[i + 2]) == 0x81) {
            flush();
            i += 3;
            continue;
        }
        const char c = author[i];
        if (c == '/' || c == '\\' || c == ',' || c == '&' || c == ';') {
            flush();
            ++i;
            continue;
        }
        current.push_back(c);
        ++i;
    }
    flush();
    // "feat." / "ft." 前缀片段没有意义，直接丢弃
    std::vector<std::string> kept;
    for (const std::string& piece : out) {
        const std::string lower = lower_ascii(piece);
        if (lower.rfind("feat.", 0) == 0 || lower.rfind("ft.", 0) == 0) continue;
        kept.push_back(piece);
    }
    return kept;
}

bool text_contains(const std::string& haystack, const std::string& needle) {
    if (needle.empty() || haystack.empty()) return false;
    return haystack.find(needle) != std::string::npos;
}

// 本地 author 与在线歌手列表是否有交集（归一化后比较）
bool artists_overlap(const std::string& local_author, const std::vector<std::string>& online_artists) {
    const std::vector<std::string> local_parts = split_artists(local_author);
    if (local_parts.empty()) return false;
    std::vector<std::string> online_norm;
    online_norm.reserve(online_artists.size());
    for (const std::string& artist : online_artists) {
        const std::string norm = normalize_text(artist);
        if (!norm.empty()) online_norm.push_back(norm);
    }
    if (online_norm.empty()) return false;
    for (const std::string& part : local_parts) {
        const std::string norm = normalize_text(part);
        if (norm.empty()) continue;
        for (const std::string& candidate : online_norm) {
            if (candidate == norm) return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------- 匹配打分

struct MatchScore {
    bool ok = false;
    int score = 0;
    int duration_diff = -1;   // -1 = 无法比较（任一时长为 0）
    std::string reason;       // 不接受时的原因
};

// 规则（宁缺毋滥：错配会导致错误的在线版本与错误的海报）：
//  * 歌名归一化后必须相同；仅一方包含另一方时降分通过（如 "Roar" vs "Roar Remix"）；
//  * 本地歌手非空时，必须与在线歌手列表有交集，否则拒绝；
//  * 时长差 > 15 秒拒绝；<= 3 秒满分，<= 10 秒降分。
MatchScore score_candidate(const BaseInfo& local, const SearchItem& item) {
    MatchScore out;
    const std::string local_title = normalize_text(local.name);
    const std::string item_title = normalize_text(item.title);
    if (local_title.empty() || item_title.empty()) {
        out.reason = "标题为空，无法匹配";
        return out;
    }
    if (local_title == item_title) {
        out.score += 100;
    } else if (text_contains(item_title, local_title) || text_contains(local_title, item_title)) {
        out.score += 55;
    } else {
        out.reason = "歌名不匹配";
        return out;
    }

    std::vector<std::string> online_artists = item.artists;
    if (online_artists.empty() && !item.sources.empty()) {
        const std::string author = item.sources.front().base.author;
        if (!author.empty()) online_artists.push_back(author);
    }
    if (!trim_ascii(local.author).empty()) {
        if (artists_overlap(local.author, online_artists)) {
            out.score += 40;
        } else {
            out.reason = "歌手不匹配";
            return out;
        }
    }

    if (local.duration_seconds > 0 && item.duration_seconds > 0) {
        out.duration_diff = local.duration_seconds - item.duration_seconds;
        if (out.duration_diff < 0) out.duration_diff = -out.duration_diff;
        if (out.duration_diff <= 3) {
            out.score += 30;
        } else if (out.duration_diff <= 10) {
            out.score += 12;
        } else if (out.duration_diff <= 15) {
            out.score += 2;
        } else {
            out.reason = "时长差过大";
            return out;
        }
    }

    out.ok = true;
    return out;
}

// 条目音质列表（含自身 quality），按 Quality::compare 降序
std::vector<Quality> entry_qualities(const SourceEntry& entry) {
    std::vector<Quality> all = entry.qualities;
    if (entry.quality.is_known()) {
        bool seen = false;
        for (const Quality& q : all) {
            if (q.same_identity(entry.quality)) {
                seen = true;
                break;
            }
        }
        if (!seen) all.push_back(entry.quality);
    }
    std::stable_sort(all.begin(), all.end(),
                     [](const Quality& a, const Quality& b) { return b.compare(a) < 0; });
    return all;
}

// 封面扩展名：先按魔数判断，退化用 .jpg
std::string cover_extension(const std::vector<uint8_t>& data) {
    if (data.size() >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') {
        return ".png";
    }
    if (data.size() >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) return ".jpg";
    if (data.size() >= 6 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F') return ".gif";
    if (data.size() >= 12 && data[8] == 'W' && data[9] == 'E' && data[10] == 'B' && data[11] == 'P') {
        return ".webp";
    }
    if (data.size() >= 2 && data[0] == 'B' && data[1] == 'M') return ".bmp";
    return ".jpg";
}

bool read_file_bytes(const std::filesystem::path& path, std::vector<uint8_t>* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
}

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

}  // namespace

// ---------------------------------------------------------------- IdentifyOptions

IdentifyOptions IdentifyOptions::from_json(const json::Value& v) {
    IdentifyOptions opt;
    if (!v.is_object()) return opt;
    if (const json::Value* sha = v.find("sha"); sha && sha->is_string()) {
        const std::string value = sha->as_string();
        if (!value.empty()) opt.shas.push_back(value);
    }
    if (const json::Value* shas = v.find("shas"); shas && shas->is_array()) {
        for (const auto& item : shas->as_array()) {
            if (item.is_string() && !item.as_string().empty()) opt.shas.push_back(item.as_string());
        }
    }
    opt.source = v.get_string("source");
    opt.page_size = static_cast<int>(v.get_int("page_size", opt.page_size));
    if (opt.page_size < 1) opt.page_size = 1;
    if (opt.page_size > 30) opt.page_size = 30;
    opt.fetch_pic = v.get_bool("fetch_pic", opt.fetch_pic);
    opt.force = v.get_bool("force", opt.force);
    return opt;
}

// ---------------------------------------------------------------- IdentifyService

IdentifyService::IdentifyService(const LibraryPaths& paths, MetaCache& meta, LibraryIndex& index,
                                 PluginRegistry& plugins)
    : paths_(paths), meta_(meta), index_(index), plugins_(plugins) {}

Result<json::Value> IdentifyService::identify(const json::Value& request) {
    IdentifyOptions opt = IdentifyOptions::from_json(request);
    if (opt.shas.empty()) {
        return Result<json::Value>::failure(ErrorCode::InvalidMetadata, "缺少 sha 或 shas");
    }
    if (opt.shas.size() > static_cast<size_t>(opt.max_shas)) {
        opt.shas.resize(static_cast<size_t>(opt.max_shas));
    }

    json::Value results = json::Value::array();
    int matched = 0;
    int already = 0;
    int no_match = 0;
    int failed = 0;
    for (const std::string& sha : opt.shas) {
        auto one = identify_one(sha, opt);
        if (!one.ok()) {
            ++failed;
            json::Value item = json::Value::object();
            item.set("sha", sha);
            item.set("matched", false);
            item.set("action", std::string("failed"));
            item.set("message", one.status().message);
            results.push_back(item);
            continue;
        }
        const json::Value& value = one.value();
        const std::string action = value.get_string("action");
        if (action == "linked") ++matched;
        else if (action == "already") ++already;
        else if (action == "no_match") ++no_match;
        else ++failed;
        results.push_back(value);
    }

    json::Value data = json::Value::object();
    data.set("results", results);
    data.set("total", static_cast<long long>(opt.shas.size()));
    data.set("matched", static_cast<long long>(matched));
    data.set("already", static_cast<long long>(already));
    data.set("no_match", static_cast<long long>(no_match));
    data.set("failed", static_cast<long long>(failed));
    return Result<json::Value>::success(data);
}

Result<json::Value> IdentifyService::identify_one(const std::string& sha, const IdentifyOptions& opt) {
    json::Value out = json::Value::object();
    out.set("sha", sha);

    auto path = meta_.find_by_sha(sha);
    if (!path.has_value()) {
        out.set("matched", false);
        out.set("action", std::string("failed"));
        out.set("message", "曲库中没有该 sha 的曲目");
        return Result<json::Value>::success(out);
    }
    auto base_result = meta_.read(*path);
    if (!base_result.ok()) {
        out.set("matched", false);
        out.set("action", std::string("failed"));
        out.set("message", base_result.status().message);
        return Result<json::Value>::success(out);
    }
    BaseInfo base = base_result.value();
    if (base.platform != "local") {
        out.set("matched", false);
        out.set("action", std::string("failed"));
        out.set("message", "只支持识别本地曲目（platform=local）");
        return Result<json::Value>::success(out);
    }

    out.set("name", base.name);
    out.set("author", base.author);

    // 已关联过在线版本：默认直接返回，不重复联网
    if (!base.match_platform.empty() && !base.item_id.empty() && !opt.force) {
        out.set("matched", true);
        out.set("action", std::string("already"));
        out.set("platform", base.match_platform);
        out.set("item_id", base.item_id);
        out.set("message", "已关联在线版本（" + base.match_platform + "）");
        return Result<json::Value>::success(out);
    }

    Candidate candidate = find_match(base, opt);
    if (!candidate.matched) {
        out.set("matched", false);
        out.set("action", std::string("no_match"));
        out.set("message", candidate.reason.empty() ? "未找到匹配的在线版本" : candidate.reason);
        return Result<json::Value>::success(out);
    }

    // ---- 回写：在线版本链接 + 海报（不改动歌名/歌手/时长，避免 song_key 漂移）----
    bool changed = false;
    if (base.item_id != candidate.item_id || base.match_platform != candidate.platform) {
        base.item_id = candidate.item_id;
        base.match_platform = candidate.platform;
        changed = true;
    }
    if (base.album.empty() && !candidate.album.empty()) {
        base.album = candidate.album;
        changed = true;
    }
    if (base.pic.empty() && opt.fetch_pic && !candidate.pic_url.empty()) {
        const std::string saved = save_cover_from_url(candidate.pic_url);
        if (!saved.empty()) {
            base.pic = saved;
            out.set("pic", saved);
            changed = true;
        }
    }

    if (changed) {
        // 同一 sha 同一路径：Overwrite 语义等价于「读-改-写」，不会丢字段
        auto put = meta_.put(base, ConflictPolicy::Overwrite);
        if (!put.ok()) {
            out.set("matched", false);
            out.set("action", std::string("failed"));
            out.set("message", put.status().message);
            return Result<json::Value>::success(out);
        }
        index_.upsert(put.value().base, put.value().path);
        const Status saved = index_.save();
        if (!saved.ok()) {
            RM_LOG_WARN("identify", "索引保存失败: " + saved.message);
        }
    }

    out.set("matched", true);
    out.set("action", std::string("linked"));
    out.set("platform", candidate.platform);
    out.set("item_id", candidate.item_id);
    out.set("online_name", candidate.name);
    out.set("online_author", candidate.author);
    out.set("album", base.album);
    out.set("duration", static_cast<long long>(candidate.duration_seconds));
    out.set("score", static_cast<long long>(candidate.score));
    out.set("duration_diff", static_cast<long long>(candidate.duration_diff));
    json::Value qualities = json::Value::array();
    for (const Quality& q : candidate.qualities) qualities.push_back(q.to_json());
    out.set("qualities", qualities);
    out.set("message", "已匹配「" + candidate.platform + "」在线版本");
    return Result<json::Value>::success(out);
}

IdentifyService::Candidate IdentifyService::find_match(const BaseInfo& base, const IdentifyOptions& opt) {
    Candidate best;
    best.matched = false;

    // 搜索关键词：优先「歌名」，带歌手更准；空歌名时退回歌手
    std::string keyword = trim_ascii(base.name);
    if (!keyword.empty() && !trim_ascii(base.author).empty()) {
        keyword += " " + trim_ascii(base.author);
    } else if (keyword.empty()) {
        keyword = trim_ascii(base.author);
    }
    if (keyword.empty()) {
        best.reason = "缺少歌名与歌手信息，无法搜索";
        return best;
    }

    json::Value options = json::Value::object();
    options.set("page", static_cast<long long>(1));
    options.set("page_size", static_cast<long long>(opt.page_size));
    options.set("type", std::string("song"));

    for (const auto& plugin : plugins_.enabled_sources()) {
        const std::string platform =
            plugin->platforms().empty() ? plugin->manifest().id : plugin->platforms().front();
        if (!opt.source.empty() && platform != opt.source) continue;
        Status capable = plugins_.check_capability(platform, "search");
        if (!capable.ok()) continue;

        auto result = plugin->search_all(keyword, options);
        if (!result.ok()) {
            RM_LOG_DEBUG("identify", "源 " + platform + " 搜索失败: " + result.status().message);
            continue;
        }
        for (const SearchItem& item : result.value()) {
            if (item.sources.empty()) continue;
            const SourceEntry& entry = item.sources.front();
            if (entry.item_id.empty()) continue;
            MatchScore score = score_candidate(base, item);
            if (!score.ok) continue;
            const int quality_rank =
                entry.quality.is_known() ? quality_level_rank(entry.quality.level) : 0;
            // 先比分，再比音质，最后比来源顺序（enabled_sources 的顺序即稳定优先级）
            const bool better = !best.matched || score.score > best.score ||
                                (score.score == best.score && quality_rank > best.quality_rank());
            if (!better) continue;
            best.matched = true;
            best.reason.clear();
            best.platform = platform;
            best.item_id = entry.item_id;
            best.name = entry.base.name.empty() ? item.title : entry.base.name;
            best.author = entry.base.author.empty()
                              ? (item.artists.empty() ? std::string() : item.artists.front())
                              : entry.base.author;
            best.album = entry.base.album.empty() ? item.album : entry.base.album;
            best.duration_seconds = entry.base.duration_seconds > 0 ? entry.base.duration_seconds
                                                                    : item.duration_seconds;
            best.pic_url = entry.base.pic;
            best.score = score.score;
            best.duration_diff = score.duration_diff;
            best.qualities = entry_qualities(entry);
            best.best_quality = entry.quality;
            best.quality_rank_ = quality_rank;
        }
    }
    if (!best.matched && best.reason.empty()) {
        best.reason = "所有音源均未找到匹配的在线版本";
    }
    return best;
}

std::string IdentifyService::save_cover_from_url(const std::string& url) {
    if (!is_remote_url(url)) return std::string();
    std::error_code ec;
    const std::filesystem::path dir = paths_.tmp();
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path tmp = dir / ("identify_cover_" + std::to_string(++tmp_seq_) + ".bin");
    long long received = 0;
    long long total = -1;
    std::string error;
    const bool ok = http_fetch_file(url, tmp, &received, &total, &error);
    if (!ok) {
        RM_LOG_DEBUG("identify", "海报下载失败: " + url + " (" + error + ")");
        std::filesystem::remove(tmp, ec);
        return std::string();
    }
    std::vector<uint8_t> bytes;
    if (!read_file_bytes(tmp, &bytes) || bytes.size() < 64) {
        std::filesystem::remove(tmp, ec);
        return std::string();
    }
    const std::string digest = crypto::sha256_hex(
        std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    std::string hash16 = digest.substr(0, 16);
    if (hash16.size() < 16) hash16.append(16 - hash16.size(), '0');
    const std::string relative = "covers/" + hash16 + cover_extension(bytes);
    const std::filesystem::path target = paths_.root / relative;
    if (!write_bytes_file(target.parent_path(), target, bytes)) {
        std::filesystem::remove(tmp, ec);
        RM_LOG_WARN("identify", "海报写入失败: " + generic_utf8(target));
        return std::string();
    }
    std::filesystem::remove(tmp, ec);
    return relative;
}

}  // namespace radmusic
