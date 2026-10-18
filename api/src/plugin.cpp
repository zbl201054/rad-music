#include "radmusic/plugin.hpp"

#include <algorithm>

namespace radmusic {

const char* plugin_type_id(PluginType type) {
    return type == PluginType::Source ? "source" : "function";
}

PluginType plugin_type_from_id(std::string_view id) {
    return id == "source" ? PluginType::Source : PluginType::Function;
}

json::Value PluginLimits::to_json() const {
    json::Value v = json::Value::object();
    v.set("max_concurrency", static_cast<long long>(max_concurrency));
    v.set("qps", static_cast<long long>(qps));
    return v;
}

PluginLimits PluginLimits::from_json(const json::Value& v) {
    PluginLimits limits;
    if (!v.is_object()) return limits;
    limits.max_concurrency = static_cast<int>(v.get_int("max_concurrency", limits.max_concurrency));
    limits.qps = static_cast<int>(v.get_int("qps", limits.qps));
    return limits;
}

json::Value PluginUi::to_json() const {
    json::Value v = json::Value::object();
    if (!panel.empty()) v.set("panel", panel);
    if (!settings.empty()) v.set("settings", settings);
    if (!menu.empty()) {
        json::Value items = json::Value::array();
        for (const auto& m : menu) items.push_back(m);
        v.set("menu", items);
    }
    return v;
}

PluginUi PluginUi::from_json(const json::Value& v) {
    PluginUi ui;
    if (!v.is_object()) return ui;
    ui.panel = v.get_string("panel");
    ui.settings = v.get_string("settings");
    if (const json::Value* menu = v.find("menu"); menu && menu->is_array()) {
        for (const auto& item : menu->as_array()) ui.menu.push_back(item.as_string());
    }
    return ui;
}

json::Value PluginSettingsSchema::to_json() const {
    json::Value v = json::Value::object();
    if (!endpoint.empty()) v.set("endpoint", endpoint);
    if (!schema.empty()) {
        json::Value arr = json::Value::array();
        for (const auto& field : schema) arr.push_back(field);
        v.set("schema", arr);
    }
    return v;
}

PluginSettingsSchema PluginSettingsSchema::from_json(const json::Value& v) {
    PluginSettingsSchema s;
    if (!v.is_object()) return s;
    s.endpoint = v.get_string("endpoint");
    if (const json::Value* arr = v.find("schema"); arr && arr->is_array()) {
        for (const auto& item : arr->as_array()) {
            if (item.is_object()) s.schema.push_back(item);
        }
    }
    return s;
}

json::Value PluginManifest::to_json() const {
    json::Value v = json::Value::object();
    v.set("id", id);
    v.set("name", name);
    v.set("type", std::string(plugin_type_id(type)));
    json::Value platforms_json = json::Value::array();
    for (const auto& p : platforms) platforms_json.push_back(p);
    v.set("platforms", platforms_json);
    v.set("api_version", api_version);
    if (!version.empty()) v.set("version", version);
    if (!version_range.empty()) v.set("version_range", version_range);
    if (!entry.empty()) v.set("entry", entry);
    if (!transport.empty()) v.set("transport", transport);
    if (!endpoint.empty()) v.set("endpoint", endpoint);
    if (!command.empty()) {
        json::Value argv = json::Value::array();
        for (const auto& part : command) argv.push_back(part);
        v.set("command", argv);
    }
    json::Value caps = json::Value::array();
    for (const auto& c : capabilities) caps.push_back(c);
    v.set("capabilities", caps);
    if (!quality_levels.empty()) {
        json::Value levels = json::Value::array();
        for (const auto& spec : quality_levels) levels.push_back(spec.to_json());
        v.set("quality_levels", levels);
    }
    v.set("limits", limits.to_json());
    json::Value perms = json::Value::array();
    for (const auto& p : permissions) perms.push_back(p);
    v.set("permissions", perms);
    v.set("ui", ui.present() ? ui.to_json() : json::Value());
    v.set("settings_schema", settings_schema.present() ? settings_schema.to_json() : json::Value());
    return v;
}

PluginManifest PluginManifest::from_json(const json::Value& v) {
    PluginManifest m;
    if (!v.is_object()) return m;
    m.id = v.get_string("id");
    m.name = v.get_string("name");
    m.type = plugin_type_from_id(v.get_string("type", "function"));
    if (const json::Value* platforms_json = v.find("platforms"); platforms_json && platforms_json->is_array()) {
        for (const auto& p : platforms_json->as_array()) m.platforms.push_back(p.as_string());
    }
    m.api_version = v.get_string("api_version", "1");
    m.version = v.get_string("version");
    m.version_range = v.get_string("version_range");
    m.entry = v.get_string("entry");
    m.transport = v.get_string("transport");
    m.endpoint = v.get_string("endpoint");
    if (const json::Value* cmd = v.find("command"); cmd && cmd->is_array()) {
        for (const auto& part : cmd->as_array()) m.command.push_back(part.as_string());
    }
    if (const json::Value* caps = v.find("capabilities"); caps && caps->is_array()) {
        for (const auto& c : caps->as_array()) m.capabilities.push_back(c.as_string());
    }
    if (const json::Value* levels = v.find("quality_levels"); levels && levels->is_array()) {
        for (const auto& spec : levels->as_array()) m.quality_levels.push_back(QualityLevelSpec::from_json(spec));
    }
    if (const json::Value* limits = v.find("limits")) m.limits = PluginLimits::from_json(*limits);
    if (const json::Value* perms = v.find("permissions"); perms && perms->is_array()) {
        for (const auto& p : perms->as_array()) m.permissions.push_back(p.as_string());
    }
    if (const json::Value* ui = v.find("ui"); ui && ui->is_object()) m.ui = PluginUi::from_json(*ui);
    if (const json::Value* settings = v.find("settings_schema"); settings && settings->is_object())
        m.settings_schema = PluginSettingsSchema::from_json(*settings);
    return m;
}

Status PluginManifest::validate(std::string_view host_api_version) const {
    if (id.empty()) return Status(ErrorCode::InvalidMetadata, "plugin.json 缺少 id");
    if (name.empty()) return Status(ErrorCode::InvalidMetadata, "plugin.json 缺少 name");

    if (!api_version.empty() && !host_api_version.empty() && api_version != host_api_version) {
        return Status(ErrorCode::UnsupportedVersion,
                      "插件 api_version=" + api_version + " 与宿主 " + std::string(host_api_version) + " 不兼容");
    }

    std::string error;
    if (!QualityLevelSpec::validate_all(quality_levels, &error)) {
        return Status(ErrorCode::InvalidMetadata, error);
    }

    // 外部进程插件（5.5）：transport=http 必须给出本地 JSON-RPC 地址
    if (transport == "http" && endpoint.empty()) {
        return Status(ErrorCode::InvalidMetadata, "transport=http 的插件必须声明 endpoint");
    }

    if (type == PluginType::Source) {
        if (platforms.empty()) {
            return Status(ErrorCode::InvalidMetadata, "播放源类插件必须声明 platforms");
        }
        if (ui.present()) {
            return Status(ErrorCode::InvalidMetadata, "播放源类插件必须无界面（ui 必须为空）");
        }
    } else {
        if (!platforms.empty()) {
            return Status(ErrorCode::InvalidMetadata, "其他类插件不得声明 platforms");
        }
        if (!quality_levels.empty()) {
            return Status(ErrorCode::InvalidMetadata, "其他类插件不得声明 quality_levels");
        }
        if (!ui.present()) {
            return Status(ErrorCode::InvalidMetadata, "其他类插件必须声明界面入口 ui");
        }
    }
    return Status::success();
}

bool PluginManifest::has_capability(std::string_view op) const {
    return std::find(capabilities.begin(), capabilities.end(), op) != capabilities.end();
}

bool PluginManifest::has_permission(std::string_view permission) const {
    return std::find(permissions.begin(), permissions.end(), permission) != permissions.end();
}

bool PluginManifest::provides_platform(std::string_view platform) const {
    return std::find(platforms.begin(), platforms.end(), platform) != platforms.end();
}

// ---------------------------------------------------------------- PluginRegistry

void PluginRegistry::add_source(std::shared_ptr<ISourcePlugin> plugin) {
    if (!plugin) return;
    const PluginManifest manifest = plugin->manifest();
    for (auto& entry : entries_) {
        if (entry.manifest.id == manifest.id) {
            entry.manifest = manifest;
            entry.source = plugin;
            entry.enabled = true;
            entry.crashed = false;
            return;
        }
    }
    Entry entry;
    entry.manifest = manifest;
    entry.source = std::move(plugin);
    entries_.push_back(std::move(entry));
}

void PluginRegistry::add_function(std::shared_ptr<IFunctionPlugin> plugin) {
    if (!plugin) return;
    const PluginManifest manifest = plugin->manifest();
    for (auto& entry : entries_) {
        if (entry.manifest.id == manifest.id) {
            entry.manifest = manifest;
            entry.function = plugin;
            entry.enabled = true;
            entry.crashed = false;
            return;
        }
    }
    Entry entry;
    entry.manifest = manifest;
    entry.function = std::move(plugin);
    entries_.push_back(std::move(entry));
}

Status PluginRegistry::add_manifest(const PluginManifest& manifest) {
    Status valid = manifest.validate();
    if (!valid.ok()) return valid;
    for (auto& entry : entries_) {
        if (entry.manifest.id == manifest.id) {
            entry.manifest = manifest;
            return Status::success();
        }
    }
    Entry entry;
    entry.manifest = manifest;
    entries_.push_back(std::move(entry));
    return Status::success();
}

std::shared_ptr<ISourcePlugin> PluginRegistry::source_for_platform(std::string_view platform) const {
    for (const auto& entry : entries_) {
        if (!entry.enabled || entry.crashed || !entry.source) continue;
        if (entry.manifest.provides_platform(platform)) return entry.source;
    }
    return nullptr;
}

std::vector<std::shared_ptr<ISourcePlugin>> PluginRegistry::enabled_sources() const {
    std::vector<std::shared_ptr<ISourcePlugin>> out;
    for (const auto& entry : entries_) {
        if (entry.enabled && !entry.crashed && entry.source) out.push_back(entry.source);
    }
    return out;
}

std::vector<std::shared_ptr<IFunctionPlugin>> PluginRegistry::function_plugins() const {
    std::vector<std::shared_ptr<IFunctionPlugin>> out;
    for (const auto& entry : entries_) {
        if (entry.enabled && !entry.crashed && entry.function) out.push_back(entry.function);
    }
    return out;
}

std::vector<PluginManifest> PluginRegistry::manifests() const {
    std::vector<PluginManifest> out;
    out.reserve(entries_.size());
    for (const auto& entry : entries_) out.push_back(entry.manifest);
    return out;
}

const PluginManifest* PluginRegistry::manifest_of(std::string_view plugin_id) const {
    for (const auto& entry : entries_) {
        if (entry.manifest.id == plugin_id) return &entry.manifest;
    }
    return nullptr;
}

bool PluginRegistry::remove_manifest(std::string_view plugin_id) {
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->manifest.id == plugin_id) {
            entries_.erase(it);
            return true;
        }
    }
    return false;
}

bool PluginRegistry::has_implementation(std::string_view plugin_id) const {
    for (const auto& entry : entries_) {
        if (entry.manifest.id != plugin_id) continue;
        return static_cast<bool>(entry.source) || static_cast<bool>(entry.function);
    }
    return false;
}

bool PluginRegistry::is_crashed(std::string_view plugin_id) const {
    for (const auto& entry : entries_) {
        if (entry.manifest.id == plugin_id) return entry.crashed;
    }
    return false;
}

std::vector<std::string> PluginRegistry::plugins_with_capability(std::string_view op) const {
    std::vector<std::string> out;
    for (const auto& entry : entries_) {
        if (!entry.manifest.has_capability(op)) continue;
        out.push_back(entry.manifest.id);
    }
    return out;
}

Status PluginRegistry::check_capability(std::string_view platform, std::string_view op) const {
    bool platform_known = false;
    for (const auto& entry : entries_) {
        if (!entry.manifest.provides_platform(platform)) continue;
        platform_known = true;
        if (!entry.enabled || entry.crashed) continue;
        if (entry.manifest.has_capability(op)) return Status::success();
        return Status(ErrorCode::UnsupportedCapability,
                      "插件 " + entry.manifest.id + " 未声明 op: " + std::string(op));
    }
    if (platform_known) return Status(ErrorCode::PluginDisabled, "插件被禁用或已崩溃隔离");
    return Status(ErrorCode::PlatformError, "platform 未知或无可用插件: " + std::string(platform));
}

std::vector<QualityLevelSpec> PluginRegistry::quality_levels_of(std::string_view platform) const {
    for (const auto& entry : entries_) {
        if (entry.manifest.provides_platform(platform)) return entry.manifest.quality_levels;
    }
    return {};
}

void PluginRegistry::set_enabled(std::string_view plugin_id, bool enabled) {
    for (auto& entry : entries_) {
        if (entry.manifest.id == plugin_id) entry.enabled = enabled;
    }
}

bool PluginRegistry::is_enabled(std::string_view plugin_id) const {
    for (const auto& entry : entries_) {
        if (entry.manifest.id == plugin_id) return entry.enabled && !entry.crashed;
    }
    return false;
}

void PluginRegistry::mark_crashed(std::string_view plugin_id) {
    for (auto& entry : entries_) {
        if (entry.manifest.id == plugin_id) entry.crashed = true;
    }
}

}  // namespace radmusic
