// Rad music API - Function 插件标准（对应 Readme.md 第 5 节）
// 播放源类（type=source）：无界面，提供 platform 支持、request 能力与平台音质定义
// 其他类（type=function）：有界面，提供与音源无关的功能扩展
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "radmusic/error.hpp"
#include "radmusic/json.hpp"
#include "radmusic/model.hpp"
#include "radmusic/quality.hpp"

namespace radmusic {

enum class PluginType { Source, Function };

const char* plugin_type_id(PluginType type);
PluginType plugin_type_from_id(std::string_view id);   // 未知 → Function

struct PluginLimits {
    int max_concurrency = 4;
    int qps = 5;

    json::Value to_json() const;
    static PluginLimits from_json(const json::Value& v);
};

struct PluginSettingsSchema {
    // 入口端点：宿主设置面板「打开设置页」时跳到该 URL（相对/绝对均可）
    std::string endpoint;
    // 插件自描述的配置项；前端按 type 渲染：int / bool / string / select（后端只需给 schema）
    std::vector<json::Value> schema;   // 每个元素形如 {"key","label","type","min","max","default","hint","options"}

    bool present() const { return !endpoint.empty() || !schema.empty(); }
    json::Value to_json() const;
    static PluginSettingsSchema from_json(const json::Value& v);
};

struct PluginUi {
    std::string panel;
    std::string settings;
    std::vector<std::string> menu;   // [菜单名, 项名]
    bool present() const { return !panel.empty() || !settings.empty() || !menu.empty(); }

    json::Value to_json() const;
    static PluginUi from_json(const json::Value& v);
};

// plugin.json（Readme 5.4）
struct PluginManifest {
    std::string id;
    std::string name;
    PluginType type = PluginType::Function;
    std::vector<std::string> platforms;        // 仅播放源类可非空
    std::string api_version = "1";
    std::string version;
    std::string version_range;                 // 如 ">=1.0.0"
    std::string entry;                         // 动态库/脚本入口
    // 外部插件接入方式（Readme 5.5）：
    //   ""/"builtin" = 由宿主编译期内置实现；"http" = 通过本地 HTTP JSON-RPC 与插件进程通信
    std::string transport;
    std::string endpoint;                      // transport=http 时使用，如 http://127.0.0.1:8848
    std::vector<std::string> command;          // 可选：宿主启动时自动拉起插件进程的命令行
    std::vector<std::string> capabilities;     // search/get_song/get_url/get_qualities/get_lyric/...
    std::vector<QualityLevelSpec> quality_levels;  // 仅播放源类可声明
    PluginLimits limits;
    std::vector<std::string> permissions;      // network/account/library_read/library_write/audio_dsp
    PluginUi ui;                               // 播放源类必须为空
    PluginSettingsSchema settings_schema;      // 插件自描述的可配置项（设置面板渲染用）

    json::Value to_json() const;
    static PluginManifest from_json(const json::Value& v);

    // 5.4 / 5.5 校验：类型与 ui/platforms/quality_levels 不得矛盾；api_version 必须兼容
    Status validate(std::string_view host_api_version = "1") const;

    bool has_capability(std::string_view op) const;
    bool has_permission(std::string_view permission) const;
    bool provides_platform(std::string_view platform) const;
};

// 宿主提供给其他类插件的 API（5.3）：插件不得自行实现 platform 支持
class HostApi {
public:
    virtual ~HostApi() = default;
    virtual Result<SearchResponse> search(const SearchRequest& request) = 0;
    virtual Result<json::Value> request(std::string_view platform, std::string_view op,
                                        const json::Value& params) = 0;
    virtual Result<BaseInfo> load(std::string_view key) = 0;
    virtual Status start() = 0;
    virtual PlayStatus status() const = 0;
    virtual Status subscribe_play_events(std::string_view subscriber_id) = 0;
};

// 播放源类插件（5.2）
class ISourcePlugin {
public:
    virtual ~ISourcePlugin() = default;
    virtual const PluginManifest& manifest() const = 0;

    virtual std::vector<std::string> platforms() const = 0;
    virtual bool supports(std::string_view version) const = 0;
    virtual std::vector<std::string> ops() const = 0;
    virtual std::vector<QualityLevelSpec> quality_levels() const = 0;

    virtual Result<BaseInfo> resolve(std::string_view item_id) = 0;
    virtual Result<SearchItem> search(std::string_view keyword, const json::Value& options) = 0;
    virtual Result<std::string> open(std::string_view item_id) = 0;
    virtual Result<json::Value> request(std::string_view op, const json::Value& params) = 0;
    virtual Result<std::vector<Quality>> qualities(std::string_view item_id) = 0;

    // 一次搜索返回多条结果（默认退化为单条，兼容只实现 search() 的旧插件）。
    // 真实音源插件应覆盖此方法，让 /music/search 能返回该源的完整结果列表。
    virtual Result<std::vector<SearchItem>> search_all(std::string_view keyword, const json::Value& options) {
        auto one = search(keyword, options);
        if (!one.ok()) return Result<std::vector<SearchItem>>(one.status());
        std::vector<SearchItem> items;
        items.push_back(std::move(one.value()));
        return Result<std::vector<SearchItem>>::success(std::move(items));
    }
};

// 其他类插件（5.3）
class IFunctionPlugin {
public:
    virtual ~IFunctionPlugin() = default;
    virtual const PluginManifest& manifest() const = 0;
    virtual std::vector<std::string> ui_entries() const = 0;
    virtual Status on_enable(HostApi& host) = 0;
    virtual Status on_disable() = 0;
};

// 插件注册表：按 platform / capability / 类型查找，处理禁用与冲突
class PluginRegistry {
public:
    void add_source(std::shared_ptr<ISourcePlugin> plugin);
    void add_function(std::shared_ptr<IFunctionPlugin> plugin);

    std::shared_ptr<ISourcePlugin> source_for_platform(std::string_view platform) const;
    std::vector<std::shared_ptr<ISourcePlugin>> enabled_sources() const;
    std::vector<std::shared_ptr<IFunctionPlugin>> function_plugins() const;

    // plugin.json 校验通过并加载的清单
    Status add_manifest(const PluginManifest& manifest);
    std::vector<PluginManifest> manifests() const;
    const PluginManifest* manifest_of(std::string_view plugin_id) const;

    // 卸载清单（设置页「删除插件」用）；返回是否确实删掉了
    bool remove_manifest(std::string_view plugin_id);
    // 该清单是否挂了真实实现（原生播放源/功能插件）；纯声明清单只有元数据
    bool has_implementation(std::string_view plugin_id) const;
    // 是否因崩溃被隔离
    bool is_crashed(std::string_view plugin_id) const;
    // 声明了某能力的插件 id 列表（设置页「视频源」等选择器用）
    std::vector<std::string> plugins_with_capability(std::string_view op) const;

    // 某源是否声明了某个 op（未声明 → 1012）
    Status check_capability(std::string_view platform, std::string_view op) const;
    // 平台音质定义（唯一来源是插件清单，见 5.4 / 2.13）
    std::vector<QualityLevelSpec> quality_levels_of(std::string_view platform) const;

    void set_enabled(std::string_view plugin_id, bool enabled);
    bool is_enabled(std::string_view plugin_id) const;
    // 标记崩溃隔离（1013）
    void mark_crashed(std::string_view plugin_id);

private:
    struct Entry {
        PluginManifest manifest;
        std::shared_ptr<ISourcePlugin> source;
        std::shared_ptr<IFunctionPlugin> function;
        bool enabled = true;
        bool crashed = false;
    };
    std::vector<Entry> entries_;
};

}  // namespace radmusic
