// Rad music API - 音质定义（对应 Readme.md 1.1.1 / 5.4 / 2.13）
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "radmusic/json.hpp"

namespace radmusic {

// 归一化等级（比较顺序由高到低：HiRes > Lossless > High > Medium > Low > Unknown）
enum class QualityLevel { Unknown = 0, Low = 1, Medium = 2, High = 3, Lossless = 4, HiRes = 5 };

const char* quality_level_id(QualityLevel level);          // "hires" / "lossless" / ...
QualityLevel quality_level_from_id(std::string_view id);   // 未知 → Unknown
int quality_level_rank(QualityLevel level);

// 按 1.1.1 的阈值推断等级
QualityLevel infer_quality_level(bool lossless,
                                 std::optional<int> sample_rate,
                                 std::optional<int> bit_depth,
                                 std::optional<int> bitrate);

struct Quality {
    QualityLevel level = QualityLevel::Unknown;
    std::string codec;                 // flac / alac / mp3 / aac / opus / vorbis / pcm
    bool lossless = false;
    std::optional<int> bitrate;        // kbps
    std::optional<int> sample_rate;    // Hz
    std::optional<int> bit_depth;
    std::optional<int> channels;
    std::string quality_id;            // 来源平台的音质标识（本地可省略）
    std::string label;                 // 展示名，来自插件定义

    bool is_known() const { return level != QualityLevel::Unknown; }

    // 先比 level，再比 sample_rate、bit_depth、bitrate，最后比 codec（无损优先）
    int compare(const Quality& other) const;
    bool operator<(const Quality& other) const { return compare(other) < 0; }
    bool operator==(const Quality& other) const { return compare(other) == 0; }
    bool operator!=(const Quality& other) const { return compare(other) != 0; }

    // 仅比较“音质身份”（用于同曲同质判定）：level + codec + bitrate + 采样率/位深
    bool same_identity(const Quality& other) const;

    // 音质目录名后半段：在线用 quality_id，本地用 <level>-<codec>-<bitrate>
    std::string key_suffix() const;

    json::Value to_json() const;
    static Quality from_json(const json::Value& v);
};

// 插件清单中的一条音质定义（Readme.md 5.4）
struct QualityLevelSpec {
    std::string id;        // 平台音质标识（同一插件内唯一）
    std::string label;     // 展示名
    Quality spec;          // 归一化等级与编码参数

    bool valid() const { return !id.empty() && spec.is_known(); }
    json::Value to_json() const;
    static QualityLevelSpec from_json(const json::Value& v);
    // 校验：id 非空、不重复、level 必须是归一化枚举之一
    static bool validate_all(const std::vector<QualityLevelSpec>& specs, std::string* error);
};

// quality_key：<platform>.<quality_id>（在线）或 local.<level>-<codec>-<bitrate>（本地/已下载）
std::string quality_key(const std::string& platform, const Quality& quality);

// 由插件清单把 (platform, quality_id) 解析为完整 Quality；找不到返回 nullopt
std::optional<Quality> resolve_quality(const std::vector<QualityLevelSpec>& specs, std::string_view quality_id);

// 按用户偏好挑选音质：请求 "auto" / 归一化 level / 平台 quality_id
// 返回该源可用音质中的最佳匹配；无匹配返回 nullopt（调用方返回 1021/1027）
std::optional<Quality> pick_quality(const std::vector<QualityLevelSpec>& specs,
                                    const std::vector<std::string>& available_ids,
                                    std::string_view request);

}  // namespace radmusic
