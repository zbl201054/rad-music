// Rad music API - 领域模型（base.json、搜索结果、播放状态）
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "radmusic/error.hpp"
#include "radmusic/json.hpp"
#include "radmusic/quality.hpp"

namespace radmusic {

// ---------------- base.json（Readme 1.1 / 1.1.1） ----------------
struct BaseInfo {
    std::string platform;              // netease / radish / local / 插件标识
    std::string version;               // 音乐版本
    std::string location;              // 文件地址（相对/绝对/URL）
    std::string sha;                   // 文件 SHA-256（小写 hex）
    std::string name;                  // 音乐名
    std::string author;                // 作者 / 歌手
    std::string pic;                   // 封面地址
    int secret = 0;                    // 1 = 加密，需要密码
    Quality quality;                   // 音质（可选字段，缺失按 unknown）

    // 导入扩展字段（必须可选，见 1.10）
    std::string album;
    int duration_seconds = 0;          // 0 表示未知
    int track_no = 0;                  // 0 表示未知
    std::string tag_source;            // id3v2 / vorbis / filename / manual ...
    std::string item_id;               // 在线曲目 id（platform:id 唯一键用）
    // 识别关联（1.10 扩展）：本地曲目经联网识别后记录匹配到的在线平台，
    // 与 item_id 一起构成「本地文件 ↔ 在线版本」的链接，供播放时切换音质版本
    std::string match_platform;
    // 自定义合并（merge.hpp）：归入合并组前的原始标签，非空表示这条曾被合并改写过。
    // 取消合并时用它还原，避免 song_key 漂移（1029）。
    std::string orig_name;
    std::string orig_author;
    int orig_duration = 0;

    json::Value to_json() const;
    static BaseInfo from_json(const json::Value& v);

    // 校验：平台/版本/location/sha/name 必填，sha 必须 64 位小写 hex，secret 只能 0/1
    Status validate() const;

    // 唯一键辅助
    std::string group_key() const;     // 折叠键（2.7）
    bool is_local() const { return platform == "local"; }
    bool has_sha() const { return !sha.empty(); }
};

// ---------------- 搜索（Readme 2.5 / 2.7） ----------------
struct SourceStatus {
    std::string platform;
    ErrorCode code = ErrorCode::Ok;    // 该源的结果码（0 表示成功）
    std::string message;
    long long elapsed_ms = 0;
    int result_count = 0;              // 该源返回的条目数（供前端按音源切换）

    json::Value to_json() const;
};

struct SourceEntry {
    std::string platform;
    std::string item_id;               // 该源的曲目标识
    bool available = true;
    std::string reason;                // 不可用原因（如 vip required）
    Quality quality;                   // 最高可用音质
    std::vector<Quality> qualities;    // 该源可用音质列表
    BaseInfo base;                     // 可选：归一化元数据
    std::string location;              // 可选：可直接播放/下载的地址

    json::Value to_json() const;
    static SourceEntry from_json(const json::Value& v);
};

struct SearchItem {
    std::string group_key;             // 折叠键
    std::string title;
    std::vector<std::string> artists;
    std::string album;
    int duration_seconds = 0;
    bool folded = true;
    std::string best_source;           // 默认选中源（优先级最高且可用）
    std::vector<SourceEntry> sources;  // 同一首歌的各音源（不得按音质拆开）

    size_t source_count() const { return sources.size(); }
    json::Value to_json() const;
    static SearchItem from_json(const json::Value& v);
};

struct SearchRequest {
    std::string keyword;
    std::vector<std::string> sources;  // 空 = 全部启用的源（宿主会额外并入本地曲库）
    std::string type = "song";         // song / album / artist / playlist
    int page = 1;
    int page_size = 20;
    int timeout_ms = 5000;
    std::string strategy = "fanout";   // fanout / race
    bool fold = true;

    // 筛选（v0.11）：由宿主统一过滤，分页在过滤之后进行
    std::string quality_min;           // 空 = 不限；low/medium/high/lossless/hires
    int duration_min = 0;              // 秒，0 = 不限
    int duration_max = 0;              // 秒，0 = 不限
    bool available_only = false;       // 只保留至少有一个可用音源的条目
    std::string sort = "relevance";    // relevance / title / artist / duration / quality
};

struct SearchResponse {
    std::string keyword;
    int page = 1;
    int page_size = 20;
    int total = 0;
    std::vector<SourceStatus> sources_status;
    std::vector<SearchItem> results;

    json::Value to_json() const;
};

// ---------------- 播放（Readme 1.3） ----------------
enum class PlayState { Stopped, Playing, Paused, Buffering, Ended };

const char* play_state_id(PlayState state);

struct PlayStatus {
    PlayState state = PlayState::Stopped;
    long long position_ms = 0;
    long long duration_ms = 0;
    double volume = 1.0;
    std::string sha;
    std::string platform;
    std::string item_id;

    json::Value to_json() const;
};

// ---------------- SMTC（Readme 1.6） ----------------
struct SmtcMetadata {
    std::string title;
    std::string artist;
    std::string album;
    long long duration_ms = 0;
    std::string cover_path;   // 本地可读封面
};

}  // namespace radmusic
