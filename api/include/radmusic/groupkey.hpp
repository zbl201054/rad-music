// Rad music API - 歌曲归一化、折叠键与缓存目录键（对应 Readme.md 2.7 / 1.1）
#pragma once

#include <string>
#include <string_view>

namespace radmusic {

// 折叠用的时长容差（秒）：±3 秒视为同一首歌
inline constexpr int kDurationToleranceSeconds = 3;
// song_key 中可读前缀的最大字节数
inline constexpr size_t kSongKeySlugMaxBytes = 64;
// group_key 哈希取前 N 位十六进制
inline constexpr size_t kSongKeyHashHex = 12;

// 归一化：小写、全角转半角、去掉括号内容、去除非字母数字汉字字符、压缩空白
std::string normalize_text(std::string_view text);

struct SongIdentity {
    std::string title;         // 归一化后的标题
    std::string artist;        // 归一化后的主作者
    int duration_seconds = 0;  // 取整秒

    bool empty() const { return title.empty() && artist.empty(); }
};

// 构造折叠键：normalize(title) + "|" + normalize(artist) + "|" + round(duration)
std::string group_key(std::string_view title, std::string_view primary_artist, int duration_seconds);
std::string group_key(const SongIdentity& identity);

// 解析折叠键（失败返回 false）
bool parse_group_key(std::string_view key, SongIdentity* out);

// 折叠判定：标题与主作者归一化后相同，且时长差 <= kDurationToleranceSeconds
bool songs_fold_together(const SongIdentity& a, const SongIdentity& b);
bool group_keys_fold_together(std::string_view a, std::string_view b);

// 文件系统安全化：替换非法字符、去掉结尾点/空格、规避 Windows 保留名、限制字节长度
std::string fs_sanitize(std::string_view text, size_t max_bytes = kSongKeySlugMaxBytes);

// 缓存歌曲级目录名：<安全化 title - artist>_<sha256(group_key) 前 12 位 hex>
std::string song_key(std::string_view title, std::string_view primary_artist, int duration_seconds);
std::string song_key(const SongIdentity& identity);

// 元数据缺失时的兜底目录名（在线曲目）：online_<platform>_<id 的哈希>
std::string fallback_song_key(std::string_view platform, std::string_view item_id);

// 反向校验：目录名是否与元数据一致（用于 1029 meta path mismatch）
bool song_key_consistent(std::string_view dir_name, std::string_view title,
                         std::string_view primary_artist, int duration_seconds);

}  // namespace radmusic
