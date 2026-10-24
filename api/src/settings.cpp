#include "radmusic/settings.hpp"

#include <filesystem>

#include "radmusic/log.hpp"

namespace radmusic {
namespace {

bool valid_quality(std::string_view id) {
    return id == "auto" || id == "hires" || id == "lossless" || id == "high" || id == "medium" ||
           id == "low" || id == "unknown";
}

}  // namespace

json::Value AppSettings::to_json() const {
    json::Value v = json::Value::object();
    v.set("theme", theme);
    v.set("color_scheme", color_scheme);
    v.set("player_radius", static_cast<long long>(player_radius));
    v.set("compact_player", compact_player);
    v.set("preferred_quality", preferred_quality);
    v.set("volume", volume);
    v.set("autoplay_next", autoplay_next);
    v.set("mv_source", mv_source);
    v.set("hide_unavailable", hide_unavailable);
    v.set("fold_results", fold_results);
    v.set("fade_enabled", fade_enabled);
    v.set("fade_duration", fade_duration);
    v.set("identify_on_import", identify_on_import);
    v.set("update_threads", static_cast<long long>(update_threads));
    v.set("update_channel", update_channel);
    v.set("plugin_enabled", plugin_enabled);
    if (!plugin_settings.empty()) v.set("plugin_settings", plugin_settings);
    return v;
}

void AppSettings::merge_json(const json::Value& v) {
    if (!v.is_object()) return;

    if (v.contains("theme")) theme = v.get_string("theme", theme);
    if (v.contains("color_scheme")) {
        const std::string scheme = v.get_string("color_scheme", color_scheme);
        if (scheme == "dark" || scheme == "light" || scheme == "auto") color_scheme = scheme;
    }
    if (v.contains("player_radius")) {
        int radius = static_cast<int>(v.get_int("player_radius", player_radius));
        if (radius < 0) radius = 0;
        if (radius > 48) radius = 48;
        player_radius = radius;
    }
    if (v.contains("compact_player")) compact_player = v.get_bool("compact_player", compact_player);
    if (v.contains("preferred_quality")) {
        const std::string quality = v.get_string("preferred_quality", preferred_quality);
        if (valid_quality(quality)) preferred_quality = quality;
    }
    if (v.contains("volume")) {
        double volume_value = v.get_double("volume", volume);
        if (volume_value < 0.0) volume_value = 0.0;
        if (volume_value > 1.0) volume_value = 1.0;
        volume = volume_value;
    }
    if (v.contains("autoplay_next")) autoplay_next = v.get_bool("autoplay_next", autoplay_next);
    if (v.contains("mv_source")) mv_source = v.get_string("mv_source", mv_source);
    if (v.contains("hide_unavailable")) hide_unavailable = v.get_bool("hide_unavailable", hide_unavailable);
    if (v.contains("fold_results")) fold_results = v.get_bool("fold_results", fold_results);
    if (v.contains("fade_enabled")) fade_enabled = v.get_bool("fade_enabled", fade_enabled);
    if (v.contains("fade_duration")) {
        double value = v.get_double("fade_duration", fade_duration);
        if (value < 0.1) value = 0.1;
        if (value > 5.0) value = 5.0;
        fade_duration = value;
    }
    if (v.contains("identify_on_import")) {
        identify_on_import = v.get_bool("identify_on_import", identify_on_import);
    }
    if (v.contains("update_threads")) {
        int threads = static_cast<int>(v.get_int("update_threads", update_threads));
        if (threads < 1) threads = 1;
        if (threads > 16) threads = 16;
        update_threads = threads;
    }
    if (v.contains("update_channel")) {
        const std::string channel = v.get_string("update_channel", update_channel);
        if (channel == "release" || channel == "dev" || channel == "canary") update_channel = channel;
    }
    if (const json::Value* enabled = v.find("plugin_enabled"); enabled && enabled->is_object()) {
        for (const auto& kv : enabled->as_object()) {
            if (kv.second.is_bool()) plugin_enabled.set(kv.first, kv.second.as_bool());
        }
    }
    if (const json::Value* ps = v.find("plugin_settings"); ps && ps->is_object()) {
        for (const auto& kv : ps->as_object()) {
            if (kv.second.is_object()) plugin_settings.set(kv.first, kv.second);
        }
    }
}

AppSettings AppSettings::from_json(const json::Value& v) {
    AppSettings settings;
    settings.merge_json(v);
    return settings;
}

SettingsStore::SettingsStore(const LibraryPaths& paths) : paths_(paths) {}

Status SettingsStore::load() {
    const std::filesystem::path file = paths_.root / "settings.json";
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec) {
        values_ = AppSettings();
        return Status::success();
    }
    auto text = read_text_file(file);
    if (!text.ok()) {
        RM_LOG_WARN("settings", "读取 settings.json 失败: " + text.status().message);
        return Status::success();
    }
    json::Value parsed = json::Value::parse(text.value());
    if (parsed.is_null()) {
        RM_LOG_WARN("settings", "settings.json 不是合法 JSON，已回退默认设置");
        return Status::success();
    }
    values_ = AppSettings::from_json(parsed);
    return Status::success();
}

Status SettingsStore::save() {
    std::error_code ec;
    std::filesystem::create_directories(paths_.root, ec);
    return write_text_file_atomic(paths_.root, paths_.root / "settings.json", values_.to_json().dump(2));
}

bool SettingsStore::plugin_enabled(std::string_view plugin_id, bool def) const {
    const json::Value* v = values_.plugin_enabled.find(plugin_id);
    if (!v || !v->is_bool()) return def;
    return v->as_bool();
}

void SettingsStore::set_plugin_enabled(std::string_view plugin_id, bool enabled) {
    values_.plugin_enabled.set(std::string(plugin_id), enabled);
}

void SettingsStore::forget_plugin(std::string_view plugin_id) {
    values_.plugin_enabled.erase(std::string(plugin_id));
    values_.plugin_settings.erase(std::string(plugin_id));
}

void SettingsStore::set_plugin_setting(std::string_view plugin_id, const json::Value& patch) {
    const std::string key(plugin_id);
    json::Value entry = values_.plugin_settings.is_object() ? values_.plugin_settings.get(key) : json::Value();
    if (!entry.is_object()) entry = json::Value::object();
    if (patch.is_object()) {
        for (const auto& kv : patch.as_object()) {
            entry.set(kv.first, kv.second);
        }
    }
    values_.plugin_settings.set(key, entry);
}

void SettingsStore::clear_plugin_setting(std::string_view plugin_id) {
    values_.plugin_settings.erase(std::string(plugin_id));
}

}  // namespace radmusic
