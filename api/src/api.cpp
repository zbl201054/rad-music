#include "radmusic/api.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <system_error>
#include <vector>

#include "radmusic/cache.hpp"
#include "radmusic/collections.hpp"
#include "radmusic/decrypt.hpp"
#include "radmusic/download.hpp"
#include "radmusic/groupkey.hpp"
#include "radmusic/identify.hpp"
#include "radmusic/log.hpp"
#include "radmusic/merge.hpp"
#include "radmusic/migrate.hpp"
#include "radmusic/settings.hpp"
#include "radmusic/update.hpp"

namespace radmusic {
namespace {

std::vector<std::string> split_path(std::string_view path) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < path.size()) {
        while (i < path.size() && path[i] == '/') ++i;
        const size_t start = i;
        while (i < path.size() && path[i] != '/') ++i;
        if (i > start) out.emplace_back(path.substr(start, i - start));
    }
    return out;
}

std::vector<std::string> string_array(const json::Value& v, std::string_view key) {
    std::vector<std::string> out;
    const json::Value* arr = v.find(key);
    if (!arr || !arr->is_array()) return out;
    for (const auto& item : arr->as_array()) out.push_back(item.as_string());
    return out;
}

json::Value track_to_json(const LoadedTrack& track) {
    json::Value v = json::Value::object();
    v.set("base", track.base.to_json());
    v.set("play_path", track.play_path);
    v.set("decrypted", track.decrypted);
    v.set("sha_warning", track.sha_warning);
    v.set("song_key", track.meta_path.song_key);
    v.set("quality_key", track.meta_path.quality_key);
    v.set("lyric_lines", static_cast<long long>(track.lyric.lines.size()));
    return v;
}

// ---------------- 本地曲库搜索（2.5：sources 可包含 local） ----------------
std::string ascii_lower_ascii(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text) {
        out.push_back(static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c));
    }
    return out;
}

bool text_hits(const std::string& value, const std::string& needle) {
    if (value.empty()) return false;
    return ascii_lower_ascii(value).find(needle) != std::string::npos;
}

// 在曲库缓存中按 歌名 / 歌手 / 专辑 / sha 做包含匹配；只返回本地文件（platform=local）
std::vector<SourceEntry> local_search_entries(const MetaCache& meta, const std::string& keyword) {
    std::vector<SourceEntry> out;
    const std::string needle = ascii_lower_ascii(keyword);
    if (needle.empty()) return out;
    for (const auto& entry : meta.list_all_with_paths()) {
        const BaseInfo& base = entry.second;
        if (base.platform != "local") continue;
        if (!text_hits(base.name, needle) && !text_hits(base.author, needle) &&
            !text_hits(base.album, needle) && !text_hits(base.sha, needle)) {
            continue;
        }
        SourceEntry item;
        item.platform = "local";
        item.item_id = base.sha;
        item.available = true;
        item.base = base;
        item.base.item_id = base.sha;
        item.quality = base.quality;
        if (base.quality.is_known() || !base.quality.quality_id.empty()) {
            item.qualities.push_back(base.quality);
        }
        item.location = base.location;
        out.push_back(std::move(item));
        if (out.size() >= 500) break;   // 防御：本地曲库过大时限制单次候选量
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------- ApiRequest / ApiResponse

std::string ApiRequest::query_value(std::string_view key, std::string def) const {
    for (const auto& kv : query) {
        if (kv.first == key) return kv.second;
    }
    return def;
}

std::string ApiRequest::segment(size_t index, std::string def) const {
    if (index < path_segments.size()) return path_segments[index];
    return def;
}

json::Value ApiResponse::to_json() const {
    json::Value v = json::Value::object();
    v.set("code", static_cast<long long>(static_cast<int>(code)));
    v.set("message", message.empty() ? std::string(error_id(code)) : message);
    v.set("data", data);
    return v;
}

ApiResponse ApiResponse::success(json::Value data_value) {
    ApiResponse r;
    r.code = ErrorCode::Ok;
    r.message = "ok";
    r.data = std::move(data_value);
    return r;
}

ApiResponse ApiResponse::failure(ErrorCode code_value, std::string message_value) {
    ApiResponse r;
    r.code = code_value;
    r.message = message_value.empty() ? std::string(error_id(code_value)) : std::move(message_value);
    return r;
}

ApiResponse ApiResponse::from_status(const Status& status) {
    if (status.ok()) return success();
    return failure(status.code, status.message);
}

// ---------------------------------------------------------------- 调音状态

json::Value EqState::to_json() const {
    json::Value v = json::Value::object();
    v.set("enabled", enabled);
    if (!preset.empty()) v.set("preset", preset);
    v.set("preamp_db", preamp_db);
    json::Value bands_json = json::Value::array();
    for (const auto& band : bands) {
        json::Value b = json::Value::object();
        b.set("type", band.type);
        b.set("freq", band.freq);
        b.set("gain", band.gain_db);
        b.set("q", band.q);
        bands_json.push_back(b);
    }
    v.set("bands", bands_json);
    v.set("latency_ms", static_cast<long long>(latency_ms));
    v.set("engine", engine);
    return v;
}

EqState EqState::from_json(const json::Value& v) {
    EqState state;
    if (!v.is_object()) return state;
    state.enabled = v.get_bool("enabled", false);
    state.preset = v.get_string("preset");
    state.preamp_db = v.get_double("preamp_db", 0.0);
    if (const json::Value* bands = v.find("bands"); bands && bands->is_array()) {
        for (const auto& b : bands->as_array()) {
            EqBand band;
            band.type = b.get_string("type", "peak");
            band.freq = b.get_double("freq", 1000.0);
            band.gain_db = b.get_double("gain", 0.0);
            band.q = b.get_double("q", 1.0);
            state.bands.push_back(band);
        }
    }
    state.latency_ms = static_cast<int>(v.get_int("latency_ms", 0));
    state.engine = v.get_string("engine", "builtin");
    return state;
}

// ---------------------------------------------------------------- ApiDispatcher

ApiDispatcher::ApiDispatcher(const LibraryPaths& paths, PluginRegistry& plugins, CoreEngine& engine,
                             MetaCache& meta, LibraryIndex& index)
    : paths_(paths), plugins_(plugins), engine_(engine), meta_(meta), index_(index) {}

Status ApiDispatcher::set_eq(const EqState& state) {
    eq_ = state;
    if (!state.preset.empty()) {
        const auto dir = paths_.presets() / "eq";
        auto written = write_text_file_atomic(dir, dir / (state.preset + ".json"), state.to_json().dump(2));
        if (!written.ok()) return written;
    }
    return Status::success();
}

ApiResponse ApiDispatcher::handle(const ApiRequest& request_in) {
    ApiRequest request = request_in;
    if (request.path_segments.empty()) request.path_segments = split_path(request.path);
    const std::string path = request.path;
    const std::vector<std::string>& seg = request.path_segments;
    // /music/<group>[/<rest>...]
    if (seg.size() < 2 || seg[0] != "music") {
        return ApiResponse::failure(ErrorCode::RequestFailed, "未知路径: " + path);
    }
    const std::string& group = seg[1];

    if (group == "load") return h_load(request);
    if (group == "start") return h_play(request, "start");
    if (group == "pause") return h_play(request, "pause");
    if (group == "resume") return h_play(request, "resume");
    if (group == "stop") return h_play(request, "stop");
    if (group == "seek") return h_play(request, "seek");
    if (group == "status") return h_status(request);
    if (group == "info") return h_info(request);
    if (group == "verify") return h_verify(request);
    if (group == "search") return h_search(request);
    if (group == "request") return h_request(request);
    if (group == "lyric") return h_lyric(request);
    if (group == "quality") return h_quality(request);
    if (group == "cache") return h_cache(request);
    if (group == "download") return h_download(request);
    if (group == "import") return h_import(request);
    if (group == "identify") return h_identify(request);
    if (group == "library") return h_library(request);
    if (group == "eq") return h_eq(request);
    if (group == "dsp") return h_dsp(request);
    // v0.10：歌单 / 专辑 / MV。playlist/album 都先看第二段是不是「search」，
    // 再走 id-based 路由，否则 /music/album/search 会被当成「专辑 id=search」。
    if (group == "playlist") {
        const std::string action = request.segment(2);
        if (action == "search") return h_playlist_search(request);
        return h_playlist(request);
    }
    if (group == "album") {
        const std::string action = request.segment(2);
        if (action == "search") return h_album_search(request);
        return h_album(request);
    }
    if (group == "mv") return h_mv(request);
    if (group == "logs") return h_logs(request);
    // 设置页扩展
    if (group == "plugins" || group == "plugin") return h_plugins(request);
    if (group == "settings") return h_settings(request);
    if (group == "data") return h_data(request);
    if (group == "restart") return h_restart(request);
    // 检查更新（GET /music/update，POST /music/update/{check|download|apply}）
    if (group == "update") return h_update(request);
    // 「打开所在文件夹」：下载完成后在资源管理器里定位文件
    if (group == "open-folder" || group == "openfolder" || group == "reveal") return h_open_folder(request);

    return ApiResponse::failure(ErrorCode::RequestFailed, "未知接口: " + path);
}

ApiResponse ApiDispatcher::call(std::string_view method_and_path, const json::Value& body) {
    ApiRequest request;
    const size_t space = method_and_path.find(' ');
    std::string target(method_and_path);
    if (space != std::string_view::npos) {
        request.method = std::string(method_and_path.substr(0, space));
        target = std::string(method_and_path.substr(space + 1));
    }
    const size_t qmark = target.find('?');
    if (qmark != std::string::npos) {
        const std::string query_string = target.substr(qmark + 1);
        target = target.substr(0, qmark);
        size_t i = 0;
        while (i < query_string.size()) {
            const size_t amp = query_string.find('&', i);
            const std::string pair = query_string.substr(i, amp == std::string::npos ? std::string::npos : amp - i);
            const size_t eq = pair.find('=');
            if (eq == std::string::npos) {
                request.query.emplace_back(pair, std::string());
            } else {
                request.query.emplace_back(pair.substr(0, eq), pair.substr(eq + 1));
            }
            if (amp == std::string::npos) break;
            i = amp + 1;
        }
    }
    request.path = target;
    request.path_segments = split_path(target);
    request.body = body;
    return handle(request);
}

ApiResponse ApiDispatcher::h_load(const ApiRequest& request) {
    std::string key = request.body.get_string("meta");
    if (key.empty()) key = request.body.get_string("sha");
    if (key.empty()) {
        const std::string platform = request.body.get_string("platform");
        const std::string item_id = request.body.get_string("id");
        if (!platform.empty() && !item_id.empty()) key = "online:" + platform + ":" + item_id;
    }
    if (key.empty()) return ApiResponse::failure(ErrorCode::InvalidMetadata, "缺少 meta/sha 或 platform+id");

    const std::string password = request.body.get_string("password");
    auto loaded = engine_.load(key, password);
    if (!loaded.ok()) return ApiResponse::from_status(loaded.status());

    engine_.meta_cache();   // 缓存已在 engine 内写入
    if (auto path = meta_.find_by_sha(loaded.value().base.sha); path.has_value()) {
        index_.upsert(loaded.value().base, *path);
        index_.save();
    }
    return ApiResponse::success(track_to_json(loaded.value()));
}

ApiResponse ApiDispatcher::h_play(const ApiRequest& request, std::string_view action) {
    Status status = Status::success();
    if (action == "start") {
        status = engine_.start();
    } else if (action == "pause") {
        status = engine_.pause();
    } else if (action == "resume") {
        status = engine_.resume();
    } else if (action == "stop") {
        status = engine_.stop();
    } else if (action == "seek") {
        double seconds = request.body.get_double("seconds", request.body.get_double("position", -1.0));
        if (seconds < 0.0) {
            const std::string q = request.query_value("seconds");
            if (!q.empty()) seconds = std::atof(q.c_str());
        }
        if (seconds < 0.0) return ApiResponse::failure(ErrorCode::InvalidMetadata, "缺少 seconds");
        status = engine_.seek(seconds);
    }
    if (!status.ok()) return ApiResponse::from_status(status);
    const PlayStatus play = engine_.status();
    if (on_status) on_status(play);
    return ApiResponse::success(play.to_json());
}

ApiResponse ApiDispatcher::h_status(const ApiRequest&) {
    return ApiResponse::success(engine_.status().to_json());
}

ApiResponse ApiDispatcher::h_info(const ApiRequest& request) {
    const std::string sha = request.query_value("sha", request.body.get_string("sha"));
    auto path = meta_.find_by_sha(sha);
    if (!path.has_value()) return ApiResponse::failure(ErrorCode::FileNotFound, "缓存中没有该 sha 的 base.json");
    auto base = meta_.read(*path);
    if (!base.ok()) return ApiResponse::from_status(base.status());
    json::Value data = json::Value::object();
    data.set("base", base.value().to_json());
    data.set("song_key", path->song_key);
    data.set("quality_key", path->quality_key);
    return ApiResponse::success(data);
}

ApiResponse ApiDispatcher::h_verify(const ApiRequest& request) {
    const std::string sha = request.body.get_string("sha", request.query_value("sha"));
    const std::string password = request.body.get_string("password");
    if (sha.empty()) return ApiResponse::failure(ErrorCode::InvalidMetadata, "缺少 sha");
    Decryptor decryptor(paths_);
    auto sec = decryptor.load_sec(sha);
    if (!sec.ok()) return ApiResponse::from_status(sec.status());
    Status verified = decryptor.verify_password(sec.value(), password);
    if (!verified.ok()) return ApiResponse::from_status(verified);
    json::Value data = json::Value::object();
    data.set("valid", true);
    return ApiResponse::success(data);
}

ApiResponse ApiDispatcher::h_search(const ApiRequest& request) {
    SearchRequest search;
    search.keyword = request.body.get_string("keyword", request.query_value("keyword"));
    if (search.keyword.empty()) return ApiResponse::failure(ErrorCode::InvalidMetadata, "缺少 keyword");
    search.sources = string_array(request.body, "sources");
    if (search.sources.empty()) {
        const std::string single = request.query_value("sources");
        if (!single.empty()) search.sources.push_back(single);
    }
    search.type = request.body.get_string("type", "song");
    search.page = static_cast<int>(request.body.get_int("page", 1));
    search.page_size = static_cast<int>(request.body.get_int("page_size", 20));
    search.timeout_ms = static_cast<int>(request.body.get_int("timeout_ms", 5000));
    search.strategy = request.body.get_string("strategy", "fanout");
    search.fold = request.body.get_bool("fold", true);

    // 筛选与排序（v0.11）：在服务端过滤更准确，避免分页把结果切碎
    search.quality_min = request.body.get_string("quality_min", request.query_value("quality_min"));
    search.duration_min = static_cast<int>(request.body.get_int("duration_min", 0));
    search.duration_max = static_cast<int>(request.body.get_int("duration_max", 0));
    search.available_only = request.body.get_bool("available_only", false);
    search.sort = request.body.get_string("sort", "relevance");

    // sources 为空 = 全部已启用的源；本地曲库始终作为宿主的 "local" 源参与
    bool want_local = search.sources.empty();
    for (const std::string& platform : search.sources) {
        if (platform == "local") {
            want_local = true;
            break;
        }
    }
    std::vector<SourceEntry> local_entries;
    const bool song_type = search.type.empty() || search.type == "song";
    if (want_local && song_type) local_entries = local_search_entries(meta_, search.keyword);

    auto sources = plugins_.enabled_sources();
    auto result = execute_search(search, sources, source_priority_, local_entries,
                                 want_local ? std::string("local") : std::string());
    if (!result.ok()) return ApiResponse::from_status(result.status());
    return ApiResponse::success(result.value().to_json());
}

ApiResponse ApiDispatcher::h_request(const ApiRequest& request) {
    const std::string source = request.body.get_string("source");
    const std::string op = request.body.get_string("op");
    if (source.empty() || op.empty()) {
        return ApiResponse::failure(ErrorCode::InvalidMetadata, "缺少 source 或 op");
    }
    Status capable = plugins_.check_capability(source, op);
    if (!capable.ok()) return ApiResponse::from_status(capable);

    auto plugin = plugins_.source_for_platform(source);
    if (!plugin) return ApiResponse::failure(ErrorCode::PlatformError, "无可用插件: " + source);

    const json::Value* params = request.body.find("params");
    json::Value empty = json::Value::object();
    auto result = plugin->request(op, params ? *params : empty);
    if (!result.ok()) return ApiResponse::from_status(result.status());

    json::Value data = json::Value::object();
    data.set("source", source);
    data.set("op", op);
    data.set("result", result.value());
    return ApiResponse::success(data);
}

ApiResponse ApiDispatcher::h_lyric(const ApiRequest& request) {
    const std::string sha = request.query_value("sha", request.body.get_string("sha"));
    std::optional<BaseInfo> base;
    if (!sha.empty()) {
        if (auto path = meta_.find_by_sha(sha); path.has_value()) {
            auto read = meta_.read(*path);
            if (read.ok()) base = read.value();
        }
    } else {
        const std::string platform = request.query_value("source", request.body.get_string("source"));
        const std::string item_id = request.body.get_string("id", request.query_value("id"));
        if (!platform.empty() && !item_id.empty()) {
            if (auto path = meta_.find_by_platform_id(platform, item_id); path.has_value()) {
                auto read = meta_.read(*path);
                if (read.ok()) base = read.value();
            }
        }
    }
    if (!base.has_value()) {
        return ApiResponse::failure(ErrorCode::FileNotFound, "缓存中没有对应的 base.json（请先 load）");
    }

    auto lyric = engine_.load_lyric(*base);
    if (!lyric.ok()) {
        // 1015 非致命：返回空歌词而不是错误（1.5 规则 2 / 2.8 规则 3）
        if (lyric.status().code == ErrorCode::LyricUnavailable) {
            json::Value data = json::Value::object();
            data.set("sha", base->sha);
            data.set("origin", "none");
            data.set("timed", false);
            data.set("line_count", 0);
            json::Value empty = json::Value::array();
            data.set("lines", empty);
            ApiResponse response = ApiResponse::success(data);
            response.code = ErrorCode::LyricUnavailable;
            response.message = error_id(ErrorCode::LyricUnavailable);
            return response;
        }
        return ApiResponse::from_status(lyric.status());
    }

    json::Value data = json::Value::object();
    data.set("sha", base->sha);
    data.set("origin", base->platform == "local" ? "local" : "online");
    data.set("source", lyric.value().source.empty() ? json::Value() : json::Value(lyric.value().source));
    data.set("format", lyric.value().format);
    data.set("timed", lyric.value().timed);
    data.set("offset_ms", static_cast<long long>(lyric.value().offset_ms));
    data.set("line_count", static_cast<long long>(lyric.value().lines.size()));
    data.set("lyric", lyric.value().to_json(true, true, true));
    return ApiResponse::success(data);
}

ApiResponse ApiDispatcher::h_quality(const ApiRequest& request) {
    const std::string platform = request.segment(2);
    const std::string item_id = request.segment(3);

    if (!platform.empty() && !item_id.empty()) {
        // 2.13 quality_of：转发 request(op="get_qualities")
        Status capable = plugins_.check_capability(platform, "get_qualities");
        if (!capable.ok()) return ApiResponse::from_status(capable);
        auto plugin = plugins_.source_for_platform(platform);
        if (!plugin) return ApiResponse::failure(ErrorCode::PlatformError, "无可用插件: " + platform);
        json::Value params = json::Value::object();
        params.set("id", item_id);
        auto result = plugin->request("get_qualities", params);
        if (!result.ok()) return ApiResponse::from_status(result.status());
        json::Value data = json::Value::object();
        data.set("platform", platform);
        data.set("id", item_id);
        data.set("qualities", result.value());
        return ApiResponse::success(data);
    }

    // 2.13 quality_levels：以插件清单为唯一来源
    json::Value sources_json = json::Value::array();
    for (const auto& manifest : plugins_.manifests()) {
        if (manifest.type != PluginType::Source) continue;
        if (!platform.empty() && !manifest.provides_platform(platform)) continue;
        json::Value entry = json::Value::object();
        entry.set("plugin_id", manifest.id);
        entry.set("platform", manifest.platforms.empty() ? std::string() : manifest.platforms.front());
        json::Value levels = json::Value::array();
        for (const auto& spec : manifest.quality_levels) levels.push_back(spec.to_json());
        entry.set("levels", levels);
        sources_json.push_back(entry);
    }
    json::Value data = json::Value::object();
    data.set("sources", sources_json);
    return ApiResponse::success(data);
}

ApiResponse ApiDispatcher::h_cache(const ApiRequest& request) {
    if (!cache_) return ApiResponse::failure(ErrorCode::RequestFailed, "缓存模块未装配");

    const std::string action = request.segment(2);
    if (action.empty()) {
        CacheStats stats = cache_->stats();
        json::Value data = json::Value::object();
        data.set("usage_bytes", static_cast<long long>(stats.usage_bytes));
        data.set("limit_bytes", static_cast<long long>(stats.limit_bytes));
        json::Value layers = json::Value::object();
        json::Value layer_list = json::Value::array();
        for (const auto& layer : stats.layers) {
            json::Value item = json::Value::object();
            item.set("name", layer.name);
            item.set("entries", static_cast<long long>(layer.entries));
            item.set("bytes", static_cast<long long>(layer.bytes));
            layer_list.push_back(item);
            layers.set(layer.name, static_cast<long long>(layer.bytes));
        }
        data.set("layers", layers);
        data.set("layer_list", layer_list);
        data.set("broken_meta", static_cast<long long>(stats.broken_meta));
        data.set("pending_eviction", static_cast<long long>(stats.pending_eviction));
        return ApiResponse::success(data);
    }

    if (action == "clear") {
        std::string type = request.body.get_string("type", request.query_value("type", "all"));
        size_t skipped = 0;
        auto cleared = cache_->clear(type, &skipped);
        if (!cleared.ok()) return ApiResponse::from_status(cleared.status());
        json::Value data = json::Value::object();
        data.set("cleared", static_cast<long long>(cleared.value()));
        data.set("skipped", static_cast<long long>(skipped));
        return ApiResponse::success(data);
    }

    if (action == "rebuild") {
        const std::string song_key = request.body.get_string("song_key", request.query_value("song_key"));
        const std::string quality_key = request.body.get_string("quality_key", request.query_value("quality_key"));
        Status status = cache_->rebuild_meta(song_key, quality_key);
        if (!status.ok()) return ApiResponse::from_status(status);
        json::Value data = json::Value::object();
        data.set("rebuilt", true);
        return ApiResponse::success(data);
    }

    if (action == "config") {
        CacheConfig config = cache_->config();
        if (request.body.contains("limit_meta")) {
            config.limit_meta = static_cast<std::uintmax_t>(request.body.get_int("limit_meta", 0));
        }
        if (request.body.contains("limit_audio")) {
            config.limit_audio = static_cast<std::uintmax_t>(request.body.get_int("limit_audio", 0));
        }
        if (request.body.contains("limit_lyric")) {
            config.limit_lyric = static_cast<std::uintmax_t>(request.body.get_int("limit_lyric", 0));
        }
        if (request.body.contains("limit_cover")) {
            config.limit_cover = static_cast<std::uintmax_t>(request.body.get_int("limit_cover", 0));
        }
        if (request.body.contains("preload")) config.preload = request.body.get_bool("preload", false);
        Status status = cache_->set_config(config);
        if (!status.ok()) return ApiResponse::from_status(status);
        return ApiResponse::success(config.to_json());
    }

    return ApiResponse::failure(ErrorCode::RequestFailed, "未知缓存操作: " + action);
}

ApiResponse ApiDispatcher::h_download(const ApiRequest& request) {
    if (!downloads_) return ApiResponse::failure(ErrorCode::RequestFailed, "下载模块未装配");

    const std::string task_id = request.segment(2);
    const std::string action = request.segment(3);

    if (task_id.empty()) {
        // 请求里的 filename 是命名模板（与响应里的 filename=真实文件名区分），也接受 filename_template
        auto template_of = [](const json::Value& body) {
            std::string tmpl = body.get_string("filename_template");
            if (tmpl.empty()) tmpl = body.get_string("filename");
            if (tmpl.empty()) tmpl = "{artist} - {title} [{quality}]";
            return tmpl;
        };
        if (request.method == "POST") {
            json::Value created = json::Value::array();
            const json::Value* tasks = request.body.find("tasks");
            if (tasks && tasks->is_array() && !tasks->as_array().empty()) {
                for (const auto& item : tasks->as_array()) {
                    auto task = downloads_->create(item.get_string("source"), item.get_string("item_id"),
                                                   item.get_string("quality", "auto"), template_of(item));
                    if (!task.ok()) return ApiResponse::from_status(task.status());
                    created.push_back(task.value().to_json());
                }
            } else {
                auto task = downloads_->create(request.body.get_string("source"),
                                               request.body.get_string("item_id"),
                                               request.body.get_string("quality", "auto"),
                                               template_of(request.body));
                if (!task.ok()) return ApiResponse::from_status(task.status());
                created.push_back(task.value().to_json());
            }
            json::Value data = json::Value::object();
            data.set("tasks", created);
            downloads_->save();   // 立即落盘，避免进程重启丢任务
            return ApiResponse::success(data);
        }
        json::Value list = json::Value::array();
        for (const auto& task : downloads_->list()) list.push_back(task.to_json());
        json::Value data = json::Value::object();
        data.set("tasks", list);
        return ApiResponse::success(data);
    }

    if (request.method == "DELETE") {
        auto removed = downloads_->remove(task_id, request.query_value("delete_file") == "true");
        if (!removed.ok()) return ApiResponse::from_status(removed.status());
        json::Value data = json::Value::object();
        data.set("removed", removed.value());
        return ApiResponse::success(data);
    }

    if (action == "pause") {
        Status status = downloads_->pause(task_id);
        if (!status.ok()) return ApiResponse::from_status(status);
    } else if (action == "resume") {
        Status status = downloads_->resume(task_id);
        if (!status.ok()) return ApiResponse::from_status(status);
    } else if (action == "cancel") {
        Status status = downloads_->cancel(task_id);
        if (!status.ok()) return ApiResponse::from_status(status);
    } else if (action == "open-folder" || action == "open_folder") {
        // 便捷别名：POST /music/download/{id}/open-folder，等价于 /music/open-folder {task_id}
        ApiRequest sub = request;
        json::Value body = json::Value::object();
        body.set("task_id", task_id);
        sub.body = body;
        return h_open_folder(sub);
    } else {
        return ApiResponse::failure(ErrorCode::RequestFailed, "未知下载操作: " + action);
    }

    auto task = downloads_->get(task_id);
    if (!task.ok()) return ApiResponse::from_status(task.status());
    return ApiResponse::success(task.value().to_json());
}

ApiResponse ApiDispatcher::h_import(const ApiRequest& request) {
    if (!importer_) return ApiResponse::failure(ErrorCode::RequestFailed, "导入模块未装配");
    const std::string action = request.segment(2);

    if (action == "scan") {
        auto result = importer_->scan(request.body);
        if (!result.ok()) return ApiResponse::from_status(result.status());
        return ApiResponse::success(result.value());
    }
    if (action.empty()) {
        auto result = importer_->run(request.body);
        if (!result.ok()) return ApiResponse::from_status(result.status());
        return ApiResponse::success(result.value());
    }
    // /music/import/{id}[/report]
    const std::string import_id = action;
    if (request.segment(3) == "report") {
        auto result = importer_->report(import_id);
        if (!result.ok()) return ApiResponse::from_status(result.status());
        return ApiResponse::success(result.value());
    }
    auto result = importer_->status(import_id);
    if (!result.ok()) return ApiResponse::from_status(result.status());
    return ApiResponse::success(result.value());
}

// 本地曲目联网识别（1.10 扩展）：搜索在线版本 → 回写 item_id/match_platform/海报
ApiResponse ApiDispatcher::h_identify(const ApiRequest& request) {
    if (!identifier_) return ApiResponse::failure(ErrorCode::RequestFailed, "识别模块未装配");
    if (request.method != "POST") {
        return ApiResponse::failure(ErrorCode::InvalidMetadata, "请使用 POST /music/identify");
    }
    auto result = identifier_->identify(request.body);
    if (!result.ok()) return ApiResponse::from_status(result.status());
    return ApiResponse::success(result.value());
}

namespace {

// 是否允许删除某个路径：必须严格位于 root 之内（root 自身与外部文件一律拒绝）
bool within_dir(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    std::error_code ec;
    const auto root_path = std::filesystem::absolute(root, ec).lexically_normal();
    if (ec) return false;
    const auto target_path = std::filesystem::absolute(candidate, ec).lexically_normal();
    if (ec) return false;
    auto root_it = root_path.begin();
    auto target_it = target_path.begin();
    for (; root_it != root_path.end(); ++root_it, ++target_it) {
        if (target_it == target_path.end()) return false;
        if (*root_it != *target_it) return false;
    }
    return target_it != target_path.end();
}

// 目录字节数（删除前统计，跳过无权限项）
std::uintmax_t directory_size(const std::filesystem::path& dir) {
    std::error_code ec;
    std::uintmax_t total = 0;
    std::filesystem::recursive_directory_iterator it(
        dir, std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::recursive_directory_iterator end;
    while (!ec && it != end) {
        if (it->is_regular_file(ec)) {
            std::error_code size_ec;
            const auto size = it->file_size(size_ec);
            if (!size_ec) total += size;
        }
        it.increment(ec);
    }
    return total;
}

bool truthy(std::string_view value, bool fallback) {
    const std::string text = ascii_lower_ascii(value);
    if (text == "true" || text == "1" || text == "yes" || text == "on") return true;
    if (text == "false" || text == "0" || text == "no" || text == "off") return false;
    return fallback;
}

}  // namespace

// 本地曲库列表：默认全部，?platform=local 只列本地导入曲目
ApiResponse ApiDispatcher::h_library(const ApiRequest& request) {
    if (request.method == "DELETE") return library_remove(request);
    // 自定义合并：POST /music/library/{merge|unmerge}、GET /music/library/merges
    const std::string action = request.segment(2);
    if (action == "merge") return library_merge(request);
    if (action == "unmerge") return library_unmerge(request);
    if (action == "merges") return library_merges(request);
    const std::string filter = request.query_value("platform", request.body.get_string("platform"));
    json::Value tracks = json::Value::array();
    for (const auto& entry : meta_.list_all_with_paths()) {
        const MetaPath& path = entry.first;
        const BaseInfo& base = entry.second;
        if (!filter.empty() && base.platform != filter) continue;
        json::Value item = json::Value::object();
        item.set("sha", base.sha);
        item.set("name", base.name);
        item.set("author", base.author);
        if (!base.album.empty()) item.set("album", base.album);
        item.set("duration", static_cast<long long>(base.duration_seconds));
        item.set("platform", base.platform);
        item.set("origin", base.is_local() ? "local" : "online");
        if (!base.item_id.empty()) item.set("item_id", base.item_id);
        if (!base.match_platform.empty()) item.set("match_platform", base.match_platform);
        item.set("group_key", base.group_key());
        item.set("secret", static_cast<long long>(base.secret));
        if (!base.pic.empty()) item.set("pic", base.pic);
        item.set("quality", base.quality.to_json());
        item.set("location", base.location);
        item.set("song_key", path.song_key);
        item.set("quality_key", path.quality_key);
        // 自定义合并：标注该条目所属合并组（前端据此显示「已合并」与「取消合并」）
        if (merge_) {
            if (const MergeGroup* group = merge_->find(path.song_key)) {
                item.set("merged", true);
                item.set("merge_song_key", group->song_key);
                item.set("merge_count", static_cast<long long>(group->members.size()));
                item.set("merge_title", group->title);
            }
        }
        if (!base.orig_name.empty()) item.set("orig_name", base.orig_name);
        if (!base.orig_author.empty()) item.set("orig_author", base.orig_author);
        if (base.orig_duration > 0) item.set("orig_duration", static_cast<long long>(base.orig_duration));
        tracks.push_back(item);
    }
    json::Value data = json::Value::object();
    data.set("tracks", tracks);
    data.set("total", static_cast<long long>(tracks.as_array().size()));
    return ApiResponse::success(data);
}

// 从曲库移除：DELETE /music/library/{sha}[?delete_file=true&all=true]
//   delete_file（默认 true）：连同本地音频文件与 tracks/<sha>/ 目录一起删除
//   all：连同同一 song_key 下的其它音质版本一起移除
// 只做「曲库之内」的删除：外部文件（link / 绝对路径导入）一律保留。
ApiResponse ApiDispatcher::library_remove(const ApiRequest& request) {
    std::string sha = request.segment(2);
    if (sha.empty()) sha = request.query_value("sha", request.body.get_string("sha"));
    if (sha.empty()) return ApiResponse::failure(ErrorCode::InvalidMetadata, "缺少 sha");

    // platform + item_id 也可以定位在线曲目
    const std::string platform = request.query_value("platform", request.body.get_string("platform"));
    const std::string item_id = request.query_value("item_id", request.body.get_string("item_id"));
    const bool delete_file = truthy(request.query_value("delete_file", request.body.get_string("delete_file", "true")), true);
    const bool all_variants = truthy(request.query_value("all", request.body.get_string("all", "false")), false);

    std::optional<MetaPath> anchor;
    if (!sha.empty()) anchor = meta_.find_by_sha(sha);
    if (!anchor.has_value() && !platform.empty() && !item_id.empty()) anchor = meta_.find_by_platform_id(platform, item_id);
    if (!anchor.has_value()) {
        return ApiResponse::failure(ErrorCode::FileNotFound, "曲库中没有该曲目" + (sha.empty() ? std::string() : ": " + sha));
    }
    auto anchor_base = meta_.read(*anchor);
    if (!anchor_base.ok()) return ApiResponse::from_status(anchor_base.status());

    // 待移除的条目：单条 or 同一首歌的全部音质版本
    std::vector<std::pair<MetaPath, BaseInfo>> targets;
    if (all_variants) {
        for (const auto& entry : meta_.list_all_with_paths()) {
            if (entry.first.song_key == anchor->song_key) targets.push_back(entry);
        }
    }
    if (targets.empty()) targets.push_back({*anchor, anchor_base.value()});

    std::vector<std::string> target_shas;
    std::vector<std::string> target_keys;   // platform:item_id，用于清理歌单/专辑引用
    for (const auto& entry : targets) {
        target_shas.push_back(entry.second.sha);
        if (!entry.second.item_id.empty()) target_keys.push_back(entry.second.platform + ":" + entry.second.item_id);
    }
    auto is_target_sha = [&](const std::string& candidate) {
        return std::find(target_shas.begin(), target_shas.end(), candidate) != target_shas.end();
    };

    // 正在播放这片曲目：先停后端再删文件（Windows 上被占用的文件删不掉）
    bool stopped = false;
    const std::string playing_sha = engine_.status().sha;
    if (!playing_sha.empty() && is_target_sha(playing_sha)) {
        engine_.stop();
        engine_.reset_session();
        stopped = true;
    }

    size_t deleted_files = 0;
    std::uintmax_t deleted_bytes = 0;
    size_t dirs_removed = 0;
    json::Value skipped = json::Value::array();
    std::vector<std::string> deleted_paths;

    if (delete_file) {
        for (const auto& entry : targets) {
            const BaseInfo& info = entry.second;
            // 1) location 指向的真实音频文件
            if (!info.location.empty() && info.location.rfind("http://", 0) != 0 &&
                info.location.rfind("https://", 0) != 0) {
                std::filesystem::path file(info.location);
                if (file.is_relative()) file = paths_.root / file;
                std::error_code ec;
                if (std::filesystem::is_regular_file(file, ec) && !ec) {
                    if (within_dir(paths_.tracks(), file)) {
                        const auto size = std::filesystem::file_size(file, ec);
                        std::error_code remove_ec;
                        std::filesystem::remove(file, remove_ec);
                        if (!remove_ec) {
                            ++deleted_files;
                            if (!ec) deleted_bytes += size;
                            deleted_paths.push_back(file.u8string());
                        } else {
                            skipped.push_back(info.location);
                        }
                    } else {
                        skipped.push_back(info.location);   // 曲库之外的源文件，不动
                    }
                }
            }
            // 2) 该 sha 的私有目录（音频 / sec.json / 歌词 / 封面）
            const auto dir = paths_.track_dir(info.sha);
            std::error_code ec;
            if (std::filesystem::is_directory(dir, ec) && !ec) {
                if (within_dir(paths_.tracks(), dir)) {
                    const auto size = directory_size(dir);
                    std::error_code remove_ec;
                    std::filesystem::remove_all(dir, remove_ec);
                    if (!remove_ec) {
                        ++dirs_removed;
                        deleted_bytes += size;
                        deleted_paths.push_back(dir.u8string());
                    }
                }
            }
        }
    }

    // 3) 元数据 + 库索引
    size_t removed = 0;
    for (const auto& entry : targets) {
        Status erased = meta_.erase(entry.first);
        if (!erased.ok()) continue;
        index_.remove_by_sha(entry.second.sha);
        ++removed;
    }
    if (removed > 0) index_.save();
    // song_key 空目录顺手清理，避免 cache/meta 下堆积空壳
    std::error_code empty_ec;
    const auto song_dir = paths_.meta() / anchor->song_key;
    if (std::filesystem::is_directory(song_dir, empty_ec) && std::filesystem::is_empty(song_dir, empty_ec)) {
        std::filesystem::remove_all(song_dir, empty_ec);
    }

    // 4) 歌单 / 专辑里的引用（否则会留下播不了的死条目）
    auto clean_refs = [&](auto* store) -> size_t {
        if (!store) return 0;
        size_t count = 0;
        const auto collections = store->list();
        if (!collections.ok()) return 0;
        for (const auto& collection : collections.value()) {
            for (const auto& track : collection.tracks) {
                const bool hit_sha = !track.sha.empty() &&
                    std::find(target_shas.begin(), target_shas.end(), track.sha) != target_shas.end();
                const bool hit_key = !track.item_id.empty() &&
                    std::find(target_keys.begin(), target_keys.end(),
                              track.platform + ":" + track.item_id) != target_keys.end();
                if (!hit_sha && !hit_key) continue;
                if (store->remove_track(collection.id, track.platform, track.item_id).ok()) ++count;
            }
        }
        return count;
    };
    const size_t playlist_refs = clean_refs(playlists_);
    const size_t album_refs = clean_refs(albums_);

    // 5) 下载任务里指向已删文件的「已完成」条目
    size_t task_removed = 0;
    if (downloads_ && !deleted_paths.empty()) {
        for (const auto& task : downloads_->list()) {
            if (task.state != DownloadState::Done || task.path.empty()) continue;
            if (std::find(deleted_paths.begin(), deleted_paths.end(), task.path) == deleted_paths.end()) continue;
            if (downloads_->remove(task.id, false).ok()) ++task_removed;
        }
        if (task_removed > 0) downloads_->save();
    }

    json::Value data = json::Value::object();
    data.set("sha", target_shas.empty() ? std::string() : target_shas.front());
    data.set("removed", static_cast<long long>(removed));
    data.set("deleted_files", static_cast<long long>(deleted_files));
    data.set("deleted_dirs", static_cast<long long>(dirs_removed));
    data.set("deleted_bytes", static_cast<long long>(deleted_bytes));
    data.set("playlist_refs", static_cast<long long>(playlist_refs));
    data.set("album_refs", static_cast<long long>(album_refs));
    data.set("download_tasks", static_cast<long long>(task_removed));
    data.set("stopped", stopped);
    json::Value shas = json::Value::array();
    for (const auto& value : target_shas) shas.push_back(value);
    data.set("shas", shas);
    if (!skipped.as_array().empty()) data.set("skipped", skipped);
    if (removed == 0) {
        return ApiResponse::failure(ErrorCode::RequestFailed, "没有任何曲库条目被移除");
    }
    return ApiResponse::success(data);
}

// ---------------------------------------------------------------- 自定义合并
namespace {

// 请求体里的字符串数组（兼容 sources / source_keys / keys / members 等别名）
std::vector<std::string> merge_key_list(const json::Value& body,
                                        std::initializer_list<std::string_view> names) {
    std::vector<std::string> out;
    for (const auto& name : names) {
        const json::Value* arr = body.find(name);
        if (!arr || !arr->is_array()) continue;
        for (const auto& item : arr->as_array()) {
            const std::string value = item.as_string();
            if (value.empty()) continue;
            if (std::find(out.begin(), out.end(), value) == out.end()) out.push_back(value);
        }
        if (!out.empty()) return out;
    }
    return out;
}

bool contains_key(const std::vector<std::string>& list, const std::string& value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

// 归组后把身份改写成规范值；原始标签只在真正变化时才写入 orig_*（取消合并时用）
bool apply_canonical(BaseInfo& base, const std::string& title, const std::string& artist, int duration) {
    if (base.name == title && base.author == artist && base.duration_seconds == duration) return false;
    if (base.orig_name.empty()) base.orig_name = base.name;
    if (base.orig_author.empty()) base.orig_author = base.author;
    if (base.orig_duration == 0) base.orig_duration = base.duration_seconds;
    base.name = title;
    base.author = artist;
    base.duration_seconds = duration;
    return true;
}

}  // namespace

// POST /music/library/merge
//   body: { target: "<song_key>", sources: ["<song_key>", ...], title?, artist?, duration? }
//   target 提供合并后的规范身份（歌名/歌手/时长），sources 是被合进来的曲目；
//   全部条目搬进同一个 song_key 目录，按 platform+quality 堆叠（同音质多实体用 ~2/~3 变体）。
ApiResponse ApiDispatcher::library_merge(const ApiRequest& request) {
    if (!merge_) return ApiResponse::failure(ErrorCode::RequestFailed, "合并模块未装配");
    if (request.method != "POST") {
        return ApiResponse::failure(ErrorCode::InvalidMetadata, "请使用 POST /music/library/merge");
    }
    const json::Value& body = request.body;
    std::vector<std::string> sources = merge_key_list(body, {"sources", "source_keys", "keys"});
    std::string target = body.get_string("target", body.get_string("song_key"));
    if (target.empty() && !sources.empty()) target = sources.front();
    if (target.empty()) {
        return ApiResponse::failure(ErrorCode::InvalidMetadata, "缺少 target（作为基准的 song_key）");
    }

    // 参与合并的 song_key：target 打头，sources 去重追加
    std::vector<std::string> keys;
    keys.push_back(target);
    for (const auto& key : sources) {
        if (!contains_key(keys, key)) keys.push_back(key);
    }
    if (keys.size() < 2) {
        return ApiResponse::failure(ErrorCode::InvalidMetadata, "请至少选择两条不同的曲目进行合并");
    }

    const auto all = meta_.list_all_with_paths();
    std::map<std::string, std::vector<std::pair<MetaPath, BaseInfo>>> by_song;
    for (const auto& entry : all) by_song[entry.first.song_key].push_back(entry);
    for (const auto& key : keys) {
        if (!by_song.count(key)) {
            return ApiResponse::failure(ErrorCode::FileNotFound, "曲库里没有该曲目: " + key);
        }
    }

    std::string canon_title = body.get_string("title");
    std::string canon_artist = body.get_string("artist");
    int canon_duration = static_cast<int>(body.get_int("duration", 0));
    const BaseInfo& anchor = by_song.find(target)->second.front().second;
    if (canon_title.empty()) canon_title = anchor.name;
    if (canon_artist.empty()) canon_artist = anchor.author;
    if (canon_duration <= 0) canon_duration = anchor.duration_seconds;
    if (canon_title.empty()) {
        return ApiResponse::failure(ErrorCode::InvalidMetadata, "无法确定合并后的标题");
    }
    const std::string new_song_key = radmusic::song_key(canon_title, canon_artist, canon_duration);

    // 合并成同一首歌后共享封面：把组内已有的海报补到缺封面的版本上（不覆盖已有封面）
    std::string canon_pic;
    for (const auto& key : keys) {
        for (const auto& entry : by_song.find(key)->second) {
            if (!entry.second.pic.empty()) {
                canon_pic = entry.second.pic;
                break;
            }
        }
        if (!canon_pic.empty()) break;
    }

    // 目标目录下「不参与本次合并」的既有条目先占位，避免被覆盖
    std::vector<std::string> occupied;
    std::map<std::string, std::string> occupied_sha;
    for (const auto& entry : all) {
        if (entry.first.song_key != new_song_key) continue;
        if (contains_key(keys, entry.first.song_key)) continue;
        occupied.push_back(entry.first.quality_key);
        occupied_sha[entry.first.quality_key] = entry.second.sha;
    }

    MergeGroup group;
    group.song_key = new_song_key;
    group.title = canon_title;
    group.artist = canon_artist;
    group.duration_seconds = canon_duration;
    // 重复合并（该组已存在）：保留原成员记录，避免丢失最早的原始标签
    if (const MergeGroup* existing = merge_->find(new_song_key)) {
        for (const auto& member : existing->members) group.members.push_back(member);
    }

    long long moved_count = 0;
    long long skipped_count = 0;
    json::Value conflicts = json::Value::array();

    for (const auto& key : keys) {
        const auto& entries = by_song.find(key)->second;
        MergeMember member;
        member.song_key = key;
        member.title = entries.front().second.name;
        member.artist = entries.front().second.author;
        member.duration_seconds = entries.front().second.duration_seconds;

        for (const auto& entry : entries) {
            BaseInfo normalized = entry.second;
            apply_canonical(normalized, canon_title, canon_artist, canon_duration);
            if (normalized.pic.empty() && !canon_pic.empty()) normalized.pic = canon_pic;

            const std::string base_quality = radmusic::quality_key(normalized.platform, normalized.quality);
            auto sha_it = occupied_sha.find(base_quality);
            if (sha_it != occupied_sha.end() && sha_it->second == normalized.sha) {
                // 同音质同内容：重复记录，删掉源条目即可
                if (!(entry.first.song_key == new_song_key && entry.first.quality_key == base_quality)) {
                    meta_.erase(entry.first);
                }
                index_.remove_by_sha(normalized.sha);
                index_.upsert(normalized, MetaPath{new_song_key, base_quality});
                ++skipped_count;
                continue;
            }

            std::string quality = base_quality;
            int variant = 1;
            while (contains_key(occupied, quality)) quality = quality_key_variant(base_quality, ++variant);
            occupied.push_back(quality);
            occupied_sha[quality] = normalized.sha;
            if (variant > 1) {
                json::Value conflict = json::Value::object();
                conflict.set("song_key", key);
                conflict.set("quality_key", base_quality);
                conflict.set("variant", quality);
                conflict.set("sha", normalized.sha);
                conflicts.push_back(conflict);
            }

            MetaPath new_path;
            new_path.song_key = new_song_key;
            new_path.quality_key = quality;
            Status written = meta_.write_direct(new_path, normalized);
            if (!written.ok()) return ApiResponse::from_status(written);
            if (!(new_path == entry.first)) meta_.erase(entry.first);
            index_.remove_by_sha(normalized.sha);
            index_.upsert(normalized, new_path);
            member.shas.push_back(normalized.sha);
            ++moved_count;
        }
        group.members.push_back(member);
    }

    Status saved = merge_->add_or_update(group);
    if (!saved.ok()) return ApiResponse::from_status(saved);
    Status index_saved = index_.save();
    if (!index_saved.ok()) return ApiResponse::from_status(index_saved);

    // 旧 song_key 留下的空目录
    std::error_code empty_ec;
    for (const auto& key : keys) {
        if (key == new_song_key) continue;
        const auto song_dir = paths_.meta() / key;
        if (std::filesystem::is_directory(song_dir, empty_ec) &&
            std::filesystem::is_empty(song_dir, empty_ec)) {
            std::filesystem::remove_all(song_dir, empty_ec);
        }
    }

    json::Value data = json::Value::object();
    data.set("song_key", new_song_key);
    data.set("title", canon_title);
    data.set("artist", canon_artist);
    data.set("duration", static_cast<long long>(canon_duration));
    data.set("moved", moved_count);
    data.set("skipped", skipped_count);
    data.set("conflicts", conflicts);
    data.set("group", group.to_json());
    return ApiResponse::success(data);
}

// POST /music/library/unmerge
//   body: { song_key: "<合并组 song_key 或成员 song_key>", members?: ["<song_key>", ...] }
//   members 缺省 = 整组拆散；指定时只把这些成员恢复为独立曲目。
ApiResponse ApiDispatcher::library_unmerge(const ApiRequest& request) {
    if (!merge_) return ApiResponse::failure(ErrorCode::RequestFailed, "合并模块未装配");
    if (request.method != "POST") {
        return ApiResponse::failure(ErrorCode::InvalidMetadata, "请使用 POST /music/library/unmerge");
    }
    const json::Value& body = request.body;
    std::string key = body.get_string("song_key", body.get_string("target"));
    if (key.empty()) key = request.segment(3);
    if (key.empty()) return ApiResponse::failure(ErrorCode::InvalidMetadata, "缺少 song_key");
    const MergeGroup* group = merge_->find(key);
    if (!group) {
        return ApiResponse::failure(ErrorCode::FileNotFound, "这首曲目没有自定义合并记录");
    }
    std::vector<std::string> only = merge_key_list(body, {"members", "member_keys"});
    std::string group_key = group->song_key;
    std::vector<MergeMember> members = group->members;

    // 待还原的 sha（用于判断哪些既有条目不参与本次还原）
    std::vector<std::string> restore_shas;
    for (const auto& member : members) {
        if (!only.empty() && !contains_key(only, member.song_key)) continue;
        for (const auto& sha : member.shas) restore_shas.push_back(sha);
    }
    if (restore_shas.empty()) {
        return ApiResponse::failure(ErrorCode::FileNotFound, "没有可还原的曲目（可能已被删除）");
    }

    const auto all = meta_.list_all_with_paths();
    std::map<std::string, std::pair<MetaPath, BaseInfo>> by_sha;
    for (const auto& entry : all) by_sha[entry.second.sha] = entry;

    long long restored_count = 0;
    std::vector<std::string> restored_members;
    for (const auto& member : members) {
        if (!only.empty() && !contains_key(only, member.song_key)) continue;
        bool any = false;
        for (const auto& sha : member.shas) {
            auto it = by_sha.find(sha);
            if (it == by_sha.end()) continue;
            BaseInfo restored = it->second.second;
            MergeStore::restore(restored, member.title, member.artist, member.duration_seconds);
            const std::string restored_song_key =
                radmusic::song_key(restored.name, restored.author, restored.duration_seconds);
            const std::string base_quality = radmusic::quality_key(restored.platform, restored.quality);

            // 目标 song_key 下「不参与本次还原」的条目先占位
            std::vector<std::string> occupied;
            for (const auto& entry : all) {
                if (entry.first.song_key != restored_song_key) continue;
                if (contains_key(restore_shas, entry.second.sha)) continue;
                occupied.push_back(entry.first.quality_key);
            }
            std::string quality = base_quality;
            int variant = 1;
            while (contains_key(occupied, quality)) quality = quality_key_variant(base_quality, ++variant);

            MetaPath new_path;
            new_path.song_key = restored_song_key;
            new_path.quality_key = quality;
            Status written = meta_.write_direct(new_path, restored);
            if (!written.ok()) return ApiResponse::from_status(written);
            if (!(new_path == it->second.first)) meta_.erase(it->second.first);
            index_.remove_by_sha(restored.sha);
            index_.upsert(restored, new_path);
            ++restored_count;
            any = true;
        }
        if (any) restored_members.push_back(member.song_key);
    }

    // 全部还原 → 整组撤销；只还原一部分 → 从组里摘掉这些成员（剩 1 个时自动整组撤销）
    if (restored_members.size() >= members.size()) {
        merge_->remove(group_key);
    } else {
        for (const auto& member_key : restored_members) merge_->remove_member(group_key, member_key);
    }
    Status index_saved = index_.save();
    if (!index_saved.ok()) return ApiResponse::from_status(index_saved);

    if (restored_count == 0) {
        return ApiResponse::failure(ErrorCode::FileNotFound, "没有可还原的曲目（可能已被删除）");
    }

    json::Value data = json::Value::object();
    data.set("song_key", group_key);
    data.set("restored", restored_count);
    json::Value restored_keys = json::Value::array();
    for (const auto& member_key : restored_members) restored_keys.push_back(member_key);
    data.set("members", restored_keys);
    data.set("group_removed", merge_->find(group_key) == nullptr);
    return ApiResponse::success(data);
}

// GET /music/library/merges：列出全部自定义合并组（供设置/曲库管理查看）
ApiResponse ApiDispatcher::library_merges(const ApiRequest& request) {
    if (!merge_) return ApiResponse::failure(ErrorCode::RequestFailed, "合并模块未装配");
    if (request.method != "GET") {
        return ApiResponse::failure(ErrorCode::InvalidMetadata, "请使用 GET /music/library/merges");
    }
    json::Value groups = json::Value::array();
    for (const auto& group : merge_->groups()) groups.push_back(group.to_json());
    json::Value data = json::Value::object();
    data.set("groups", groups);
    data.set("total", static_cast<long long>(groups.as_array().size()));
    return ApiResponse::success(data);
}

// 上传字节 → 暂存文件 → 立即导入（复用 1.10 的导入流程）
ApiResponse ApiDispatcher::import_upload(std::string_view filename, const std::string& data) {
    if (!importer_) return ApiResponse::failure(ErrorCode::RequestFailed, "导入模块未装配");
    if (data.empty()) return ApiResponse::failure(ErrorCode::InvalidMetadata, "上传内容为空");

    // 只取文件名并做安全化，避免目录穿越与非法字符
    std::string name = std::filesystem::path(std::string(filename)).filename().u8string();
    std::string safe;
    for (char c : name) {
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|' || static_cast<unsigned char>(c) < 0x20) {
            safe.push_back('_');
        } else {
            safe.push_back(c);
        }
    }
    if (safe.empty() || safe == "." || safe == "..") safe = "upload.bin";

    const std::filesystem::path dir = paths_.cache() / "tmp" / "upload";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return ApiResponse::failure(ErrorCode::RequestFailed, "无法创建上传暂存目录");

    const std::filesystem::path target = dir / safe;
    {
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) return ApiResponse::failure(ErrorCode::RequestFailed, "无法写入暂存文件");
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        out.flush();
        if (!out.good()) {
            out.close();
            std::filesystem::remove(target, ec);
            return ApiResponse::failure(ErrorCode::RequestFailed, "暂存文件写入失败");
        }
    }

    json::Value paths = json::Value::array();
    paths.push_back(target.u8string());
    json::Value req = json::Value::object();
    req.set("paths", paths);
    req.set("mode", std::string("copy"));
    req.set("conflict", std::string("skip"));
    req.set("tag_fallback", std::string("filename"));
    req.set("recursive", false);

    auto result = importer_->run(req);
    std::error_code rm_ec;
    std::filesystem::remove(target, rm_ec);   // 内容已进曲库，清理暂存
    if (!result.ok()) return ApiResponse::from_status(result.status());
    return ApiResponse::success(result.value());
}

// sha → 音频文件绝对路径（持久资源 tracks/ 或 link 模式的绝对路径）
Result<std::string> ApiDispatcher::audio_path_for(std::string_view sha) const {
    auto path = meta_.find_by_sha(sha);
    if (!path.has_value()) return Result<std::string>::failure(ErrorCode::FileNotFound, "曲库中没有该 sha");
    auto base = meta_.read(*path);
    if (!base.ok()) return Result<std::string>::failure(base.status().code, base.status().message);

    const std::string& location = base.value().location;
    if (location.rfind("http://", 0) == 0 || location.rfind("https://", 0) == 0) {
        return Result<std::string>::failure(ErrorCode::FileNotFound, "在线曲目请由播放源提供地址");
    }
    std::filesystem::path file(location);
    if (file.is_relative()) file = paths_.root / file;
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec) {
        return Result<std::string>::failure(ErrorCode::FileNotFound, "音频文件不存在: " + location);
    }
    return Result<std::string>::success(file.u8string());
}

// 封面名（covers/<hash>.<ext>）→ 绝对路径；严格白名单，防止目录穿越
Result<std::string> ApiDispatcher::cover_path_for(std::string_view name) const {
    std::string text(name);
    std::replace(text.begin(), text.end(), '\\', '/');
    // 只接受 "covers/<文件名>.<ext>"：不允许子目录、"."、".." 与空段
    const std::string prefix = "covers/";
    if (text.rfind(prefix, 0) != 0) {
        return Result<std::string>::failure(ErrorCode::FileNotFound, "封面路径不合法: " + text);
    }
    const std::string rest = text.substr(prefix.size());
    if (rest.empty() || rest.find('/') != std::string::npos || rest.find("..") != std::string::npos) {
        return Result<std::string>::failure(ErrorCode::FileNotFound, "封面路径不合法: " + text);
    }
    static const char* kExtensions[] = {".jpg", ".jpeg", ".png", ".webp", ".gif", ".bmp"};
    bool ext_ok = false;
    for (const char* ext : kExtensions) {
        const std::string suffix(ext);
        if (rest.size() > suffix.size() &&
            ascii_lower_ascii(rest.substr(rest.size() - suffix.size())) == suffix) {
            ext_ok = true;
            break;
        }
    }
    if (!ext_ok) {
        return Result<std::string>::failure(ErrorCode::FileNotFound, "封面扩展名不支持: " + text);
    }
    const std::filesystem::path file = paths_.root / (prefix + rest);
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec) {
        return Result<std::string>::failure(ErrorCode::FileNotFound, "封面文件不存在: " + text);
    }
    return Result<std::string>::success(file.u8string());
}

ApiResponse ApiDispatcher::h_eq(const ApiRequest& request) {
    if (request.method == "GET" && request.segment(2).empty()) {
        return ApiResponse::success(eq_.to_json());
    }
    if (request.method == "GET" && request.segment(2) == "presets") {
        json::Value presets = json::Value::array();
        const auto dir = paths_.presets() / "eq";
        std::error_code ec;
        if (std::filesystem::exists(dir, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
                if (entry.path().extension() == ".json") {
                    presets.push_back(entry.path().stem().u8string());
                }
            }
        }
        json::Value data = json::Value::object();
        data.set("presets", presets);
        return ApiResponse::success(data);
    }
    // POST：设置参数（1.7 规则 1：实时生效；参数非法 → 1007）
    EqState state = EqState::from_json(request.body);
    for (const auto& band : state.bands) {
        if (band.freq <= 0.0 || band.freq > 96000.0) {
            return ApiResponse::failure(ErrorCode::InvalidMetadata, "频点非法: " + std::to_string(band.freq));
        }
        if (band.gain_db < -24.0 || band.gain_db > 24.0) {
            return ApiResponse::failure(ErrorCode::InvalidMetadata, "增益超出 -24..+24 dB");
        }
    }
    if (state.preamp_db < -24.0 || state.preamp_db > 24.0) {
        return ApiResponse::failure(ErrorCode::InvalidMetadata, "preamp 超出范围");
    }
    Status status = set_eq(state);
    if (!status.ok()) return ApiResponse::from_status(status);
    json::Value data = json::Value::object();
    data.set("applied", true);
    data.set("engine", eq_.engine);
    data.set("latency_ms", static_cast<long long>(eq_.latency_ms));
    data.set("eq", eq_.to_json());
    return ApiResponse::success(data);
}

ApiResponse ApiDispatcher::h_dsp(const ApiRequest& request) {
    const std::string action = request.segment(2);
    if (action == "nodes") {
        // 音效节点来自其他类插件（1.7 / 2.9）；此处只暴露宿主已加载的插件清单信息
        json::Value nodes = json::Value::array();
        for (const auto& manifest : plugins_.manifests()) {
            if (manifest.type != PluginType::Function) continue;
            if (!manifest.has_permission("audio_dsp")) continue;
            json::Value node = json::Value::object();
            node.set("plugin_id", manifest.id);
            node.set("name", manifest.name);
            nodes.push_back(node);
        }
        json::Value data = json::Value::object();
        data.set("nodes", nodes);
        return ApiResponse::success(data);
    }
    if (action == "bypass") {
        const std::string node = request.body.get_string("node", request.query_value("node"));
        if (node.empty()) return ApiResponse::failure(ErrorCode::InvalidMetadata, "缺少 node");
        if (!plugins_.manifest_of(node)) {
            return ApiResponse::failure(ErrorCode::UnsupportedCapability, "未知节点: " + node);
        }
        json::Value data = json::Value::object();
        data.set("node", node);
        data.set("bypassed", request.body.get_bool("bypassed", true));
        return ApiResponse::success(data);
    }
    return ApiResponse::failure(ErrorCode::RequestFailed, "未知 dsp 操作: " + action);
}

// ============================ v0.10：歌单 / 专辑 / MV / 日志 ============================

namespace {

// 由请求体构造一条歌单条目：支持 {track:{...}} 与 {sha}（本地曲目从缓存补全元数据）
PlaylistTrack track_from_request(const json::Value& body, const MetaCache& meta, Status* error) {
    PlaylistTrack track;
    const json::Value* nested = body.find("track");
    const json::Value& source = (nested != nullptr && nested->is_object()) ? *nested : body;

    track.platform = source.get_string("platform", body.get_string("platform"));
    track.item_id = source.get_string("item_id", body.get_string("item_id"));
    track.sha = source.get_string("sha", body.get_string("sha"));
    track.name = source.get_string("name");
    track.author = source.get_string("author");
    track.duration_seconds = static_cast<int>(source.get_int("duration", 0));
    track.group_key = source.get_string("group_key");

    if (track.item_id.empty() && !track.sha.empty()) track.item_id = track.sha;
    if (track.platform.empty() && !track.sha.empty()) track.platform = "local";

    // 用缓存补全缺失字段（名称/作者/时长/折叠键）
    if (!track.sha.empty() && (track.name.empty() || track.author.empty() || track.duration_seconds == 0)) {
        if (auto path = meta.find_by_sha(track.sha); path.has_value()) {
            if (auto base = meta.read(*path); base.ok()) {
                const BaseInfo& info = base.value();
                if (track.name.empty()) track.name = info.name;
                if (track.author.empty()) track.author = info.author;
                if (track.duration_seconds == 0) track.duration_seconds = info.duration_seconds;
                if (track.group_key.empty()) track.group_key = info.group_key();
            }
        }
    }

    if (track.platform.empty() || track.item_id.empty()) {
        if (error) *error = Status(ErrorCode::InvalidMetadata, "缺少 platform 或 item_id（本地曲目可用 sha）");
        return track;
    }
    if (track.name.empty()) {
        if (error) *error = Status(ErrorCode::InvalidMetadata, "缺少 name（或提供可从缓存补全的 sha）");
    }
    return track;
}

}  // namespace

ApiResponse ApiDispatcher::h_playlist(const ApiRequest& request) {
    if (!playlists_) return ApiResponse::failure(ErrorCode::RequestFailed, "歌单模块未装配");
    const std::string id = request.segment(2);
    const std::string action = request.segment(3);

    // 列表
    if (id.empty()) {
        if (request.method == "POST") {
            auto created = playlists_->create(request.body.get_string("name"),
                                              request.body.get_string("description"),
                                              request.body.get_string("platform", "local"),
                                              request.body.get_string("source_id"),
                                              request.body.get_string("pic"));
            if (!created.ok()) return ApiResponse::from_status(created.status());
            RM_LOG_INFO("api", "创建歌单 " + created.value().id);
            return ApiResponse::success(created.value().to_json());
        }
        auto list = playlists_->list();
        if (!list.ok()) return ApiResponse::from_status(list.status());
        json::Value items = json::Value::array();
        for (const auto& playlist : list.value()) items.push_back(playlist.to_json());
        json::Value data = json::Value::object();
        data.set("total", static_cast<long long>(items.size()));
        data.set("playlists", items);
        return ApiResponse::success(data);
    }

    if (request.method == "DELETE") {
        if (!playlists_->remove(id)) return ApiResponse::failure(ErrorCode::PlaylistNotFound, "歌单不存在: " + id);
        json::Value data = json::Value::object();
        data.set("removed", true);
        return ApiResponse::success(data);
    }

    if (action.empty()) {
        if (request.method == "POST") {
            auto renamed = playlists_->rename(id, request.body.get_string("name"),
                                             request.body.get_string("description"));
            if (!renamed.ok()) return ApiResponse::from_status(renamed.status());
            return ApiResponse::success(renamed.value().to_json());
        }
        auto playlist = playlists_->get(id);
        if (!playlist.ok()) return ApiResponse::from_status(playlist.status());
        return ApiResponse::success(playlist.value().to_json());
    }

    if (action == "add") {
        Status error = Status::success();
        const PlaylistTrack track = track_from_request(request.body, meta_, &error);
        if (!error.ok()) return ApiResponse::from_status(error);
        auto updated = playlists_->add_track(id, track);
        if (!updated.ok()) return ApiResponse::from_status(updated.status());
        return ApiResponse::success(updated.value().to_json());
    }
    if (action == "add_many") {
        const json::Value* items = request.body.find("items");
        if (items == nullptr || !items->is_array()) {
            return ApiResponse::failure(ErrorCode::InvalidMetadata, "缺少 items 数组");
        }
        std::vector<PlaylistTrack> tracks;
        for (const auto& item : items->as_array()) {
            Status error = Status::success();
            PlaylistTrack track = track_from_request(item, meta_, &error);
            if (error.ok()) tracks.push_back(std::move(track));
        }
        if (tracks.empty()) {
            return ApiResponse::failure(ErrorCode::InvalidMetadata, "没有可加入歌单的有效曲目");
        }
        auto updated = playlists_->add_tracks(id, tracks);
        if (!updated.ok()) return ApiResponse::from_status(updated.status());
        return ApiResponse::success(updated.value().to_json());
    }
    if (action == "remove") {
        auto updated = playlists_->remove_track(id, request.body.get_string("platform"),
                                               request.body.get_string("item_id"));
        if (!updated.ok()) return ApiResponse::from_status(updated.status());
        return ApiResponse::success(updated.value().to_json());
    }
    if (action == "move") {
        auto updated = playlists_->move_track(id, request.body.get_string("platform"),
                                              request.body.get_string("item_id"),
                                              static_cast<int>(request.body.get_int("index", 0)));
        if (!updated.ok()) return ApiResponse::from_status(updated.status());
        return ApiResponse::success(updated.value().to_json());
    }
    if (action == "sync") {
        auto playlist = playlists_->get(id);
        if (!playlist.ok()) return ApiResponse::from_status(playlist.status());
        const std::string platform = playlist.value().platform;
        Status capable = plugins_.check_capability(platform, "get_playlist");
        if (!capable.ok()) return ApiResponse::from_status(capable);
        auto plugin = plugins_.source_for_platform(platform);
        if (!plugin) return ApiResponse::failure(ErrorCode::PlatformError, "无可用插件: " + platform);

        json::Value params = json::Value::object();
        params.set("id", playlist.value().source_id.empty() ? id : playlist.value().source_id);
        auto result = plugin->request("get_playlist", params);
        if (!result.ok()) return ApiResponse::from_status(result.status());

        // 期望 {tracks:[{platform?, item_id, name, author, duration}]}
        std::vector<PlaylistTrack> tracks;
        const json::Value* list = result.value().is_object() ? result.value().find("tracks") : nullptr;
        if (list != nullptr && list->is_array()) {
            for (const auto& item : list->as_array()) {
                PlaylistTrack track;
                track.platform = item.get_string("platform", platform);
                track.item_id = item.get_string("item_id");
                track.sha = item.get_string("sha");
                track.name = item.get_string("name");
                track.author = item.get_string("author");
                track.duration_seconds = static_cast<int>(item.get_int("duration", 0));
                track.group_key = item.get_string("group_key");
                if (!track.item_id.empty()) tracks.push_back(track);
            }
        }
        auto updated = playlists_->replace_tracks(id, tracks);
        if (!updated.ok()) return ApiResponse::from_status(updated.status());
        return ApiResponse::success(updated.value().to_json());
    }
    return ApiResponse::failure(ErrorCode::RequestFailed, "未知歌单操作: " + action);
}

ApiResponse ApiDispatcher::h_album(const ApiRequest& request) {
    if (!albums_) return ApiResponse::failure(ErrorCode::RequestFailed, "专辑模块未装配");
    const std::string id = request.segment(2);
    const std::string action = request.segment(3);

    if (id.empty()) {
        if (request.method == "POST") {
            auto created = albums_->create(request.body.get_string("name"), request.body.get_string("artist"),
                                           request.body.get_string("platform", "local"),
                                           request.body.get_string("source_id"), request.body.get_string("pic"),
                                           static_cast<int>(request.body.get_int("year", 0)));
            if (!created.ok()) return ApiResponse::from_status(created.status());
            return ApiResponse::success(created.value().to_json());
        }
        auto list = albums_->list();
        if (!list.ok()) return ApiResponse::from_status(list.status());
        json::Value items = json::Value::array();
        for (const auto& album : list.value()) items.push_back(album.to_json());
        json::Value data = json::Value::object();
        data.set("total", static_cast<long long>(items.size()));
        data.set("albums", items);
        return ApiResponse::success(data);
    }

    if (request.method == "DELETE") {
        if (!albums_->remove(id)) return ApiResponse::failure(ErrorCode::AlbumNotFound, "专辑不存在: " + id);
        json::Value data = json::Value::object();
        data.set("removed", true);
        return ApiResponse::success(data);
    }

    if (action.empty()) {
        if (request.method == "POST") {
            auto updated = albums_->update(id, request.body.get_string("name"),
                                           request.body.get_string("artist"),
                                           request.body.get_string("pic"),
                                           request.body.get_string("description"),
                                           static_cast<int>(request.body.get_int("year", 0)));
            if (!updated.ok()) return ApiResponse::from_status(updated.status());
            return ApiResponse::success(updated.value().to_json());
        }
        auto album = albums_->get(id);
        if (!album.ok()) return ApiResponse::from_status(album.status());
        return ApiResponse::success(album.value().to_json());
    }
    if (action == "add") {
        Status error = Status::success();
        const PlaylistTrack track = track_from_request(request.body, meta_, &error);
        if (!error.ok()) return ApiResponse::from_status(error);
        auto updated = albums_->add_track(id, track);
        if (!updated.ok()) return ApiResponse::from_status(updated.status());
        return ApiResponse::success(updated.value().to_json());
    }
    if (action == "add_many") {
        const json::Value* items = request.body.find("items");
        if (items == nullptr || !items->is_array()) {
            return ApiResponse::failure(ErrorCode::InvalidMetadata, "缺少 items 数组");
        }
        std::vector<PlaylistTrack> tracks;
        for (const auto& item : items->as_array()) {
            Status error = Status::success();
            PlaylistTrack track = track_from_request(item, meta_, &error);
            if (error.ok()) tracks.push_back(std::move(track));
        }
        if (tracks.empty()) {
            return ApiResponse::failure(ErrorCode::InvalidMetadata, "没有可加入专辑的有效曲目");
        }
        auto updated = albums_->add_tracks(id, tracks);
        if (!updated.ok()) return ApiResponse::from_status(updated.status());
        return ApiResponse::success(updated.value().to_json());
    }
    if (action == "remove") {
        auto updated = albums_->remove_track(id, request.body.get_string("platform"),
                                            request.body.get_string("item_id"));
        if (!updated.ok()) return ApiResponse::from_status(updated.status());
        return ApiResponse::success(updated.value().to_json());
    }
    if (action == "sync") {
        auto album = albums_->get(id);
        if (!album.ok()) return ApiResponse::from_status(album.status());
        const std::string platform = album.value().platform;
        Status capable = plugins_.check_capability(platform, "get_album");
        if (!capable.ok()) return ApiResponse::from_status(capable);
        auto plugin = plugins_.source_for_platform(platform);
        if (!plugin) return ApiResponse::failure(ErrorCode::PlatformError, "无可用插件: " + platform);

        json::Value params = json::Value::object();
        params.set("id", album.value().source_id.empty() ? id : album.value().source_id);
        auto result = plugin->request("get_album", params);
        if (!result.ok()) return ApiResponse::from_status(result.status());

        std::vector<PlaylistTrack> tracks;
        const json::Value* list = result.value().is_object() ? result.value().find("tracks") : nullptr;
        if (list != nullptr && list->is_array()) {
            for (const auto& item : list->as_array()) {
                PlaylistTrack track;
                track.platform = item.get_string("platform", platform);
                track.item_id = item.get_string("item_id");
                track.sha = item.get_string("sha");
                track.name = item.get_string("name");
                track.author = item.get_string("author", album.value().artist);
                track.duration_seconds = static_cast<int>(item.get_int("duration", 0));
                if (!track.item_id.empty()) tracks.push_back(track);
            }
        }
        auto updated = albums_->replace_tracks(id, tracks);
        if (!updated.ok()) return ApiResponse::from_status(updated.status());
        return ApiResponse::success(updated.value().to_json());
    }
    return ApiResponse::failure(ErrorCode::RequestFailed, "未知专辑操作: " + action);
}

ApiResponse ApiDispatcher::h_mv(const ApiRequest& request) {
    if (!mvs_) return ApiResponse::failure(ErrorCode::RequestFailed, "MV 模块未装配");
    const std::string id = request.segment(2);
    const std::string action = request.segment(3);

    // GET /music/mv?platform=&id= ：按来源查找
    const std::string query_platform = request.query_value("platform");
    const std::string query_id = request.query_value("id");
    if (action == "search" || (id == "search")) {
        // POST /music/mv/search {keyword, source?, page?, page_size?}
        // 用设置页选定的视频源（或第一个声明 search_mv/get_mv 能力的源）搜索 MV
        std::string source = request.body.get_string("source", request.query_value("source"));
        if (source.empty() && settings_) source = settings_->values().mv_source;
        if (source.empty()) {
            for (const auto& manifest : plugins_.manifests()) {
                if (!plugins_.is_enabled(manifest.id)) continue;
                if (!plugins_.has_implementation(manifest.id)) continue;
                if (manifest.has_capability("search_mv") || manifest.has_capability("get_mv")) {
                    if (!manifest.platforms.empty()) {
                        source = manifest.platforms.front();
                        break;
                    }
                }
            }
        }
        const std::string keyword = request.body.get_string("keyword", request.query_value("keyword"));
        if (keyword.empty()) return ApiResponse::failure(ErrorCode::InvalidMetadata, "缺少 keyword");
        if (source.empty()) {
            return ApiResponse::failure(ErrorCode::UnsupportedCapability, "没有可用于搜索 MV 的视频源");
        }
        Status capable = plugins_.check_capability(source, "search_mv");
        if (!capable.ok()) {
            // 兼容只声明 get_mv 的源：退化为 get_mv 查询
            capable = plugins_.check_capability(source, "get_mv");
            if (!capable.ok()) {
                return ApiResponse::failure(ErrorCode::UnsupportedCapability,
                    "视频源 " + source + " 未声明 search_mv/get_mv 能力，请在设置中更换视频源");
            }
        }
        auto plugin = plugins_.source_for_platform(source);
        if (!plugin) {
            return ApiResponse::failure(ErrorCode::PlatformError,
                "视频源 " + source + " 没有可用的插件实现，请在设置中更换视频源");
        }
        json::Value params = json::Value::object();
        params.set("keyword", keyword);
        params.set("page", static_cast<long long>(request.body.get_int("page", 1)));
        params.set("page_size", static_cast<long long>(request.body.get_int("page_size", 20)));
        const std::string op = plugins_.check_capability(source, "search_mv").ok() ? "search_mv" : "get_mv";
        auto result = plugin->request(op, params);
        if (!result.ok()) return ApiResponse::from_status(result.status());
        json::Value data = json::Value::object();
        data.set("source", source);
        data.set("op", op);
        data.set("keyword", keyword);
        data.set("result", result.value());
        return ApiResponse::success(data);
    }

    if (id.empty() && !query_platform.empty() && !query_id.empty()) {
        auto found = mvs_->find_by_source(query_platform, query_id);
        if (!found.ok()) return ApiResponse::from_status(found.status());
        return ApiResponse::success(found.value().to_json());
    }

    if (id.empty()) {
        if (request.method == "POST") {
            MvInfo info = MvInfo::from_json(request.body);
            if (info.sha.empty()) return ApiResponse::failure(ErrorCode::InvalidMetadata, "缺少 sha");
            auto stored = mvs_->put(info);
            if (!stored.ok()) return ApiResponse::from_status(stored.status());
            return ApiResponse::success(stored.value().to_json());
        }
        auto list = mvs_->list();
        if (!list.ok()) return ApiResponse::from_status(list.status());
        json::Value items = json::Value::array();
        for (const auto& mv : list.value()) items.push_back(mv.to_json());
        json::Value data = json::Value::object();
        data.set("total", static_cast<long long>(items.size()));
        data.set("mvs", items);
        return ApiResponse::success(data);
    }

    if (request.method == "DELETE") {
        if (!mvs_->remove(id)) return ApiResponse::failure(ErrorCode::MvNotFound, "MV 不存在: " + id);
        json::Value data = json::Value::object();
        data.set("removed", true);
        return ApiResponse::success(data);
    }

    if (action == "open") {
        auto mv = mvs_->get(id);
        if (!mv.ok()) return ApiResponse::from_status(mv.status());
        const std::string platform = mv.value().platform;
        Status capable = plugins_.check_capability(platform, "get_mv");
        if (!capable.ok()) return ApiResponse::from_status(capable);
        auto plugin = plugins_.source_for_platform(platform);
        if (!plugin) return ApiResponse::failure(ErrorCode::PlatformError, "无可用插件: " + platform);

        json::Value params = json::Value::object();
        params.set("id", mv.value().item_id.empty() ? id : mv.value().item_id);
        auto result = plugin->request("get_mv", params);
        if (!result.ok()) return ApiResponse::from_status(result.status());

        MvInfo updated = mv.value();
        if (result.value().is_object()) {
            const std::string url = result.value().get_string("url", result.value().get_string("location"));
            if (!url.empty()) updated.location = url;
            const long long duration = result.value().get_int("duration", 0);
            if (duration > 0) updated.duration_seconds = static_cast<int>(duration);
        } else if (result.value().is_string()) {
            updated.location = result.value().as_string();
        }
        auto stored = mvs_->put(updated);
        if (!stored.ok()) return ApiResponse::from_status(stored.status());
        return ApiResponse::success(stored.value().to_json());
    }

    auto mv = mvs_->get(id);
    if (!mv.ok()) return ApiResponse::from_status(mv.status());
    return ApiResponse::success(mv.value().to_json());
}

ApiResponse ApiDispatcher::h_logs(const ApiRequest& request) {
    const std::string action = request.segment(2);
    if (action == "level") {
        const std::string level_id = request.body.get_string("level", request.query_value("level"));
        if (level_id.empty()) return ApiResponse::failure(ErrorCode::InvalidMetadata, "缺少 level");
        const bool known = level_id == "trace" || level_id == "debug" || level_id == "info" ||
                           level_id == "warn" || level_id == "warning" || level_id == "error" ||
                           level_id == "off";
        if (!known) return ApiResponse::failure(ErrorCode::InvalidMetadata, "未知日志级别: " + level_id);
        Logger::instance().set_level(log_level_from_id(level_id));
        json::Value data = json::Value::object();
        data.set("level", std::string(log_level_id(Logger::instance().level())));
        RM_LOG_INFO("api", "日志级别调整为 " + level_id);
        return ApiResponse::success(data);
    }

    int lines = static_cast<int>(request.body.get_int("lines", 200));
    const std::string lines_query = request.query_value("lines");
    if (!lines_query.empty()) lines = std::atoi(lines_query.c_str());
    if (lines <= 0) lines = 200;
    if (lines > 500) lines = 500;
    const std::string level_filter = request.query_value("level", request.body.get_string("level"));
    const std::string category_filter = request.query_value("category", request.body.get_string("category"));

    auto sink = log_memory_sink();
    json::Value records = json::Value::array();
    int matched = 0;
    if (sink) {
        const auto& all = sink->records();
        for (const auto& record : all) {
            if (!level_filter.empty() && std::string(log_level_id(record.level)) != level_filter) continue;
            if (!category_filter.empty() && record.category != category_filter) continue;
            ++matched;
        }
        int skip = matched > lines ? matched - lines : 0;
        for (const auto& record : all) {
            if (!level_filter.empty() && std::string(log_level_id(record.level)) != level_filter) continue;
            if (!category_filter.empty() && record.category != category_filter) continue;
            if (skip > 0) {
                --skip;
                continue;
            }
            records.push_back(record.to_json());
        }
    }
    json::Value data = json::Value::object();
    data.set("level", std::string(log_level_id(Logger::instance().level())));
    data.set("total", static_cast<long long>(matched));
    data.set("records", records);
    return ApiResponse::success(data);
}

// ---------------- 设置页：插件管理 / 宿主设置 ----------------

namespace {

bool safe_id(std::string_view id) {
    if (id.empty() || id.size() > 96) return false;
    for (char ch : id) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                        ch == '.' || ch == '-' || ch == '_';
        if (!ok) return false;
    }
    return id != "." && id != "..";
}

json::Value plugin_entry_json(const PluginManifest& manifest, bool enabled, bool has_impl, bool crashed) {
    json::Value v = manifest.to_json();
    v.set("id", manifest.id);
    v.set("enabled", enabled);
    v.set("has_implementation", has_impl);
    v.set("crashed", crashed);
    return v;
}

}  // namespace

ApiResponse ApiDispatcher::h_plugins(const ApiRequest& request) {
    const std::string id = request.segment(2);
    const std::string action = request.segment(3);

    if (!id.empty() && action == "enabled" && request.method == "POST") {
        const json::Value* enabled_value = request.body.find("enabled");
        if (!enabled_value || !enabled_value->is_bool()) {
            return ApiResponse::failure(ErrorCode::InvalidMetadata, "缺少 enabled 布尔字段");
        }
        const bool enabled = enabled_value->as_bool();
        if (!plugins_.manifest_of(id)) {
            return ApiResponse::failure(ErrorCode::RequestFailed, "插件不存在: " + id);
        }
        plugins_.set_enabled(id, enabled);
        if (settings_) {
            settings_->set_plugin_enabled(id, enabled);
            auto saved = settings_->save();
            if (!saved.ok()) return ApiResponse::from_status(saved);
        }
        json::Value data = json::Value::object();
        data.set("id", id);
        data.set("enabled", enabled);
        return ApiResponse::success(data);
    }

    if (!id.empty() && action == "settings") {
        // GET 读 / POST 写插件级配置
        return h_plugin_settings(request);
    }

    if (request.method == "POST" && id.empty()) {
        json::Value manifest_json = request.body;
        if (const json::Value* nested = request.body.find("manifest"); nested && nested->is_object()) {
            manifest_json = *nested;
        }
        if (const json::Value* raw = request.body.find("plugin_json"); raw && raw->is_string()) {
            manifest_json = json::Value::parse(raw->as_string());
            if (manifest_json.is_null()) {
                return ApiResponse::failure(ErrorCode::InvalidMetadata, "plugin_json 不是合法 JSON");
            }
        }
        if (!manifest_json.is_object()) {
            return ApiResponse::failure(ErrorCode::InvalidMetadata, "缺少插件清单对象");
        }
        PluginManifest manifest = PluginManifest::from_json(manifest_json);
        auto validated = manifest.validate();
        if (!validated.ok()) return ApiResponse::from_status(validated);
        if (!safe_id(manifest.id)) {
            return ApiResponse::failure(ErrorCode::InvalidMetadata, "插件 id 只能包含字母/数字/._-: " + manifest.id);
        }
        auto added = plugins_.add_manifest(manifest);
        if (!added.ok()) return ApiResponse::from_status(added);

        const std::filesystem::path dir = paths_.plugins() / manifest.id;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        auto written = write_text_file_atomic(dir, dir / "plugin.json", manifest.to_json().dump(2));
        if (!written.ok()) return ApiResponse::from_status(written);

        if (settings_) {
            settings_->set_plugin_enabled(manifest.id, true);
            auto saved = settings_->save();
            if (!saved.ok()) return ApiResponse::from_status(saved);
        }
        json::Value data = json::Value::object();
        data.set("plugin", plugin_entry_json(manifest, true, plugins_.has_implementation(manifest.id),
                                             plugins_.is_crashed(manifest.id)));
        RM_LOG_INFO("api", "注册插件 " + manifest.id);
        return ApiResponse::success(data);
    }

    if (request.method == "DELETE" && !id.empty()) {
        if (plugins_.has_implementation(id)) {
            return ApiResponse::failure(ErrorCode::UnsupportedCapability,
                                        "内置插件不能删除，可在设置中禁用: " + id);
        }
        if (!plugins_.remove_manifest(id)) {
            return ApiResponse::failure(ErrorCode::RequestFailed, "插件不存在: " + id);
        }
        if (settings_) {
            settings_->forget_plugin(id);
            auto saved = settings_->save();
            if (!saved.ok()) return ApiResponse::from_status(saved);
        }
        if (safe_id(id)) {
            std::error_code ec;
            std::filesystem::remove_all(paths_.plugins() / id, ec);
        }
        json::Value data = json::Value::object();
        data.set("removed", true);
        data.set("id", id);
        return ApiResponse::success(data);
    }

    if (!id.empty()) {
        const PluginManifest* manifest = plugins_.manifest_of(id);
        if (!manifest) return ApiResponse::failure(ErrorCode::RequestFailed, "插件不存在: " + id);
        return ApiResponse::success(plugin_entry_json(*manifest, plugins_.is_enabled(id),
                                                      plugins_.has_implementation(id),
                                                      plugins_.is_crashed(id)));
    }

    json::Value list = json::Value::array();
    for (const auto& manifest : plugins_.manifests()) {
        list.push_back(plugin_entry_json(manifest, plugins_.is_enabled(manifest.id),
                                         plugins_.has_implementation(manifest.id),
                                         plugins_.is_crashed(manifest.id)));
    }
    json::Value data = json::Value::object();
    data.set("total", static_cast<long long>(list.size()));
    data.set("plugins", list);
    data.set("source_priority", [&] {
        json::Value priority = json::Value::array();
        for (const auto& item : source_priority_) priority.push_back(item);
        return priority;
    }());
    return ApiResponse::success(data);
}

// GET /music/plugins/{id}/settings
//   统一返回：登录态 + 插件 settings_schema + 当前值 + 版本号。
//   播放源插件：transport=http 的直接转给插件 /request op=settings；
//               内置 / 声明式插件则只返回 settings_schema，前端按 schema 渲染，
//               并通过 POST /music/plugins/{id}/settings 落到宿主 settings.json。
// POST /music/plugins/{id}/settings  {values: {...}}
//   把用户在面板里改过的值落到 <root>/settings.json 的 plugin_settings[id]。
ApiResponse ApiDispatcher::h_plugin_settings(const ApiRequest& request) {
    const std::string id = request.segment(2);
    if (id.empty()) return ApiResponse::failure(ErrorCode::InvalidMetadata, "缺少 plugin id");
    const PluginManifest* manifest = plugins_.manifest_of(id);
    if (!manifest) return ApiResponse::failure(ErrorCode::RequestFailed, "插件不存在: " + id);

    if (request.method == "POST") {
        if (!settings_) return ApiResponse::failure(ErrorCode::RequestFailed, "宿主设置未装配");
        const json::Value* patch = request.body.find("values");
        if (patch != nullptr && patch->is_object()) {
            settings_->set_plugin_setting(id, *patch);
        } else {
            settings_->set_plugin_setting(id, request.body);
        }
        auto saved = settings_->save();
        if (!saved.ok()) return ApiResponse::from_status(saved);
        json::Value data = json::Value::object();
        data.set("saved", true);
        data.set("plugin_id", id);
        data.set("values", settings_->values().plugin_settings.find(id) != nullptr
                              ? *settings_->values().plugin_settings.find(id)
                              : json::Value::object());
        return ApiResponse::success(data);
    }

    json::Value data = json::Value::object();
    data.set("plugin_id", id);
    data.set("name", manifest->name);
    data.set("type", std::string(plugin_type_id(manifest->type)));
    data.set("version", manifest->version);
    data.set("enabled", plugins_.is_enabled(id));
    data.set("has_implementation", plugins_.has_implementation(id));
    data.set("settings_schema", manifest->settings_schema.present() ? manifest->settings_schema.to_json() : json::Value());
    data.set("endpoint", manifest->endpoint);
    json::Value platforms_arr = json::Value::array();
    for (const auto& p : manifest->platforms) platforms_arr.push_back(p);
    data.set("platforms", platforms_arr);
    json::Value caps = json::Value::array();
    for (const auto& c : manifest->capabilities) caps.push_back(c);
    data.set("capabilities", caps);

    // 当前值：内置插件暂未抽象 settings，外部插件需要时由插件自报
    json::Value values = json::Value::object();

    // 已注册但尚未就绪（没有实现、endpoint 不通）的插件：返回空 values，前端显示「未就绪」
    const bool can_proxy = manifest->transport == "http" && !manifest->endpoint.empty() &&
                           plugins_.has_implementation(id) && plugins_.is_enabled(id);
    if (can_proxy) {
        // 外部插件：转发 settings op
        auto plugin = plugins_.source_for_platform(manifest->platforms.empty() ? id : manifest->platforms.front());
        if (plugin == nullptr) {
            return ApiResponse::failure(ErrorCode::PluginDisabled, "插件未就绪: " + id);
        }
        json::Value params = json::Value::object();
        // 支持 ?refresh=1 强制刷新登录态
        if (request.query_value("refresh") == "1") params.set("refresh", true);
        auto result = plugin->request("settings", params);
        if (result.ok()) {
            // 插件返回的 data 是个对象，混入宿主的元信息
            const json::Value& remote = result.value();
            if (remote.is_object()) {
                if (const json::Value* v = remote.find("login"); v && !v->is_null()) data.set("login", *v);
                if (const json::Value* v = remote.find("cookie_file"); v && v->is_string()) data.set("cookie_file", *v);
                if (const json::Value* v = remote.find("config_file"); v && v->is_string()) data.set("config_file", *v);
                if (const json::Value* v = remote.find("values"); v && v->is_object()) values = *v;
                if (const json::Value* v = remote.find("login_page"); v && v->is_string()) data.set("login_page", *v);
                if (const json::Value* v = remote.find("settings_page"); v && v->is_string()) data.set("settings_page", *v);
                if (const json::Value* v = remote.find("schema"); v && v->is_array()) {
                    json::Value schema_obj = json::Value::object();
                    if (!manifest->settings_schema.endpoint.empty()) schema_obj.set("endpoint", manifest->settings_schema.endpoint);
                    schema_obj.set("schema", *v);
                    data.set("settings_schema", schema_obj);
                }
                // 插件报告的 version 覆盖 manifest 的（运行时的版本更准）
                if (const json::Value* v = remote.find("version"); v && v->is_string()) data.set("version", *v);
            }
        } else {
            // 1004/1013 等错误时仍返回 schema（让用户能看到入口），但 values 留空
            data.set("warning", std::string("插件未响应设置查询：") + result.status().message);
        }
    } else {
        data.set("warning", plugins_.is_enabled(id)
            ? std::string("该插件未提供设置项")
            : std::string("该插件已禁用，启用后可见设置"));
    }

    // 持久化到宿主 settings.json 的 plugin_settings[id]（便于内置插件 / 声明式插件也能存值）
    if (settings_) {
        const AppSettings& all = settings_->values();
        const json::Value* stored = all.plugin_settings.find(id);
        if (stored != nullptr && stored->is_object()) {
            // 合并：插件报告的 values 优先（运行时），否则用宿主持久化的值
            json::Value merged = *stored;
            if (values.is_object()) {
                for (const auto& kv : values.as_object()) {
                    merged.set(kv.first, kv.second);
                }
            }
            data.set("values", merged);
        } else {
            data.set("values", values);
        }
    } else {
        data.set("values", values);
    }
    return ApiResponse::success(data);
}

// POST /music/restart —— 让宿主退出当前进程；调用方按宿主自己的方式（守护进程 / 服务 / 手动）
// 重启它。回调由宿主注入；未注入时返回 1011 提示「设置面重启宿主」需宿主侧接好。
ApiResponse ApiDispatcher::h_restart(const ApiRequest& request) {
    if (request.method != "POST") {
        return ApiResponse::failure(ErrorCode::RequestFailed, "restart 仅支持 POST");
    }
    const std::string confirm = request.body.get_string("confirm");
    if (confirm != "RESTART") {
        return ApiResponse::failure(ErrorCode::RequestFailed, "确认短语不匹配，未执行重启");
    }
    if (!restart_callback_) {
        return ApiResponse::failure(ErrorCode::RequestFailed,
                                    "未注入重启回调：请通过 schtasks / 守护进程 / 手动重新启动宿主");
    }
    Status status = restart_callback_(std::string());  // 空 = 沿用当前数据根
    if (!status.ok()) return ApiResponse::from_status(status);
    json::Value data = json::Value::object();
    data.set("restarting", true);
    return ApiResponse::success(data);
}

// ---------------------------------------------------------------- 检查更新
// GET  /music/update              读取状态（idle / checking / available / downloading / downloaded / failed）
//      附带频道信息（channel / build_channel / channels）与该频道的发布历史（releases）
// POST /music/update/check        触发检查（异步：http 请求在 idle hook 里执行，不阻塞其他接口）
//                                 body 可带 channel（release / dev / canary）切换频道，会写回设置
// POST /music/update/download     下载本平台安装包（分片推进）
// POST /music/update/apply        解压 + 覆盖 + 重启（需 confirm=UPDATE）
ApiResponse ApiDispatcher::h_update(const ApiRequest& request) {
    if (update_ == nullptr) {
        return ApiResponse::failure(ErrorCode::RequestFailed, "未装配更新服务（宿主未注入 UpdateService）");
    }
    const std::string action = request.segment(2);
    if (action.empty() || action == "status") {
        // 线程数与频道都跟随设置页：任何一次状态查询都同步（下载中改设置也不会打乱当前分段）
        if (settings_ != nullptr) {
            update_->set_threads(settings_->values().update_threads);
            update_->set_channel(settings_->values().update_channel);
        }
        return ApiResponse::success(update_->status());
    }

    if (request.method != "POST") {
        return ApiResponse::failure(ErrorCode::RequestFailed, "update/" + action + " 仅支持 POST");
    }

    Status status = Status::success();
    if (action == "check") {
        // 请求体里的 channel 优先（设置页切换频道会同时落盘，保证重启后仍是同一频道）
        const std::string channel = request.body.get_string("channel");
        if (!channel.empty()) {
            if (update_->busy()) {
                return ApiResponse::failure(ErrorCode::RequestFailed, "更新任务正在执行，请稍候再切换频道");
            }
            update_->set_channel(channel);
            if (settings_ != nullptr && settings_->values().update_channel != update_->channel()) {
                settings_->values().update_channel = update_->channel();
                settings_->save();
            }
        }
        status = update_->begin_check(request.body.get_bool("allow_downgrade", false));
    } else if (action == "download") {
        // 线程数：请求体优先，其次读宿主设置（设置页「下载线程数」），默认 4
        int threads = static_cast<int>(request.body.get_int("threads", 0));
        if (threads <= 0 && settings_ != nullptr) threads = settings_->values().update_threads;
        status = update_->begin_download(threads);
    } else if (action == "apply") {
        const std::string confirm = request.body.get_string("confirm");
        if (confirm != "UPDATE") {
            return ApiResponse::failure(ErrorCode::RequestFailed, "确认短语不匹配，未执行更新");
        }
        status = update_->apply();
    } else {
        return ApiResponse::failure(ErrorCode::RequestFailed, "未知的更新操作: " + action);
    }
    if (!status.ok()) return ApiResponse::from_status(status);
    return ApiResponse::success(update_->status());
}

// POST /music/playlist/search  {keyword, source, page, page_size}
//   同步 MV 搜索接口的形态：用设置页「视频源」选定的源（或第一个声明 search_playlist 的源）
//   搜索歌单。返回与 netease search_playlist 一致的 {items:[{id,name,creator,pic,track_count,play_count}]}。
ApiResponse ApiDispatcher::h_playlist_search(const ApiRequest& request) {
    const std::string keyword = request.body.get_string("keyword", request.query_value("keyword"));
    if (keyword.empty()) return ApiResponse::failure(ErrorCode::InvalidMetadata, "缺少 keyword");
    std::string source = request.body.get_string("source", request.query_value("source"));
    if (source.empty()) {
        // 默认走第一个声明了 search_playlist 的插件平台
        for (const auto& manifest : plugins_.manifests()) {
            if (!plugins_.is_enabled(manifest.id)) continue;
            if (!plugins_.has_implementation(manifest.id)) continue;
            if (!manifest.has_capability("search_playlist")) continue;
            if (!manifest.platforms.empty()) {
                source = manifest.platforms.front();
                break;
            }
        }
    }
    if (source.empty()) {
        return ApiResponse::failure(ErrorCode::UnsupportedCapability,
            "没有声明 search_playlist 的播放源；可在设置中启用新插件或扩展现有插件");
    }
    Status capable = plugins_.check_capability(source, "search_playlist");
    if (!capable.ok()) return ApiResponse::from_status(capable);
    auto plugin = plugins_.source_for_platform(source);
    if (!plugin) return ApiResponse::failure(ErrorCode::PlatformError, "无可用插件: " + source);

    json::Value params = json::Value::object();
    params.set("keyword", keyword);
    params.set("page", static_cast<long long>(request.body.get_int("page", 1)));
    params.set("page_size", static_cast<long long>(request.body.get_int("page_size", 20)));
    auto result = plugin->request("search_playlist", params);
    if (!result.ok()) return ApiResponse::from_status(result.status());

    json::Value data = json::Value::object();
    data.set("source", source);
    data.set("keyword", keyword);
    data.set("result", result.value());
    // 兼容前端：把 items 平铺到顶层
    if (result.value().is_object() && result.value().find("items")) {
        data.set("items", result.value().get("items"));
    } else if (result.value().is_array()) {
        data.set("items", result.value());
    } else {
        json::Value empty = json::Value::array();
        data.set("items", empty);
    }
    return ApiResponse::success(data);
}

ApiResponse ApiDispatcher::h_album_search(const ApiRequest& request) {
    const std::string keyword = request.body.get_string("keyword", request.query_value("keyword"));
    if (keyword.empty()) return ApiResponse::failure(ErrorCode::InvalidMetadata, "缺少 keyword");
    std::string source = request.body.get_string("source", request.query_value("source"));
    if (source.empty()) {
        for (const auto& manifest : plugins_.manifests()) {
            if (!plugins_.is_enabled(manifest.id)) continue;
            if (!plugins_.has_implementation(manifest.id)) continue;
            if (!manifest.has_capability("search_album")) continue;
            if (!manifest.platforms.empty()) {
                source = manifest.platforms.front();
                break;
            }
        }
    }
    if (source.empty()) {
        return ApiResponse::failure(ErrorCode::UnsupportedCapability,
            "没有声明 search_album 的播放源；可在设置中启用新插件或扩展现有插件");
    }
    Status capable = plugins_.check_capability(source, "search_album");
    if (!capable.ok()) return ApiResponse::from_status(capable);
    auto plugin = plugins_.source_for_platform(source);
    if (!plugin) return ApiResponse::failure(ErrorCode::PlatformError, "无可用插件: " + source);

    json::Value params = json::Value::object();
    params.set("keyword", keyword);
    params.set("page", static_cast<long long>(request.body.get_int("page", 1)));
    params.set("page_size", static_cast<long long>(request.body.get_int("page_size", 20)));
    auto result = plugin->request("search_album", params);
    if (!result.ok()) return ApiResponse::from_status(result.status());

    json::Value data = json::Value::object();
    data.set("source", source);
    data.set("keyword", keyword);
    data.set("result", result.value());
    if (result.value().is_object() && result.value().find("items")) {
        data.set("items", result.value().get("items"));
    } else if (result.value().is_array()) {
        data.set("items", result.value());
    } else {
        json::Value empty = json::Value::array();
        data.set("items", empty);
    }
    return ApiResponse::success(data);
}

// ---------------- 设置页：数据管理（占用统计 / 清除全部数据） ----------------

namespace {

struct DataUsage {
    std::uintmax_t bytes = 0;
    std::size_t files = 0;
    bool present = false;
};

struct DataGroupSpec {
    std::string id;
    std::string name;
    std::string description;
    std::vector<std::filesystem::path> paths;
};

struct PurgeReport {
    std::size_t files = 0;
    std::uintmax_t bytes = 0;
    std::size_t failed = 0;
};

// 可清理的数据分类：id 由前端提交，paths 全部位于 <root> 之内
// 注意：曲目与元数据放在同一类，避免「删了音频却留下无法播放的库条目」
std::vector<DataGroupSpec> data_group_specs(const LibraryPaths& paths) {
    std::vector<DataGroupSpec> groups;
    groups.push_back({"tracks", "曲目与元数据",
                      "曲库音频及其 base.json 元数据（tracks/、cache/meta）",
                      {paths.tracks(), paths.meta()}});
    groups.push_back({"cache", "缓存文件",
                      "音频 / 流 / 歌词 / 封面缓存与下载任务（cache/ 其余部分）",
                      {paths.audio_cache(), paths.stream_cache(), paths.lyric_cache(), paths.cover_cache(),
                       paths.tmp(), paths.cache() / "cache.json", paths.cache() / "downloads.json"}});
    groups.push_back({"covers", "封面", "曲库封面图片（covers/）", {paths.covers()}});
    groups.push_back({"library", "歌单与专辑", "自建歌单、专辑（library/）", {paths.library()}});
    groups.push_back({"mv", "MV", "已下载的 MV 视频（mv/）", {paths.mv()}});
    groups.push_back({"index", "索引与统计", "曲库索引、播放次数（index/、usec.json）",
                      {paths.index_dir(), paths.usec_file()}});
    groups.push_back({"logs", "运行日志", "服务端日志（logs/）", {paths.logs()}});
    groups.push_back({"presets", "音效预设", "EQ 预设（presets/）", {paths.presets()}});
    groups.push_back({"plugins", "插件", "已安装的插件（plugins/）", {paths.plugins()}});
    groups.push_back({"settings", "应用设置", "主题 / 播放 / 音源偏好（settings.json）",
                      {paths.root / "settings.json"}});
    return groups;
}

DataUsage path_usage(const std::filesystem::path& path) {
    DataUsage usage;
    if (path.empty()) return usage;
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec || !std::filesystem::exists(status)) return usage;
    usage.present = true;
    if (!std::filesystem::is_directory(status)) {
        const auto size = std::filesystem::file_size(path, ec);
        if (!ec) usage.bytes = size;
        usage.files = 1;
        return usage;
    }
    std::filesystem::recursive_directory_iterator it{
        path, std::filesystem::directory_options::skip_permission_denied, ec};
    const std::filesystem::recursive_directory_iterator end;
    while (!ec && it != end) {
        std::error_code entry_ec;
        if (it->is_regular_file(entry_ec) && !entry_ec) {
            const auto size = it->file_size(entry_ec);
            if (!entry_ec) usage.bytes += size;
            ++usage.files;
        }
        it.increment(ec);
        if (ec) ec.clear();   // 个别目录无权限时跳过，不中断统计
    }
    return usage;
}

// 只在 <root> 之内操作：拒绝根目录本身与任何越界路径
bool inside_root(const std::filesystem::path& root, const std::filesystem::path& target) {
    const auto root_path = root.lexically_normal();
    const auto target_path = target.lexically_normal();
    if (root_path == target_path) return false;
    auto root_it = root_path.begin();
    auto target_it = target_path.begin();
    for (; root_it != root_path.end(); ++root_it, ++target_it) {
        if (target_it == target_path.end()) return false;
        if (*root_it != *target_it) return false;
    }
    return true;
}

// 递归删除一个条目；Windows 上被占用的文件（例如正在写入的日志）退化为清空内容
void purge_entry(const std::filesystem::path& entry, PurgeReport& report) {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(entry, ec);
    if (ec || !std::filesystem::exists(status)) return;

    if (std::filesystem::is_directory(status)) {
        std::filesystem::directory_iterator it{
            entry, std::filesystem::directory_options::skip_permission_denied, ec};
        const std::filesystem::directory_iterator end;
        while (!ec && it != end) {
            const std::filesystem::path child = it->path();
            it.increment(ec);
            purge_entry(child, report);
        }
        std::error_code remove_ec;
        std::filesystem::remove_all(entry, remove_ec);
        if (remove_ec) ++report.failed;
        return;
    }

    const auto size = std::filesystem::file_size(entry, ec);
    const std::uintmax_t bytes = ec ? 0 : size;

    std::error_code remove_ec;
    std::filesystem::remove(entry, remove_ec);
    if (remove_ec) {
        // 被占用的文件（例如日志）：退化为清空内容
        std::error_code truncate_ec;
        std::filesystem::resize_file(entry, 0, truncate_ec);
        if (truncate_ec) {
            ++report.failed;
            return;
        }
    }
    report.bytes += bytes;
    ++report.files;
}

}  // namespace

ApiResponse ApiDispatcher::h_data(const ApiRequest& request) {
    const std::string action = request.segment(2);

    // GET /music/data：占用概览
    if (action.empty() || action == "info") {
        json::Value groups = json::Value::array();
        std::uintmax_t total_bytes = 0;
        std::size_t total_files = 0;
        for (const auto& group : data_group_specs(paths_)) {
            DataUsage usage;
            for (const auto& path : group.paths) {
                const DataUsage part = path_usage(path);
                usage.bytes += part.bytes;
                usage.files += part.files;
                usage.present = usage.present || part.present;
            }
            total_bytes += usage.bytes;
            total_files += usage.files;
            json::Value item = json::Value::object();
            item.set("id", group.id);
            item.set("name", group.name);
            item.set("description", group.description);
            item.set("bytes", static_cast<long long>(usage.bytes));
            item.set("files", static_cast<long long>(usage.files));
            item.set("present", usage.present);
            groups.push_back(item);
        }
        json::Value data = json::Value::object();
        data.set("root", paths_.root.generic_u8string());
        data.set("usage_bytes", static_cast<long long>(total_bytes));
        data.set("total_files", static_cast<long long>(total_files));
        data.set("tracks_count", static_cast<long long>(index_.size()));
        data.set("groups", groups);
        return ApiResponse::success(data);
    }

    // GET /music/data/location：当前数据根 + 持久化配置文件 + 默认根 + 迁移状态
    if (action == "location") {
        json::Value data = json::Value::object();
        data.set("root", paths_.root.generic_u8string());
        data.set("config_path", data_config_path_);
        data.set("default_root", default_root_);
        if (migrator_) {
            json::Value ms = migrator_->status().to_json();
            ms.set("root", paths_.root.generic_u8string());
            ms.set("config_path", data_config_path_);
            data.set("migration", ms);
        }
        return ApiResponse::success(data);
    }

    // /music/data/migrate：GET=查询进度，POST=启动，POST .../migrate/cancel=取消
    if (action == "migrate") {
        if (request.method == "GET") {
            json::Value data = migrator_ ? migrator_->status().to_json() : json::Value::object();
            if (!migrator_) data.set("state", "idle");
            data.set("root", paths_.root.generic_u8string());
            return ApiResponse::success(data);
        }
        if (request.segment(3) == "cancel") {
            if (request.method != "POST") {
                return ApiResponse::failure(ErrorCode::RequestFailed, "cancel 仅支持 POST");
            }
            if (migrator_) migrator_->cancel();
            json::Value data = migrator_ ? migrator_->status().to_json() : json::Value::object();
            data.set("root", paths_.root.generic_u8string());
            return ApiResponse::success(data);
        }
        // POST /music/data/migrate：{new_root, confirm:"MIGRATE"}
        if (!migrator_) {
            return ApiResponse::failure(ErrorCode::RequestFailed, "未注入迁移器，无法迁移");
        }
        if (migrator_->active()) {
            return ApiResponse::failure(ErrorCode::RequestFailed, "已有迁移任务正在进行");
        }
        const std::string new_root = request.body.get_string("new_root", request.body.get_string("root"));
        if (new_root.empty()) {
            return ApiResponse::failure(ErrorCode::InvalidMetadata, "缺少 new_root（新的数据目录）");
        }
        if (request.body.get_string("confirm") != "MIGRATE") {
            return ApiResponse::failure(ErrorCode::RequestFailed, "确认短语不匹配，未执行迁移");
        }
        const Status st = migrator_->start(new_root);
        if (!st.ok()) return ApiResponse::from_status(st);
        json::Value data = migrator_->status().to_json();
        data.set("root", paths_.root.generic_u8string());
        return ApiResponse::success(data);
    }

    if (action != "purge" && action != "clear") {
        return ApiResponse::failure(ErrorCode::RequestFailed, "未知数据操作: " + action);
    }

    // POST /music/data/purge：{targets:[...]|["all"], confirm:"PURGE", dry_run?:bool}
    std::vector<std::string> requested;
    if (const json::Value* targets = request.body.find("targets"); targets && targets->is_array()) {
        for (const auto& item : targets->as_array()) {
            const std::string id = item.as_string();
            if (!id.empty()) requested.push_back(id);
        }
    } else if (const json::Value* all = request.body.find("targets"); all && all->is_string()) {
        requested.push_back(all->as_string());
    }
    if (requested.empty()) {
        return ApiResponse::failure(ErrorCode::InvalidMetadata, "缺少 targets（至少选择一项）");
    }

    const bool purge_all = std::find(requested.begin(), requested.end(), "all") != requested.end();
    const auto specs = data_group_specs(paths_);
    std::vector<const DataGroupSpec*> selected;
    for (const auto& id : requested) {
        if (id == "all") continue;
        const auto found = std::find_if(specs.begin(), specs.end(),
                                        [&id](const DataGroupSpec& spec) { return spec.id == id; });
        if (found == specs.end()) {
            return ApiResponse::failure(ErrorCode::InvalidMetadata, "未知数据分类: " + id);
        }
    }
    for (const auto& spec : specs) {
        const bool wanted = purge_all ||
                            std::find(requested.begin(), requested.end(), spec.id) != requested.end();
        if (wanted) selected.push_back(&spec);
    }

    const bool dry_run = request.body.get_bool("dry_run", false);
    if (!dry_run && request.body.get_string("confirm") != "PURGE") {
        return ApiResponse::failure(ErrorCode::RequestFailed, "确认短语不匹配，未执行清除");
    }

    std::vector<std::string> selected_ids;
    for (const auto* spec : selected) selected_ids.push_back(spec->id);
    const auto chose = [&selected_ids](std::string_view id) {
        return std::find(selected_ids.begin(), selected_ids.end(), id) != selected_ids.end();
    };

    // 先停掉播放会话，避免删除正在使用的文件；下载任务随后从磁盘重载（文件已删即为空）
    if (!dry_run) engine_.reset_session();
    // 删日志目录前释放日志文件句柄，否则 Windows 上文件被占用无法删除
    const bool reset_logs = !dry_run && chose("logs");
    if (reset_logs) log_suspend_sinks();

    PurgeReport report;
    for (const auto* spec : selected) {
        for (const auto& path : spec->paths) {
            if (!inside_root(paths_.root, path)) {
                ++report.failed;
                continue;
            }
            if (dry_run) {
                const DataUsage usage = path_usage(path);
                report.bytes += usage.bytes;
                report.files += usage.files;
            } else {
                purge_entry(path, report);
            }
        }
    }

    json::Value purged = json::Value::array();
    for (const auto* spec : selected) purged.push_back(spec->id);

    if (!dry_run) {
        // 曲库相关目录被清空 → 重置索引（避免指向已删除的文件）
        if (chose("tracks") || chose("cache") || chose("index")) {
            index_.clear();
            index_.save();
        }
        if (cache_ && chose("cache")) cache_->set_config(CacheConfig());
        if (downloads_) downloads_->load();
        if (chose("plugins")) {
            std::vector<std::string> ids;
            for (const auto& manifest : plugins_.manifests()) ids.push_back(manifest.id);
            for (const auto& id : ids) plugins_.remove_manifest(id);
        }
        if (settings_) {
            if (chose("settings")) settings_->values() = AppSettings();
            settings_->save();   // 补齐 settings.json，避免下次读取仍拿到被删的旧文件
        }
        // 目录布局重新建好，服务可继续正常读写
        const Status layout = paths_.ensure_layout();
        if (reset_logs) log_resume_sinks();
        RM_LOG_WARN("data", "已清除数据: " + std::to_string(report.files) + " 个文件 / " +
                                std::to_string(report.bytes) + " 字节，失败 " +
                                std::to_string(report.failed) + " 项");
        if (!layout.ok()) {
            return ApiResponse::failure(layout.code, "数据已清除，但重建目录失败: " + layout.message);
        }
    }

    json::Value data = json::Value::object();
    data.set("dry_run", dry_run);
    data.set("root", paths_.root.generic_u8string());
    data.set("purged", purged);
    data.set("removed_files", static_cast<long long>(report.files));
    data.set("removed_bytes", static_cast<long long>(report.bytes));
    data.set("failed", static_cast<long long>(report.failed));
    return ApiResponse::success(data);
}

// 「打开所在文件夹」：POST /music/open-folder
//   body 三选一：task_id（下载任务，用其落盘路径）/ sha（曲库曲目）/ path（绝对路径或相对曲库根）
//   都没给 → 打开曲库 tracks 目录（即下载落盘位置）。
//   文件存在时选中该文件，目录则直接打开。
ApiResponse ApiDispatcher::h_open_folder(const ApiRequest& request) {
    const std::string task_id = request.body.get_string("task_id");
    const std::string sha = request.body.get_string("sha");
    const std::string raw_path = request.body.get_string("path");

    std::string target;
    if (!task_id.empty()) {
        if (!downloads_) return ApiResponse::failure(ErrorCode::RequestFailed, "下载模块未装配");
        auto task = downloads_->get(task_id);
        if (!task.ok()) return ApiResponse::from_status(task.status());
        if (task.value().path.empty()) {
            // 排队 / 下载中 / 失败的任务都还没有落盘文件，别让资源管理器去开一个不存在的目录
            return ApiResponse::failure(ErrorCode::FileNotFound, "该下载任务还没有落盘文件");
        }
        target = task.value().path;
    } else if (!sha.empty()) {
        auto resolved = audio_path_for(sha);
        if (!resolved.ok()) {
            return ApiResponse::failure(resolved.status().code,
                                        "该曲目还没有本地文件，下载后才能打开所在文件夹");
        }
        target = resolved.value();
    } else if (!raw_path.empty()) {
        std::filesystem::path file(raw_path);
        if (file.is_relative()) file = paths_.root / file;
        target = file.u8string();
    } else {
        // 兜底：打开下载落盘根目录
        target = paths_.tracks().u8string();
    }

    const Status opened = reveal_in_file_manager(target);
    if (!opened.ok()) return ApiResponse::from_status(opened);

    json::Value data = json::Value::object();
    // 曲库里存的 location 用 / 分隔，回显给用户前统一成原生分隔符
    data.set("path", std::filesystem::path(target).make_preferred().u8string());
    data.set("folder", containing_folder(target));
    return ApiResponse::success(data);
}

ApiResponse ApiDispatcher::h_settings(const ApiRequest& request) {
    if (!settings_) {
        AppSettings defaults;
        json::Value data = defaults.to_json();
        return ApiResponse::success(data);
    }
    if (request.method == "POST") {
        settings_->values().merge_json(request.body);
        auto saved = settings_->save();
        if (!saved.ok()) return ApiResponse::from_status(saved);
    }
    return ApiResponse::success(settings_->values().to_json());
}

}  // namespace radmusic
