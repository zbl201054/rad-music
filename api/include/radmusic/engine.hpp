// Rad music API - 核心引擎（load.cpp / start.cpp 的语义，Readme 1.2 / 1.3 / 1.5 / 1.6）
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "radmusic/decrypt.hpp"
#include "radmusic/error.hpp"
#include "radmusic/lyrics.hpp"
#include "radmusic/model.hpp"
#include "radmusic/plugin.hpp"
#include "radmusic/storage.hpp"

namespace radmusic {

// SMTC（1.6）：宿主实现（Windows 上 WinRT SystemMediaTransportControls，其他平台等价物）
class SmtcSink {
public:
    virtual ~SmtcSink() = default;
    virtual Status update_metadata(const SmtcMetadata& metadata) = 0;
    virtual Status update_state(PlayState state, long long position_ms, long long duration_ms) = 0;
    virtual Status set_buttons(bool has_next, bool has_previous, bool can_seek) = 0;
};

struct LoadedTrack {
    BaseInfo base;
    MetaPath meta_path;
    std::string play_path;       // 实际可播放路径（本地文件/解密后明文/在线 URL）
    bool decrypted = false;
    bool sha_warning = false;    // sha error 为警告（1.3 b）
    Lyric lyric;
};

struct EngineOptions {
    bool auto_rebuild_base = true;   // 1.3 规则 5：base.json 缺失先重建
    bool verify_sha = true;          // 1.3 b
    bool cache_audio = true;         // 1.3 d：加载至缓存
    bool require_platform_plugin = true;
};

class CoreEngine {
public:
    CoreEngine(const LibraryPaths& paths, PluginRegistry& plugins, EngineOptions options = {});
    ~CoreEngine();

    void set_smtc_sink(SmtcSink* sink) { smtc_ = sink; }
    Decryptor& decryptor() { return decryptor_; }
    MetaCache& meta_cache() { return meta_; }
    PluginRegistry& plugins() { return plugins_; }

    // ---- 注入点（宿主或测试提供） ----
    // 计算文件实际 sha（默认 crypto::sha256_file_hex）
    std::function<Result<std::string>(const std::string& path)> sha_provider;
    // 文件是否存在（默认 std::filesystem::exists）
    std::function<bool(const std::string& path)> file_exists;
    // base.json 缺失时的重建（本地：重新解析标签；在线：重新拉取）
    std::function<Result<BaseInfo>(std::string_view key)> rebuild_base;
    // 在线曲目取可播放地址（默认走插件 request(op="get_url")）
    std::function<Result<std::string>(const BaseInfo& base)> open_online;
    // 歌词来源（默认：本地 lyric.lrc 优先，其次播放源插件）
    std::function<Result<Lyric>(const BaseInfo& base)> lyric_provider;
    // 播放后端（默认只维护状态；真实播放器由宿主注入）
    std::function<Status(const std::string& path)> backend_start;
    std::function<Status()> backend_stop;
    // 拷贝到缓存（默认真实拷贝；测试可替换）
    std::function<bool(const std::string& from, const std::string& to)> copy_file;

    // ---- 1.3 加载 ----
    Result<LoadedTrack> load(std::string_view key, std::string_view password = std::string());

    // ---- 1.3 播放 ----
    Status start();
    Status pause();
    Status resume();
    Status stop();
    Status seek(double seconds);
    Status set_volume(double level);
    PlayStatus status() const { return status_; }
    const std::optional<LoadedTrack>& current() const { return current_; }

    // 曲库被清空后复位会话（停止后端播放、丢掉当前曲目与进度）
    void reset_session();

    // ---- 1.5 歌词 ----
    Result<Lyric> load_lyric(const BaseInfo& base);
    int active_lyric_line() const;

    // ---- 1.6 SMTC ----
    Status sync_smtc();

    // 进度驱动（等价于播放器的 on_progress / on_end 回调）
    Status advance_to(long long position_ms);
    Status notify_ended();

private:
    Status push_smtc(PlayState state);
    Result<BaseInfo> resolve_base(std::string_view key);
    std::optional<BaseInfo> find_base(std::string_view key);

    const LibraryPaths& paths_;
    PluginRegistry& plugins_;
    EngineOptions options_;
    Decryptor decryptor_;
    MetaCache meta_;
    SmtcSink* smtc_ = nullptr;
    std::optional<LoadedTrack> current_;
    PlayStatus status_;
};

}  // namespace radmusic
