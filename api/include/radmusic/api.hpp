// Rad music API - HTTP/本地 API 层（对应 Readme.md 第 2 节「API 调用」）
//
// 设计与 2.1「调用形态」一致：本地库调用与 REST 语义完全相同，
// 本层只做「请求 → 领域模块 → {code, message, data}」的映射，不含网络实现。
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "radmusic/engine.hpp"
#include "radmusic/error.hpp"
#include "radmusic/json.hpp"
#include "radmusic/plugin.hpp"
#include "radmusic/search.hpp"
#include "radmusic/shell.hpp"
#include "radmusic/storage.hpp"

namespace radmusic {

class CacheManager;      // cache.hpp
class DownloadManager;   // download.hpp
class DataMigrator;      // migrate.hpp（数据迁移）
class IdentifyService;   // identify.hpp（本地曲目联网识别）
class PlaylistStore;     // collections.hpp（v0.10）
class AlbumStore;        // collections.hpp（v0.10）
class MvStore;           // collections.hpp（v0.10）
class SettingsStore;     // settings.hpp（宿主设置）
class UpdateService;     // update.hpp（检查更新）

// 导入服务接口（由 import 模块实现，见 import.hpp）
class ImportService {
public:
    virtual ~ImportService() = default;
    virtual Result<json::Value> scan(const json::Value& request) = 0;
    virtual Result<json::Value> run(const json::Value& request) = 0;
    virtual Result<json::Value> status(std::string_view import_id) = 0;
    virtual Result<json::Value> report(std::string_view import_id) = 0;
};

struct ApiRequest {
    std::string method = "GET";                                  // GET / POST / DELETE
    std::string path;                                            // 如 /music/load
    std::vector<std::pair<std::string, std::string>> query;      // 查询参数
    json::Value body = json::Value::object();
    std::vector<std::string> path_segments;                      // /music/download/{id} 的 id 等

    std::string query_value(std::string_view key, std::string def = std::string()) const;
    std::string segment(size_t index, std::string def = std::string()) const;
};

struct ApiResponse {
    ErrorCode code = ErrorCode::Ok;
    std::string message;
    json::Value data = json::Value::object();

    bool ok() const { return code == ErrorCode::Ok; }
    json::Value to_json() const;

    static ApiResponse success(json::Value data = json::Value::object());
    static ApiResponse failure(ErrorCode code, std::string message = std::string());
    static ApiResponse from_status(const Status& status);
};

// 调音（1.7 / 2.9）：宿主统一 DSP 链的参数与预设管理
struct EqBand {
    std::string type = "peak";     // peak / low_shelf / high_shelf
    double freq = 1000.0;
    double gain_db = 0.0;
    double q = 1.0;
};

struct EqState {
    bool enabled = false;
    std::string preset;
    double preamp_db = 0.0;
    std::vector<EqBand> bands;
    int latency_ms = 0;
    std::string engine = "builtin";

    json::Value to_json() const;
    static EqState from_json(const json::Value& v);
};

class ApiDispatcher {
public:
    ApiDispatcher(const LibraryPaths& paths, PluginRegistry& plugins, CoreEngine& engine,
                  MetaCache& meta, LibraryIndex& index);

    // 由宿主装配（未装配的接口返回 1011 request failed）
    void set_cache(CacheManager* cache) { cache_ = cache; }
    void set_downloads(DownloadManager* downloads) { downloads_ = downloads; }
    void set_importer(ImportService* importer) { importer_ = importer; }
    // 本地曲目联网识别（1.10 扩展；未装配时 /music/identify 返回 1011）
    void set_identifier(IdentifyService* identifier) { identifier_ = identifier; }
    // v0.10：歌单 / 专辑 / MV（未装配时对应接口返回 1011）
    void set_playlists(PlaylistStore* store) { playlists_ = store; }
    void set_albums(AlbumStore* store) { albums_ = store; }
    void set_mvs(MvStore* store) { mvs_ = store; }
    // 源优先级（2.5 规则 2 / 2.7）
    void set_source_priority(std::vector<std::string> priority) { source_priority_ = std::move(priority); }
    // 宿主设置存储（设置页：主题 / 音质偏好 / 视频源 / 插件启用状态）
    void set_settings(SettingsStore* settings) { settings_ = settings; }
    // 自定义合并（merge.hpp：POST /music/library/merge 等）
    void set_merge(MergeStore* store) { merge_ = store; }
    // 重启宿主自身（设置页「重启服务」/ 迁移后切换数据根使用）
    // new_root 为空 = 沿用当前数据根；非空 = 用新数据根重启
    void set_restart_callback(std::function<Status(const std::string& new_root)> callback) {
        restart_callback_ = std::move(callback);
    }
    // 关闭宿主自身（设置页「关闭服务」）：退出当前进程，不重新拉起
    void set_shutdown_callback(std::function<Status()> callback) {
        shutdown_callback_ = std::move(callback);
    }
    // 数据迁移（设置页「存储位置 / 迁移数据」）
    void set_migrator(DataMigrator* migrator) { migrator_ = migrator; }
    // 数据根持久化信息（由宿主注入）：config_path = 保存所选位置的文件，default_root = 内置默认根
    void set_data_location(std::string config_path, std::string default_root) {
        data_config_path_ = std::move(config_path);
        default_root_ = std::move(default_root);
    }
    // 检查更新（update.hpp：/music/update* 系列）
    void set_update(UpdateService* service) { update_ = service; }

    // 外部插件目录（--plugin-dir），供 /music/plugins/{id}/panel 等定位面板文件
    void set_plugin_dirs(std::vector<std::filesystem::path> dirs) { plugin_dirs_ = std::move(dirs); }

    // 事件（宿主可转发给 UI / SMTC）
    std::function<void(const PlayStatus&)> on_status;

    ApiResponse handle(const ApiRequest& request);
    // 便捷调用：解析 "GET /music/status?sha=xxx" 形式
    ApiResponse call(std::string_view method_and_path, const json::Value& body = json::Value::object());

    // 2.9 调音状态
    const EqState& eq() const { return eq_; }
    Status set_eq(const EqState& state);

    // 本地音乐（二进制，供 HTTP 层直接调用，不走 JSON 路由）：
    //  import_upload：把上传字节写入暂存并立即导入曲库（1.10）
    //  audio_path_for：把 sha 解析为可读取的音频文件绝对路径（供 HTTP 传输层发送）
    ApiResponse import_upload(std::string_view filename, const std::string& data);
    Result<std::string> audio_path_for(std::string_view sha) const;
    // 读取功能插件的面板文件内容（ui.panel），供 HTTP 层以 text/html 提供
    Result<std::string> plugin_panel_content(const std::string& id) const;
    // 封面文件（covers/<hash>.<ext>）：供 HTTP 传输层直接发送，不做 JSON 路由
    Result<std::string> cover_path_for(std::string_view name) const;
    // v0.13：原始字节导入（.rdl 歌单 / .rdm 音频包 / 普通音频），由 HTTP 层在解析 JSON 之前调用
    ApiResponse share_import(std::string_view filename, const std::string& data);
    // 导出产物 <root>/exports/<name> 的绝对路径（供 HTTP 层以附件下载）
    Result<std::string> export_file_for(std::string_view name) const;

private:
    ApiResponse h_load(const ApiRequest&);
    ApiResponse h_play(const ApiRequest&, std::string_view action);
    ApiResponse h_status(const ApiRequest&);
    ApiResponse h_info(const ApiRequest&);
    ApiResponse h_verify(const ApiRequest&);
    ApiResponse h_search(const ApiRequest&);
    ApiResponse h_request(const ApiRequest&);
    ApiResponse h_lyric(const ApiRequest&);
    ApiResponse h_quality(const ApiRequest&);
    ApiResponse h_cache(const ApiRequest&);
    ApiResponse h_download(const ApiRequest&);
    ApiResponse h_import(const ApiRequest&);
    ApiResponse h_identify(const ApiRequest&);
    ApiResponse h_library(const ApiRequest&);
    // 从曲库移除（DELETE /music/library/{sha}）：删除 metadata + 可选删除本地文件
    ApiResponse library_remove(const ApiRequest&);
    // 自定义合并：把被错误拆开的多条曲目归一到同一首歌（音质堆叠）
    ApiResponse library_merge(const ApiRequest&);
    ApiResponse library_unmerge(const ApiRequest&);
    ApiResponse library_merges(const ApiRequest&);
    ApiResponse h_eq(const ApiRequest&);
    ApiResponse h_dsp(const ApiRequest&);
    // v0.10：歌单 / 专辑 / MV / 日志
    ApiResponse h_playlist(const ApiRequest&);
    ApiResponse h_album(const ApiRequest&);
    ApiResponse h_mv(const ApiRequest&);
    ApiResponse h_logs(const ApiRequest&);
    // 设置页：插件管理与宿主设置
    ApiResponse h_plugins(const ApiRequest&);
    ApiResponse h_plugin_settings(const ApiRequest&);   // GET /music/plugins/{id}/settings
    ApiResponse h_settings(const ApiRequest&);
    // 重启宿主自身（POST /music/restart）
    ApiResponse h_restart(const ApiRequest&);
    // 关闭宿主自身（POST /music/shutdown）：退出进程，不重新拉起
    ApiResponse h_shutdown(const ApiRequest&);
    // 歌单 / 专辑搜索（GET/POST /music/{playlist|album}/search）
    ApiResponse h_playlist_search(const ApiRequest&);
    ApiResponse h_album_search(const ApiRequest&);
    // 导入外部音源歌单（POST /music/playlist/import）：链接/ID → 插件 get_playlist → 建库
    ApiResponse h_playlist_import(const ApiRequest&);
    // 设置页：数据管理（占用统计 / 清除全部数据）
    ApiResponse h_data(const ApiRequest&);
    // 在系统文件管理器中显示文件（POST /music/open-folder，见 shell.hpp）
    ApiResponse h_open_folder(const ApiRequest&);
    // 检查更新（update.hpp）：GET /music/update，POST /music/update/{check|download|apply}
    ApiResponse h_update(const ApiRequest&);
    // v0.13 歌单分享 / 曲库导出（share.hpp）：
    //   GET  /music/share/playlist?id=  → .rdl 内容
    //   POST /music/share/export        → 导出音频文件或 .rdm 音频包
    ApiResponse h_share(const ApiRequest&);
    // 原始字节导入（.rdl / .rdm / 普通音频），由 HTTP 层在解析 JSON 之前直接调用

    const LibraryPaths& paths_;
    PluginRegistry& plugins_;
    CoreEngine& engine_;
    MetaCache& meta_;
    LibraryIndex& index_;

    CacheManager* cache_ = nullptr;
    DownloadManager* downloads_ = nullptr;
    ImportService* importer_ = nullptr;
    IdentifyService* identifier_ = nullptr;
    PlaylistStore* playlists_ = nullptr;
    AlbumStore* albums_ = nullptr;
    MvStore* mvs_ = nullptr;
    SettingsStore* settings_ = nullptr;
    MergeStore* merge_ = nullptr;
    DataMigrator* migrator_ = nullptr;
    UpdateService* update_ = nullptr;
    std::string data_config_path_;   // 保存所选数据根的文件（宿主注入）
    std::string default_root_;       // 内置默认数据根（宿主注入）
    std::function<Status(const std::string& new_root)> restart_callback_;   // 由宿主注入：触发 radmusic_server 重启
    std::function<Status()> shutdown_callback_;                             // 由宿主注入：触发 radmusic_server 退出

    std::vector<std::string> source_priority_;
    EqState eq_;
    std::vector<std::filesystem::path> plugin_dirs_;   // --plugin-dir 列表（定位插件面板文件）
};

}  // namespace radmusic
