// Rad music API - 歌词模块实现（对应 Readme.md 1.5 / 2.8）
//
// 约定与降级策略：
//  * 单行解析失败（时间戳数字非法等）只跳过该行，绝不整体失败；
//  * 输入全为空白时返回 Ok 的空歌词（lines 为空）；
//  * 完全没有时间戳但有文本 -> timed = false、format = "txt"，每行一个 has_time = false 的行；
//  * 解析结果按 time_ms 稳定升序（乱序输入会被排序）；同一时间戳的多行按
//    原文 / 翻译 / 罗马音 三轨填充（Readme 1.5 的双语写法），相同文本去重；
//  * 增强逐字：<mm:ss.xx> 标记其后文本的起始时间，words 与 text 保持可逆（words 文本拼接 == text）。

#include "radmusic/lyrics.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace radmusic {
namespace {

// ---------------------------------------------------------------- 字符工具

bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_alnum(char c) { return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\v' || c == '\f' || c == '\r' || c == '\n';
}

std::string trim(std::string_view s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && is_space(s[b])) ++b;
    while (e > b && is_space(s[e - 1])) --e;
    return std::string(s.substr(b, e - b));
}

std::string lower_ascii(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
    }
    return out;
}

// 按 \n / \r / \r\n 切分；保留空行（后续会被跳过）
std::vector<std::string> split_lines(std::string_view text) {
    std::vector<std::string> out;
    size_t i = 0;
    while (true) {
        size_t j = i;
        while (j < text.size() && text[j] != '\n' && text[j] != '\r') ++j;
        out.emplace_back(text.substr(i, j - i));
        if (j >= text.size()) break;
        if (text[j] == '\r' && j + 1 < text.size() && text[j + 1] == '\n') {
            j += 2;
        } else {
            ++j;
        }
        i = j;
    }
    return out;
}

// ---------------------------------------------------------------- 时间戳

// 解析 "mm:ss" / "mm:ss.x" / "mm:ss.xx" / "mm:ss.xxx"（mm/ss 允许任意位数字）
bool parse_time_tag(std::string_view s, int& out_ms) {
    const size_t colon = s.find(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 >= s.size()) return false;
    if (s.find(':', colon + 1) != std::string_view::npos) return false;  // 多于一个冒号

    const std::string_view mm = s.substr(0, colon);
    std::string_view rest = s.substr(colon + 1);
    for (char c : mm) {
        if (!is_digit(c)) return false;
    }

    std::string_view sec = rest;
    std::string_view frac;
    const size_t dot = rest.find('.');
    if (dot != std::string_view::npos) {
        sec = rest.substr(0, dot);
        frac = rest.substr(dot + 1);
        if (frac.empty() || frac.size() > 3) return false;  // .x / .xx / .xxx
        for (char c : frac) {
            if (!is_digit(c)) return false;
        }
    }
    if (sec.empty()) return false;
    for (char c : sec) {
        if (!is_digit(c)) return false;
    }

    long long minutes = 0;
    for (char c : mm) {
        minutes = minutes * 10 + (c - '0');
        if (minutes > 100000) return false;  // 明显非法，视为解析失败
    }
    long long seconds = 0;
    for (char c : sec) {
        seconds = seconds * 10 + (c - '0');
        if (seconds > 100000) return false;
    }

    int frac_ms = 0;
    if (!frac.empty()) {
        int f = 0;
        for (char c : frac) f = f * 10 + (c - '0');
        if (frac.size() == 1) {
            frac_ms = f * 100;
        } else if (frac.size() == 2) {
            frac_ms = f * 10;  // 两位小数按厘秒（标准 LRC）
        } else {
            frac_ms = f;  // 三位小数按毫秒
        }
    }

    const long long total = minutes * 60000 + seconds * 1000 + frac_ms;
    if (total > static_cast<long long>(INT_MAX)) return false;
    out_ms = static_cast<int>(total);
    return true;
}

// [offset:] 的值：可带 +/-，可含空白
bool parse_ms_value(std::string_view s, int& out) {
    size_t i = 0;
    while (i < s.size() && is_space(s[i])) ++i;
    int sign = 1;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        if (s[i] == '-') sign = -1;
        ++i;
    }
    if (i >= s.size()) return false;
    long long v = 0;
    bool any = false;
    for (; i < s.size() && is_digit(s[i]); ++i) {
        v = v * 10 + (s[i] - '0');
        any = true;
        if (v > 100000000LL) return false;
    }
    if (!any) return false;
    while (i < s.size() && is_space(s[i])) ++i;
    if (i != s.size()) return false;
    out = static_cast<int>(sign * v);
    return true;
}

// 形如 "xx:yy" / "00:1x.2z"：看起来想写时间戳但数字非法 -> 该行整体跳过
bool looks_like_time_tag(std::string_view s) {
    const size_t colon = s.find(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 >= s.size()) return false;
    const std::string_view a = s.substr(0, colon);
    const std::string_view b = s.substr(colon + 1);
    for (char c : a) {
        if (!is_alnum(c)) return false;  // "Foo: bar" 这类含空格/中文的当普通文本
    }
    if (!is_alnum(b[0])) return false;
    for (char c : b) {
        if (!is_alnum(c) && c != '.' && c != ':') return false;  // 00:12.34 / 00:12:34 都算“想写时间戳”
    }
    return true;
}

// [ti:] [ar:] [al:] 等元数据标签（Lyric 不保存，识别后忽略，不影响文本）
bool is_meta_key(std::string_view key) {
    static const char* kKeys[] = {"ti", "ar", "al", "au", "by", "re", "ve", "k", "length", "offset"};
    for (const char* k : kKeys) {
        if (key == k) return true;
    }
    return false;
}

// 解析增强逐字标记：<mm:ss.xx> 表示“其后文本”的起始时间
void parse_word_marks(const std::string& text, int line_ms, std::vector<LyricWord>& out, bool& has_marks) {
    size_t pos = 0;
    int pending = line_ms;
    std::string buf;
    while (pos < text.size()) {
        if (text[pos] == '<') {
            const size_t close = text.find('>', pos + 1);
            if (close != std::string::npos) {
                int ms = 0;
                if (parse_time_tag(std::string_view(text).substr(pos + 1, close - pos - 1), ms)) {
                    if (!buf.empty()) {
                        out.push_back(LyricWord{pending, buf});
                        buf.clear();
                    }
                    pending = ms;
                    has_marks = true;
                    pos = close + 1;
                    continue;
                }
            }
        }
        buf.push_back(text[pos]);
        ++pos;
    }
    if (!buf.empty()) out.push_back(LyricWord{pending, buf});
    if (out.empty()) has_marks = false;
}

// 稳定排序 + 同一时间戳合并；纯文本行（has_time=false）不参与合并。
// 同一时间戳多行是 Readme 1.5 认可的双语写法：按 原文 / 翻译 / 罗马音 三轨依次填充，
// 绝不用后来的行覆盖已就位的 text；多余行只覆盖 roma。
void sort_and_merge_same_time(std::vector<LyricLine>& lines) {
    std::stable_sort(lines.begin(), lines.end(),
                     [](const LyricLine& a, const LyricLine& b) { return a.time_ms < b.time_ms; });
    std::vector<LyricLine> out;
    out.reserve(lines.size());
    for (LyricLine& l : lines) {
        if (!out.empty() && out.back().has_time && l.has_time && out.back().time_ms == l.time_ms) {
            LyricLine& keep = out.back();
            const std::string& t = l.text;

            if (keep.words.empty() && !l.words.empty()) keep.words = l.words;

            if (t.empty()) continue;  // 空文本不参与三轨填充
            if (t == keep.text) continue;  // 同一时间戳 + 相同文本 -> 去重
            if (keep.text.empty()) {  // 第一行没有原文时，后来的文本补原文
                keep.text = t;
                continue;
            }
            if (t == keep.tr || t == keep.roma) continue;  // 已有该文本 -> 去重
            if (keep.tr.empty()) {  // 第二行 -> 翻译
                keep.tr = t;
                continue;
            }
            if (keep.roma.empty()) {  // 第三行 -> 罗马音
                keep.roma = t;
                continue;
            }
            keep.roma = t;  // 其余同时间戳行：只覆盖 roma，不覆盖 text
            continue;
        }
        out.push_back(std::move(l));
    }
    lines = std::move(out);
}

// [mm:ss.xx]（厘秒，标准 LRC 写法）
std::string format_stamp(int ms) {
    if (ms < 0) ms = 0;
    const int mm = ms / 60000;
    const int ss = (ms % 60000) / 1000;
    const int cs = (ms % 1000) / 10;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d.%02d", mm, ss, cs);
    return std::string(buf);
}

}  // namespace

// ---------------------------------------------------------------- 解析

Result<Lyric> parse_lrc(std::string_view text) {
    Lyric lyric;

    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3);  // UTF-8 BOM
    }

    bool saw_plain = false;
    for (const std::string& line : split_lines(text)) {
        std::vector<int> stamps;
        std::string body;
        bool invalid = false;

        size_t pos = 0;
        while (pos < line.size()) {
            if (line[pos] != '[') {
                body.push_back(line[pos]);
                ++pos;
                continue;
            }
            const size_t close = line.find(']', pos + 1);
            if (close == std::string::npos) {  // 括号未闭合
                if (looks_like_time_tag(std::string_view(line).substr(pos + 1))) {
                    invalid = true;  // 半截时间戳 -> 跳过整行，避免把整首歌词误判为纯文本
                } else {
                    body.append(line, pos, std::string::npos);
                }
                break;
            }
            const std::string_view content(line.data() + pos + 1, close - pos - 1);
            int ms = 0;
            if (parse_time_tag(content, ms)) {
                stamps.push_back(ms);
            } else {
                const size_t colon = content.find(':');
                const std::string key =
                    lower_ascii(colon == std::string_view::npos ? content : content.substr(0, colon));
                if (key == "offset") {
                    int value = 0;
                    if (colon != std::string_view::npos && parse_ms_value(content.substr(colon + 1), value)) {
                        lyric.offset_ms = value;
                        lyric.has_offset_tag = true;
                    }
                } else if (is_meta_key(key) && colon != std::string_view::npos) {
                    // [ti:] [ar:] [al:] 等：Lyric 无对应字段，忽略
                } else if (looks_like_time_tag(content)) {
                    invalid = true;  // 时间戳数字非法 -> 跳过整行
                } else {
                    body.push_back('[');
                    body.append(content);
                    body.push_back(']');
                }
            }
            pos = close + 1;
        }
        if (invalid) continue;

        const std::string trimmed = trim(body);
        if (stamps.empty()) {
            if (trimmed.empty()) continue;  // 空行 / 仅标签行
            LyricLine l;
            l.time_ms = 0;
            l.text = trimmed;
            l.has_time = false;
            lyric.lines.push_back(std::move(l));
            saw_plain = true;
            continue;
        }

        std::vector<LyricWord> words;
        bool has_marks = false;
        parse_word_marks(trimmed, stamps.front(), words, has_marks);

        std::string text = trimmed;
        if (has_marks) {  // 去掉逐字标记后得到纯文本（与 words 拼接一致）
            text.clear();
            for (const LyricWord& w : words) text += w.text;
        }

        for (int t : stamps) {  // 一行多个时间戳 -> 展开成多行
            LyricLine l;
            l.time_ms = t;
            l.text = text;
            l.has_time = true;
            if (has_marks) l.words = words;
            lyric.lines.push_back(std::move(l));
        }
    }

    sort_and_merge_same_time(lyric.lines);
    normalize_lyric(lyric);
    if (!lyric.timed && saw_plain && !lyric.lines.empty()) lyric.format = "txt";
    return Result<Lyric>::success(std::move(lyric));
}

// ---------------------------------------------------------------- 导出

std::string to_lrc(const Lyric& lyric, bool include_words) {
    std::string out;
    if (lyric.offset_ms != 0) {
        out += "[offset:";
        out += std::to_string(lyric.offset_ms);
        out += "]\n";
    }
    for (const LyricLine& l : lyric.lines) {
        if (!l.has_time) {  // 纯文本占位行：不输出时间戳
            out += l.text;
            out += '\n';
            continue;
        }
        out += '[';
        out += format_stamp(l.time_ms);
        out += ']';
        if (include_words && !l.words.empty()) {
            for (const LyricWord& w : l.words) {
                out += '<';
                out += format_stamp(w.t_ms);
                out += '>';
                out += w.text;
            }
        } else {
            out += l.text;
        }
        out += '\n';
    }
    return out;
}

// ---------------------------------------------------------------- 合并 / 偏移 / 定位

int merge_track(Lyric& main, const Lyric& extra, int tolerance_ms, bool as_romaji) {
    if (tolerance_ms < 0) tolerance_ms = -tolerance_ms;
    int merged = 0;
    for (const LyricLine& e : extra.lines) {
        if (!e.has_time || e.text.empty()) continue;
        int best = -1;
        long long best_diff = 0;
        for (size_t i = 0; i < main.lines.size(); ++i) {
            const LyricLine& m = main.lines[i];
            if (!m.has_time) continue;
            long long diff = static_cast<long long>(m.time_ms) - static_cast<long long>(e.time_ms);
            if (diff < 0) diff = -diff;
            if (diff <= tolerance_ms && (best < 0 || diff < best_diff)) {
                best = static_cast<int>(i);
                best_diff = diff;
            }
        }
        if (best < 0) continue;  // 主歌词没有对应的行，跳过
        if (as_romaji) {
            main.lines[best].roma = e.text;
        } else {
            main.lines[best].tr = e.text;
        }
        ++merged;
    }
    return merged;
}

int apply_offset(Lyric& lyric, int user_offset_ms) {
    int effective;
    if (user_offset_ms != 0) {
        effective = user_offset_ms;  // 用户给了非 0 值：覆盖 [offset:] 标签
    } else if (lyric.has_offset_tag) {
        effective = lyric.offset_ms;  // 否则用标签值
    } else {
        effective = 0;
    }
    lyric.offset_ms = effective;
    return effective;  // 不修改 lines 的 time_ms
}

int find_active_line(const Lyric& lyric, int position_ms) {
    if (!lyric.timed || lyric.lines.empty()) return -1;
    const long long target = static_cast<long long>(position_ms) + lyric.offset_ms;

    size_t begin = 0;
    while (begin < lyric.lines.size() && !lyric.lines[begin].has_time) ++begin;  // 跳过纯文本占位行
    if (begin >= lyric.lines.size()) return -1;

    // 二分：最后一个 time_ms <= target
    size_t lo = begin;
    size_t hi = lyric.lines.size();
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (lyric.lines[mid].time_ms <= target) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == begin) return -1;
    return static_cast<int>(lo - 1);
}

void normalize_lyric(Lyric& lyric) {
    std::stable_sort(lyric.lines.begin(), lyric.lines.end(),
                     [](const LyricLine& a, const LyricLine& b) { return a.time_ms < b.time_ms; });

    std::vector<LyricLine> out;
    out.reserve(lyric.lines.size());
    for (LyricLine& l : lyric.lines) {
        if (!out.empty() && out.back().time_ms == l.time_ms && out.back().text == l.text) {
            // 同一时间戳 + 同一文本：去重，但保留另一份里已有的可选字段
            LyricLine& keep = out.back();
            if (keep.tr.empty()) keep.tr = l.tr;
            if (keep.roma.empty()) keep.roma = l.roma;
            if (keep.words.empty()) keep.words = l.words;
            if (!keep.has_time && l.has_time) keep.has_time = l.has_time;
            continue;
        }
        out.push_back(std::move(l));
    }
    lyric.lines = std::move(out);

    lyric.timed = false;
    for (const LyricLine& l : lyric.lines) {
        if (l.has_time) {
            lyric.timed = true;
            break;
        }
    }
}

// ---------------------------------------------------------------- JSON

json::Value LyricLine::to_json(bool include_words) const {
    json::Value v = json::Value::object();
    v.set("time_ms", json::Value(time_ms));
    v.set("text", json::Value(text));
    if (!tr.empty()) v.set("tr", json::Value(tr));
    if (!roma.empty()) v.set("roma", json::Value(roma));
    if (include_words && !words.empty()) {
        json::Value arr = json::Value::array();
        for (const LyricWord& w : words) {
            json::Value wv = json::Value::object();
            wv.set("t", json::Value(w.t_ms));  // 见 Readme 1.5 归一化示例
            wv.set("text", json::Value(w.text));
            arr.push_back(std::move(wv));
        }
        v.set("words", std::move(arr));
    }
    v.set("has_time", json::Value(has_time));
    return v;
}

LyricLine LyricLine::from_json(const json::Value& v) {
    LyricLine l;
    if (!v.is_object()) return l;
    l.time_ms = static_cast<int>(v.get_int("time_ms", 0));
    l.text = v.get_string("text");
    l.tr = v.get_string("tr");
    l.roma = v.get_string("roma");
    l.has_time = v.contains("has_time") ? v.get_bool("has_time", true) : true;  // 缺失即视为有时间戳
    const json::Value* w = v.find("words");
    if (w != nullptr && w->is_array()) {
        for (const json::Value& e : w->as_array()) {
            LyricWord word;
            if (e.contains("t")) {
                word.t_ms = static_cast<int>(e.get_int("t", 0));
            } else {
                word.t_ms = static_cast<int>(e.get_int("time_ms", 0));  // 兼容写法
            }
            word.text = e.get_string("text");
            l.words.push_back(std::move(word));
        }
    }
    return l;
}

json::Value Lyric::to_json(bool include_tr, bool include_roma, bool include_words) const {
    json::Value v = json::Value::object();
    v.set("sha", json::Value(sha));
    v.set("source", json::Value(source));
    v.set("format", json::Value(format));
    v.set("timed", json::Value(timed));
    v.set("offset_ms", json::Value(offset_ms));
    json::Value arr = json::Value::array();
    for (const LyricLine& l : lines) {
        json::Value lv = l.to_json(include_words);
        if (!include_tr) lv.erase("tr");
        if (!include_roma) lv.erase("roma");
        arr.push_back(std::move(lv));
    }
    v.set("lines", std::move(arr));
    return v;
}

Lyric Lyric::from_json(const json::Value& v) {
    Lyric l;
    if (!v.is_object()) return l;
    l.sha = v.get_string("sha");
    l.source = v.get_string("source");
    l.format = v.get_string("format", "lrc");
    if (l.format.empty()) l.format = "lrc";
    l.timed = v.get_bool("timed", true);
    l.offset_ms = static_cast<int>(v.get_int("offset_ms", 0));
    l.has_offset_tag = l.offset_ms != 0;  // has_offset_tag 不是 JSON 字段，按非 0 偏移还原
    const json::Value* arr = v.find("lines");
    if (arr != nullptr && arr->is_array()) {
        for (const json::Value& e : arr->as_array()) {
            l.lines.push_back(LyricLine::from_json(e));
        }
    }
    return l;
}

}  // namespace radmusic
