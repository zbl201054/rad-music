// Rad music API - 歌词（对应 Readme.md 1.5 / 2.8）
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "radmusic/error.hpp"
#include "radmusic/json.hpp"

namespace radmusic {

struct LyricWord {
    int t_ms = 0;         // 相对歌曲开始的毫秒
    std::string text;
};

struct LyricLine {
    int time_ms = 0;      // 行时间戳
    std::string text;     // 原文
    std::string tr;       // 翻译
    std::string roma;     // 罗马音
    std::vector<LyricWord> words;  // 逐字（增强 LRC 才有）
    bool has_time = true; // false 表示纯文本歌词的占位行

    json::Value to_json(bool include_words) const;
    static LyricLine from_json(const json::Value& v);
};

struct Lyric {
    std::string sha;
    std::string source;                 // 来源平台；本地文件为空
    std::string format = "lrc";         // lrc / txt
    bool timed = true;                  // 是否带时间轴
    int offset_ms = 0;                  // [offset:] 标签（正值表示歌词提前）
    bool has_offset_tag = false;
    std::vector<LyricLine> lines;       // 必须按 time_ms 升序

    bool empty() const { return lines.empty(); }
    json::Value to_json(bool include_tr, bool include_roma, bool include_words) const;
    static Lyric from_json(const json::Value& v);
};

// 解析 LRC：标准 [mm:ss.xx] / [mm:ss.xxx] / [mm:ss]，以及增强 <mm:ss.xx> 逐字
// 纯文本（无任何时间戳）会得到 timed=false 的歌词，不视为失败
Result<Lyric> parse_lrc(std::string_view text);

// 导出为 LRC 文本（用于写回本地 / 测试）
std::string to_lrc(const Lyric& lyric, bool include_words = true);

// 按时间戳把翻译/罗马音合并进主歌词（时间戳相差 <= tolerance_ms 视为同一行），返回合并行数
int merge_track(Lyric& main, const Lyric& extra, int tolerance_ms = 50, bool as_romaji = false);

// 应用用户偏移（正值表示歌词提前）：返回生效后的偏移
int apply_offset(Lyric& lyric, int user_offset_ms);

// 定位当前行（对 lines 二分查找）。position_ms 为播放位置（不含偏移），
// 函数内部使用 position_ms + lyric.offset_ms 进行比较；返回下标，-1 表示尚无歌词行
int find_active_line(const Lyric& lyric, int position_ms);

// 校验并排序（去重同一时间戳的重复行）
void normalize_lyric(Lyric& lyric);

}  // namespace radmusic
