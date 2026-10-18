#include "radmusic/remote_source.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include "radmusic/crypto.hpp"
#include "radmusic/groupkey.hpp"
#include "radmusic/http.hpp"
#include "radmusic/log.hpp"
#include "radmusic/model.hpp"
#include "radmusic/quality.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace radmusic {
namespace {

// 把插件返回的业务错误码直接映射成宿主错误码（插件与宿主共用 Readme 第 3 节错误码）
Status status_from_code(int64_t code, const std::string& message) {
    if (code == 0) return Status::success();
    if (code >= 1001 && code <= 1032) return Status(static_cast<ErrorCode>(code), message);
    return Status(ErrorCode::RequestFailed, message.empty() ? "插件请求失败" : message);
}

// 从原始 HTTP 响应里取出状态码与 body
bool split_http_response(const std::string& raw, int* status, std::string* body) {
    const size_t header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos) return false;
    const std::string head = raw.substr(0, header_end);
    const size_t first_space = head.find(' ');
    if (first_space == std::string::npos) return false;
    const size_t code_begin = first_space + 1;
    int value = 0;
    for (size_t i = code_begin; i < head.size() && head[i] >= '0' && head[i] <= '9'; ++i) {
        value = value * 10 + (head[i] - '0');
    }
    if (value == 0) return false;
    if (status) *status = value;
    if (body) *body = raw.substr(header_end + 4);
    return true;
}

class RemoteSourcePlugin : public ISourcePlugin {
public:
    explicit RemoteSourcePlugin(RemoteSourceOptions options) : options_(std::move(options)) {
        manifest_.id = options_.id;
        manifest_.name = options_.name;
        manifest_.type = PluginType::Source;
        manifest_.platforms = options_.platforms;
        manifest_.capabilities = options_.capabilities;
        manifest_.quality_levels = options_.quality_levels;
        manifest_.limits.max_concurrency = 2;
        manifest_.limits.qps = 3;
        manifest_.permissions = {"network", "account"};
        manifest_.transport = "http";
        manifest_.endpoint = options_.endpoint;
        manifest_.version = options_.version;
        manifest_.settings_schema = options_.settings_schema;
        if (manifest_.capabilities.empty()) {
            manifest_.capabilities = {"search", "get_song", "get_url", "get_qualities", "get_lyric"};
        }
        std::string host;
        uint16_t port = 80;
        std::string prefix;
        if (parse_endpoint(options_.endpoint, &host, &port, &prefix)) {
            host_ = host;
            port_ = port;
            prefix_ = prefix;
            endpoint_ok_ = true;
        }
    }

    const PluginManifest& manifest() const override { return manifest_; }
    std::vector<std::string> platforms() const override { return manifest_.platforms; }
    bool supports(std::string_view version) const override { return version.empty() || version == "1.0.0"; }
    std::vector<std::string> ops() const override { return manifest_.capabilities; }
    std::vector<QualityLevelSpec> quality_levels() const override { return manifest_.quality_levels; }

    Result<BaseInfo> resolve(std::string_view item_id) override {
        json::Value params = json::Value::object();
        params.set("id", std::string(item_id));
        auto reply = call("get_song", params);
        if (!reply.ok()) return Result<BaseInfo>(reply.status());
        json::Value song = reply.value();
        if (song.is_object() && song.find("song")) song = song.get("song");
        if (!song.is_object()) {
            return Result<BaseInfo>::failure(ErrorCode::InvalidMetadata, "插件 get_song 返回格式不正确");
        }
        return Result<BaseInfo>::success(base_from_json(song, std::string(item_id)));
    }

    Result<SearchItem> search(std::string_view keyword, const json::Value& options) override {
        auto items = search_all(keyword, options);
        if (!items.ok()) return Result<SearchItem>(items.status());
        if (items.value().empty()) return Result<SearchItem>::success(SearchItem{});
        SearchItem first = items.value().front();
        return Result<SearchItem>::success(std::move(first));
    }

    Result<std::vector<SearchItem>> search_all(std::string_view keyword, const json::Value& options) override {
        json::Value params = json::Value::object();
        params.set("keyword", std::string(keyword));
        params.set("page", static_cast<long long>(options.get_int("page", 1)));
        params.set("page_size", static_cast<long long>(options.get_int("page_size", 20)));
        params.set("type", options.get_string("type", "song"));
        auto reply = call("search", params);
        if (!reply.ok()) return Result<std::vector<SearchItem>>(reply.status());

        const json::Value& data = reply.value();
        const json::Value* items = &data;
        if (data.is_object()) {
            if (const json::Value* found = data.find("items")) items = found;
            else if (const json::Value* found = data.find("songs")) items = found;
            else if (const json::Value* found = data.find("results")) items = found;
        }
        if (!items->is_array()) {
            return Result<std::vector<SearchItem>>::failure(ErrorCode::InvalidMetadata,
                                                            "插件 search 返回格式不正确（需要 items 数组）");
        }
        std::vector<SearchItem> out;
        out.reserve(items->as_array().size());
        for (const json::Value& raw : items->as_array()) {
            out.push_back(search_item_from_json(raw));
        }
        return Result<std::vector<SearchItem>>::success(std::move(out));
    }

    Result<std::string> open(std::string_view item_id) override {
        json::Value params = json::Value::object();
        params.set("id", std::string(item_id));
        auto reply = call("get_url", params);
        if (!reply.ok()) return Result<std::string>(reply.status());
        const json::Value& value = reply.value();
        std::string url;
        if (value.is_string()) {
            url = value.as_string();
        } else if (value.is_object()) {
            url = value.get_string("url", value.get_string("location"));
        }
        if (url.empty()) return Result<std::string>::failure(ErrorCode::PlatformError, "插件未返回播放地址");
        return Result<std::string>::success(url);
    }

    Result<json::Value> request(std::string_view op, const json::Value& params) override {
        auto reply = call(op, params);
        if (!reply.ok()) return reply;
        json::Value value = reply.value();
        if (op == "get_qualities" && value.is_object()) {
            if (const json::Value* list = value.find("qualities"); list && list->is_array()) return *list;
        }
        return Result<json::Value>::success(std::move(value));
    }

    Result<std::vector<Quality>> qualities(std::string_view item_id) override {
        json::Value params = json::Value::object();
        params.set("id", std::string(item_id));
        auto reply = request("get_qualities", params);
        if (!reply.ok()) return Result<std::vector<Quality>>(reply.status());
        std::vector<Quality> out;
        const json::Value& value = reply.value();
        if (value.is_array()) {
            for (const json::Value& item : value.as_array()) out.push_back(Quality::from_json(item));
        }
        return Result<std::vector<Quality>>::success(std::move(out));
    }

private:
    // 一次 JSON-RPC 调用，返回插件回包里的 data
    Result<json::Value> call(std::string_view op, const json::Value& params) {
        if (!endpoint_ok_) {
            return Result<json::Value>::failure(ErrorCode::PluginDisabled,
                                                "插件 endpoint 不合法: " + options_.endpoint);
        }
        json::Value request = json::Value::object();
        request.set("op", std::string(op));
        request.set("params", params);
        const std::string body = request.dump();
        const std::string path = prefix_ + "/request";
        std::string raw = "POST " + path + " HTTP/1.1\r\n";
        raw += "Host: " + host_ + ":" + std::to_string(port_) + "\r\n";
        raw += "Content-Type: application/json; charset=utf-8\r\n";
        raw += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        raw += "Connection: close\r\n\r\n";
        raw += body;

        auto response = http_roundtrip(host_, port_, raw, options_.timeout_ms);
        if (!response.ok()) {
            return Result<json::Value>::failure(
                ErrorCode::PluginDisabled,
                "插件进程未响应（" + options_.endpoint + "）。请先启动插件：" + start_hint());
        }
        int http_status = 0;
        std::string payload;
        if (!split_http_response(response.value(), &http_status, &payload)) {
            return Result<json::Value>::failure(ErrorCode::RequestFailed, "插件返回的 HTTP 响应无法解析");
        }
        if (http_status != 200) {
            return Result<json::Value>::failure(ErrorCode::RequestFailed,
                                                "插件返回 HTTP " + std::to_string(http_status));
        }
        json::Value reply = json::Value::parse(payload);
        if (reply.is_null()) {
            return Result<json::Value>::failure(ErrorCode::RequestFailed, "插件返回的不是合法 JSON");
        }
        const int64_t code = reply.get_int("code", 0);
        const std::string message = reply.get_string("message");
        if (code != 0) {
            Status status = status_from_code(code, message.empty() ? ("插件请求失败: " + std::string(op)) : message);
            if (status.code == ErrorCode::PluginDisabled) {
                status.message += "（" + options_.endpoint + "）";
            }
            return Result<json::Value>(status);
        }
        if (const json::Value* data = reply.find("data")) return Result<json::Value>::success(*data);
        return Result<json::Value>::success(json::Value::object());
    }

    std::string start_hint() const {
        return "python plugins/" + (manifest_.platforms.empty() ? std::string("plugin") : manifest_.platforms.front()) +
               "/main.py";
    }

    BaseInfo base_from_json(const json::Value& value, const std::string& fallback_id) {
        BaseInfo base = BaseInfo::from_json(value);
        if (base.platform.empty() && !manifest_.platforms.empty()) base.platform = manifest_.platforms.front();
        if (base.item_id.empty()) base.item_id = fallback_id;
        if (base.name.empty()) base.name = fallback_id;
        if (base.version.empty()) base.version = options_.version;
        if (base.sha.empty()) base.sha = crypto::sha256_hex(base.platform + ":" + base.item_id);
        base.secret = 0;
        if (base.location.rfind("http://", 0) != 0 && base.location.rfind("https://", 0) != 0) {
            base.location = "http://plugin.invalid/" + base.platform + "/" + base.item_id;
        }
        return base;
    }

    SearchItem search_item_from_json(const json::Value& raw) {
        SearchItem item;
        if (!raw.is_object()) return item;
        item.title = raw.get_string("name", raw.get_string("title"));
        if (const json::Value* artists = raw.find("artists"); artists && artists->is_array()) {
            for (const json::Value& artist : artists->as_array()) item.artists.push_back(artist.as_string());
        } else if (const json::Value* artist = raw.find("artist"); artist && artist->is_string()) {
            item.artists.push_back(artist->as_string());
        }
        item.album = raw.get_string("album");
        if (const json::Value* seconds = raw.find("duration_seconds"); seconds && seconds->is_number()) {
            item.duration_seconds = static_cast<int>(seconds->as_double());
        } else if (const json::Value* duration = raw.find("duration"); duration && duration->is_number()) {
            const double value = duration->as_double();
            item.duration_seconds = static_cast<int>(value > 1000.0 ? std::lround(value / 1000.0) : std::lround(value));
        }

        const std::string platform = manifest_.platforms.empty() ? manifest_.id : manifest_.platforms.front();
        std::string item_id = raw.get_string("id", raw.get_string("item_id"));
        if (item_id.empty()) return item;

        SourceEntry entry;
        entry.platform = platform;
        entry.item_id = item_id;
        entry.available = raw.get_bool("available", true);
        entry.reason = raw.get_string("reason");
        if (const json::Value* qualities = raw.find("qualities"); qualities && qualities->is_array()) {
            for (const json::Value& quality_json : qualities->as_array()) {
                Quality quality = Quality::from_json(quality_json);
                if (quality.is_known()) entry.qualities.push_back(quality);
            }
        }
        if (!entry.qualities.empty()) {
            entry.quality = entry.qualities.front();
            for (const Quality& candidate : entry.qualities) {
                if (entry.quality < candidate) entry.quality = candidate;
            }
        }

        entry.base = base_from_json(raw, item_id);
        entry.base.name = item.title.empty() ? entry.base.name : item.title;
        entry.base.author = item.artists.empty() ? entry.base.author : item.artists.front();
        entry.base.album = item.album;
        entry.base.duration_seconds = item.duration_seconds;
        if (entry.base.pic.empty()) entry.base.pic = raw.get_string("pic");
        entry.base.quality = entry.quality;

        item.group_key = group_key(item.title, item.artists.empty() ? std::string() : item.artists.front(),
                                   item.duration_seconds);
        item.folded = true;
        item.best_source = platform;
        item.sources.push_back(std::move(entry));
        if (item.group_key.empty()) item.group_key = item_id;
        return item;
    }

    RemoteSourceOptions options_;
    PluginManifest manifest_;
    std::string host_;
    uint16_t port_ = 80;
    std::string prefix_;
    bool endpoint_ok_ = false;
};

}  // namespace

bool parse_endpoint(const std::string& endpoint, std::string* host, uint16_t* port, std::string* path_prefix) {
    const std::string prefix = "http://";
    if (endpoint.rfind(prefix, 0) != 0) return false;
    std::string rest = endpoint.substr(prefix.size());
    std::string path;
    const size_t slash = rest.find('/');
    if (slash != std::string::npos) {
        path = rest.substr(slash);
        rest = rest.substr(0, slash);
        while (!path.empty() && path.back() == '/') path.pop_back();
    }
    std::string host_part = rest;
    uint16_t port_part = 80;
    const size_t colon = rest.rfind(':');
    if (colon != std::string::npos) {
        host_part = rest.substr(0, colon);
        const int parsed = std::atoi(rest.substr(colon + 1).c_str());
        if (parsed <= 0 || parsed > 65535) return false;
        port_part = static_cast<uint16_t>(parsed);
    }
    if (host_part.empty()) return false;
    if (host) *host = host_part;
    if (port) *port = port_part;
    if (path_prefix) *path_prefix = path;
    return true;
}

bool remote_source_ping(const std::string& endpoint, int timeout_ms) {
    std::string host;
    uint16_t port = 80;
    std::string prefix;
    if (!parse_endpoint(endpoint, &host, &port, &prefix)) return false;
    const std::string raw = "GET " + prefix + "/health HTTP/1.1\r\nHost: " + host + ":" + std::to_string(port) +
                            "\r\nConnection: close\r\n\r\n";
    auto response = http_roundtrip(host, port, raw, timeout_ms);
    if (!response.ok()) return false;
    int status = 0;
    return split_http_response(response.value(), &status, nullptr) && status == 200;
}

std::shared_ptr<ISourcePlugin> make_remote_source(RemoteSourceOptions options) {
    if (options.platforms.empty()) return nullptr;
    return std::make_shared<RemoteSourcePlugin>(std::move(options));
}

#ifdef _WIN32
namespace {
std::wstring widen_utf8(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::wstring quote_arg(const std::wstring& arg) {
    if (arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    for (const wchar_t ch : arg) {
        if (ch == L'"') {
            out += L"\\\"";
        } else {
            out += ch;
        }
    }
    out += L"\"";
    return out;
}
}  // namespace
#endif

bool spawn_plugin_process(const std::vector<std::string>& command, const std::string& cwd) {
    if (command.empty()) return false;
#ifdef _WIN32
    std::wstring line;
    for (size_t i = 0; i < command.size(); ++i) {
        if (i > 0) line += L' ';
        line += quote_arg(widen_utf8(command[i]));
    }
    std::wstring workdir = widen_utf8(cwd);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE,
                                   CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr,
                                   workdir.empty() ? nullptr : workdir.c_str(), &startup, &process);
    if (ok) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
    return ok != FALSE;
#else
    (void)command;
    (void)cwd;
    return false;
#endif
}

}  // namespace radmusic
