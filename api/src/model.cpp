#include "radmusic/model.hpp"

#include "radmusic/error.hpp"
#include "radmusic/groupkey.hpp"

namespace radmusic {
namespace {

bool is_lower_hex(std::string_view s) {
    if (s.empty()) return false;
    for (unsigned char c : s) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!ok) return false;
    }
    return true;
}

void append_string_array(json::Value& arr, const std::vector<std::string>& values) {
    for (const auto& v : values) arr.push_back(v);
}

}  // namespace

json::Value BaseInfo::to_json() const {
    json::Value v = json::Value::object();
    v.set("platform", platform);
    v.set("version", version);
    v.set("location", location);
    v.set("sha", sha);
    v.set("name", name);
    v.set("author", author);
    v.set("pic", pic);
    v.set("secret", static_cast<long long>(secret));
    if (quality.is_known() || !quality.codec.empty() || !quality.quality_id.empty()) {
        v.set("quality", quality.to_json());
    }
    if (!album.empty()) v.set("album", album);
    if (duration_seconds > 0) v.set("duration", static_cast<long long>(duration_seconds));
    if (track_no > 0) v.set("track_no", static_cast<long long>(track_no));
    if (!tag_source.empty()) v.set("tag_source", tag_source);
    if (!item_id.empty()) v.set("item_id", item_id);
    if (!match_platform.empty()) v.set("match_platform", match_platform);
    // 自定义合并：原始标签（取消合并时还原）
    if (!orig_name.empty()) v.set("orig_name", orig_name);
    if (!orig_author.empty()) v.set("orig_author", orig_author);
    if (orig_duration > 0) v.set("orig_duration", static_cast<long long>(orig_duration));
    return v;
}

BaseInfo BaseInfo::from_json(const json::Value& v) {
    BaseInfo b;
    if (!v.is_object()) return b;
    b.platform = v.get_string("platform");
    b.version = v.get_string("version");
    b.location = v.get_string("location");
    b.sha = v.get_string("sha");
    b.name = v.get_string("name");
    b.author = v.get_string("author");
    b.pic = v.get_string("pic");
    b.secret = static_cast<int>(v.get_int("secret", 0));
    if (const json::Value* q = v.find("quality")) b.quality = Quality::from_json(*q);
    b.album = v.get_string("album");
    // 插件常用 duration_seconds，base.json 规范用 duration；两者都接受
    b.duration_seconds = static_cast<int>(v.get_int("duration", v.get_int("duration_seconds", 0)));
    b.track_no = static_cast<int>(v.get_int("track_no", 0));
    b.tag_source = v.get_string("tag_source");
    b.item_id = v.get_string("item_id");
    b.match_platform = v.get_string("match_platform");
    b.orig_name = v.get_string("orig_name");
    b.orig_author = v.get_string("orig_author");
    b.orig_duration = static_cast<int>(v.get_int("orig_duration", 0));
    return b;
}

Status BaseInfo::validate() const {
    if (platform.empty()) return Status(ErrorCode::InvalidMetadata, "base.json 缺少 platform");
    if (version.empty()) return Status(ErrorCode::InvalidMetadata, "base.json 缺少 version");
    if (location.empty()) return Status(ErrorCode::InvalidMetadata, "base.json 缺少 location");
    if (name.empty()) return Status(ErrorCode::InvalidMetadata, "base.json 缺少 name");
    if (sha.empty()) return Status(ErrorCode::InvalidMetadata, "base.json 缺少 sha");
    if (sha.size() != 64 || !is_lower_hex(sha)) {
        return Status(ErrorCode::InvalidMetadata, "base.json 的 sha 必须是小写十六进制 SHA-256（64 字符）");
    }
    if (secret != 0 && secret != 1) {
        return Status(ErrorCode::InvalidMetadata, "base.json 的 secret 只能是 0 或 1");
    }
    const bool quality_present =
        quality.is_known() || !quality.codec.empty() || !quality.quality_id.empty();
    if (quality_present && !quality.is_known()) {
        return Status(ErrorCode::InvalidMetadata, "base.json 的 quality.level 非法或缺失（见 1.1.1）");
    }
    return Status::success();
}

std::string BaseInfo::group_key() const {
    return radmusic::group_key(name, author, duration_seconds);
}

json::Value SourceStatus::to_json() const {
    json::Value v = json::Value::object();
    v.set("platform", platform);
    v.set("code", static_cast<long long>(static_cast<int>(code)));
    if (!message.empty()) v.set("message", message);
    v.set("elapsed_ms", static_cast<long long>(elapsed_ms));
    v.set("result_count", static_cast<long long>(result_count));
    return v;
}

json::Value SourceEntry::to_json() const {
    json::Value v = json::Value::object();
    v.set("platform", platform);
    v.set("id", item_id);
    v.set("available", available);
    if (!reason.empty()) v.set("reason", reason);
    if (quality.is_known() || !quality.quality_id.empty()) v.set("quality", quality.to_json());
    json::Value qs = json::Value::array();
    for (const auto& q : qualities) qs.push_back(q.to_json());
    v.set("qualities", qs);
    if (!base.name.empty()) v.set("base", base.to_json());
    if (!location.empty()) v.set("location", location);
    return v;
}

SourceEntry SourceEntry::from_json(const json::Value& v) {
    SourceEntry e;
    if (!v.is_object()) return e;
    e.platform = v.get_string("platform");
    e.item_id = v.get_string("id");
    e.available = v.get_bool("available", true);
    e.reason = v.get_string("reason");
    if (const json::Value* q = v.find("quality")) e.quality = Quality::from_json(*q);
    if (const json::Value* qs = v.find("qualities"); qs && qs->is_array()) {
        for (const auto& q : qs->as_array()) e.qualities.push_back(Quality::from_json(q));
    }
    if (const json::Value* b = v.find("base")) e.base = BaseInfo::from_json(*b);
    e.location = v.get_string("location");
    return e;
}

json::Value SearchItem::to_json() const {
    json::Value v = json::Value::object();
    v.set("group_key", group_key);
    v.set("title", title);
    json::Value artist_array = json::Value::array();
    append_string_array(artist_array, artists);
    v.set("artists", artist_array);
    if (!album.empty()) v.set("album", album);
    v.set("duration", static_cast<long long>(duration_seconds));
    v.set("source_count", static_cast<long long>(sources.size()));
    v.set("folded", folded);
    if (!best_source.empty()) v.set("best_source", best_source);
    json::Value srcs = json::Value::array();
    for (const auto& s : sources) srcs.push_back(s.to_json());
    v.set("sources", srcs);
    return v;
}

SearchItem SearchItem::from_json(const json::Value& v) {
    SearchItem item;
    if (!v.is_object()) return item;
    item.group_key = v.get_string("group_key");
    item.title = v.get_string("title");
    if (const json::Value* a = v.find("artists"); a && a->is_array()) {
        for (const auto& x : a->as_array()) item.artists.push_back(x.as_string());
    }
    item.album = v.get_string("album");
    item.duration_seconds = static_cast<int>(v.get_int("duration", v.get_int("duration_seconds", 0)));
    item.folded = v.get_bool("folded", true);
    item.best_source = v.get_string("best_source");
    if (const json::Value* s = v.find("sources"); s && s->is_array()) {
        for (const auto& x : s->as_array()) item.sources.push_back(SourceEntry::from_json(x));
    }
    return item;
}

json::Value SearchResponse::to_json() const {
    json::Value v = json::Value::object();
    v.set("keyword", keyword);
    v.set("page", static_cast<long long>(page));
    v.set("page_size", static_cast<long long>(page_size));
    v.set("total", static_cast<long long>(total));
    json::Value ss = json::Value::array();
    for (const auto& s : sources_status) ss.push_back(s.to_json());
    v.set("sources_status", ss);
    json::Value rs = json::Value::array();
    for (const auto& r : results) rs.push_back(r.to_json());
    v.set("results", rs);
    return v;
}

const char* play_state_id(PlayState state) {
    switch (state) {
        case PlayState::Playing: return "playing";
        case PlayState::Paused: return "paused";
        case PlayState::Buffering: return "buffering";
        case PlayState::Ended: return "ended";
        default: return "stopped";
    }
}

json::Value PlayStatus::to_json() const {
    json::Value v = json::Value::object();
    v.set("state", std::string(play_state_id(state)));
    v.set("position_ms", position_ms);
    v.set("duration_ms", duration_ms);
    v.set("volume", volume);
    if (!sha.empty()) v.set("sha", sha);
    if (!platform.empty()) v.set("platform", platform);
    if (!item_id.empty()) v.set("id", item_id);
    return v;
}

}  // namespace radmusic
