// Rad music API - 宿主设置存储（设置页持久化）
//
// 前端设置页需要跨浏览器/跨设备保留的偏好（主题、视频源、音质偏好、播放栏圆角等）
// 统一落在 <root>/settings.json，由 HTTP API 读写（见 api.cpp 的 h_settings）。
// 这些都不是播放核心状态，缺失/损坏时回退到默认值，不影响曲库。
#pragma once

#include <string>
#include <string_view>

#include "radmusic/error.hpp"
#include "radmusic/json.hpp"
#include "radmusic/storage.hpp"

namespace radmusic {

// 设置项（全部有默认值；前端只提交需要修改的字段）
struct AppSettings {
    // 外观：主题 id（前端 CSS 变量组）、明暗模式、播放条圆角半径（px）
    std::string theme = "violet";
    std::string color_scheme = "dark";
    int player_radius = 24;
    bool compact_player = false;

    // 播放：首选音质（auto / hires / lossless / high / medium / low）、音量记忆、是否自动播放下一首
    std::string preferred_quality = "auto";
    double volume = 0.8;
    bool autoplay_next = true;

    // 视频源：MV 搜索/播放使用的插件 id（空表示自动选择第一个带 get_mv 能力的源）
    std::string mv_source;
    // 搜索：隐藏不可用音源、默认折叠
    bool hide_unavailable = false;
    bool fold_results = true;

    // 播放：切换曲目 / 暂停恢复时的淡入淡出（音量平滑过渡）
    bool fade_enabled = false;
    double fade_duration = 0.8;  // 秒，单次淡出/淡入时长

    // 导入：导入本地文件后自动联网识别（匹配在线版本与海报）
    bool identify_on_import = true;

    // 检查更新：下载线程数（1-16，按 Range 分段并发下载；1 = 单线程）
    int update_threads = 4;
    // 检查更新：发布频道（release 正式 / dev 开发 / canary 金丝雀）
    std::string update_channel = "release";

    // 插件启用状态：{plugin_id: bool}
    json::Value plugin_enabled = json::Value::object();
    // 插件级持久化值：{plugin_id: {key: value, ...}}（设置页「插件设置」用）
    json::Value plugin_settings = json::Value::object();

    json::Value to_json() const;
    // 只覆盖出现的字段（partial update）
    void merge_json(const json::Value& v);
    static AppSettings from_json(const json::Value& v);
};

class SettingsStore {
public:
    explicit SettingsStore(const LibraryPaths& paths);

    // 从 <root>/settings.json 读取；文件缺失/损坏时保持默认值（不算失败）
    Status load();
    // 原子写回 <root>/settings.json
    Status save();

    const AppSettings& values() const { return values_; }
    AppSettings& values() { return values_; }

    bool plugin_enabled(std::string_view plugin_id, bool def = true) const;
    void set_plugin_enabled(std::string_view plugin_id, bool enabled);
    void forget_plugin(std::string_view plugin_id);

    // 插件级持久化值（GET / POST）：plugin_id + 用户在面板里改过的字段
    void set_plugin_setting(std::string_view plugin_id, const json::Value& patch);
    void clear_plugin_setting(std::string_view plugin_id);

private:
    const LibraryPaths& paths_;
    AppSettings values_;
};

}  // namespace radmusic
