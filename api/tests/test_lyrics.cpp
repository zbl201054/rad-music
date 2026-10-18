// Rad music API - 歌词模块测试（零依赖）
//
// 覆盖：标准 LRC 解析与多时间戳展开、乱序排序、重复时间戳合并、[offset:] 标签与
//       apply_offset 正负值、增强逐字（words）、纯文本 -> timed=false、非法时间戳跳过、
//       merge_track（翻译/罗马音、容差内命中与容差外不命中）、find_active_line 边界与偏移、
//       to_lrc -> parse_lrc 回环、to_json -> from_json 回环、空输入。

#include "radmusic/lyrics.hpp"
#include "test_framework.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace {

using radmusic::Lyric;
using radmusic::LyricLine;
using radmusic::LyricWord;
using radmusic::Result;

Lyric parse_ok(std::string_view text) {
    Result<Lyric> r = radmusic::parse_lrc(text);
    CHECK(r.ok());
    return r.value();
}

}  // namespace

// ---------------------------------------------------------------- 标准解析

TEST_CASE(test_lyrics_parse_standard_and_multi_timestamp) {
    const std::string text =
        "[ti:示例]\n"
        "[ar:歌手]\n"
        "[al:专辑]\n"
        "[offset:-200]\n"
        "[00:12.34][01:20.00]同一行两次\n"
        "[00:05.5]一位小数\n"
        "[00:07.123]三位小数\n"
        "[00:09]无小数\n";

    const Lyric l = parse_ok(text);
    CHECK_EQ(l.lines.size(), static_cast<size_t>(5));
    CHECK(l.timed);
    CHECK_EQ(l.format, std::string("lrc"));
    CHECK_EQ(l.offset_ms, -200);
    CHECK(l.has_offset_tag);

    // 排序后：5500 / 7123 / 9000 / 12340 / 80000
    CHECK_EQ(l.lines[0].time_ms, 5500);
    CHECK_EQ(l.lines[0].text, std::string("一位小数"));
    CHECK_EQ(l.lines[1].time_ms, 7123);
    CHECK_EQ(l.lines[1].text, std::string("三位小数"));
    CHECK_EQ(l.lines[2].time_ms, 9000);
    CHECK_EQ(l.lines[2].text, std::string("无小数"));
    CHECK_EQ(l.lines[3].time_ms, 12340);
    CHECK_EQ(l.lines[3].text, std::string("同一行两次"));
    CHECK_EQ(l.lines[4].time_ms, 80000);
    CHECK_EQ(l.lines[4].text, std::string("同一行两次"));

    for (const LyricLine& line : l.lines) {
        CHECK(line.has_time);
    }
}

TEST_CASE(test_lyrics_parse_out_of_order_sorted) {
    const Lyric l = parse_ok("[00:30.00]第三\n[00:10.00]第一\n[00:20.00]第二\n");
    CHECK_EQ(l.lines.size(), static_cast<size_t>(3));
    CHECK_EQ(l.lines[0].time_ms, 10000);
    CHECK_EQ(l.lines[0].text, std::string("第一"));
    CHECK_EQ(l.lines[1].time_ms, 20000);
    CHECK_EQ(l.lines[1].text, std::string("第二"));
    CHECK_EQ(l.lines[2].time_ms, 30000);
    CHECK_EQ(l.lines[2].text, std::string("第三"));
    CHECK(l.timed);
}

// 同一时间戳多行 = 双语歌词（Readme 1.5）：依次填 原文 / 翻译 / 罗马音，不覆盖 text
TEST_CASE(test_lyrics_same_timestamp_three_tracks) {
    const Lyric l = parse_ok(
        "[00:10.00]Hello\n"
        "[00:10.00]你好\n"
        "[00:10.00]ni hao\n"
        "[00:20.00]World\n");
    CHECK_EQ(l.lines.size(), static_cast<size_t>(2));
    CHECK_EQ(l.lines[0].time_ms, 10000);
    CHECK_EQ(l.lines[0].text, std::string("Hello"));   // 第一行 -> 原文
    CHECK_EQ(l.lines[0].tr, std::string("你好"));       // 第二行 -> 翻译
    CHECK_EQ(l.lines[0].roma, std::string("ni hao"));  // 第三行 -> 罗马音
    CHECK_EQ(l.lines[1].time_ms, 20000);
    CHECK_EQ(l.lines[1].text, std::string("World"));
    CHECK(l.lines[1].tr.empty());
    CHECK(l.lines[1].roma.empty());
    CHECK(l.timed);

    // 第四条不同的文本 -> 只覆盖 roma，text / tr 保持不变
    const Lyric four = parse_ok("[00:10.00]A\n[00:10.00]B\n[00:10.00]C\n[00:10.00]D\n");
    CHECK_EQ(four.lines.size(), static_cast<size_t>(1));
    CHECK_EQ(four.lines[0].text, std::string("A"));
    CHECK_EQ(four.lines[0].tr, std::string("B"));
    CHECK_EQ(four.lines[0].roma, std::string("D"));

    // 与已有轨道相同的文本 -> 去重忽略，不占用下一轨
    const Lyric same_tr = parse_ok("[00:10.00]A\n[00:10.00]B\n[00:10.00]B\n");
    CHECK_EQ(same_tr.lines.size(), static_cast<size_t>(1));
    CHECK_EQ(same_tr.lines[0].text, std::string("A"));
    CHECK_EQ(same_tr.lines[0].tr, std::string("B"));
    CHECK(same_tr.lines[0].roma.empty());

    // 后续空文本行：不参与三轨填充
    const Lyric e = parse_ok("[00:10.00]保留\n[00:10.00]\n");
    CHECK_EQ(e.lines.size(), static_cast<size_t>(1));
    CHECK_EQ(e.lines[0].text, std::string("保留"));
    CHECK(e.lines[0].tr.empty());
    CHECK(e.lines[0].roma.empty());
}

// 同一时间戳 + 完全相同文本 -> 只保留一行且 tr / roma 为空
TEST_CASE(test_lyrics_same_timestamp_identical_text_deduped) {
    const Lyric l = parse_ok("[00:10.00]同样的\n[00:10.00]同样的\n[00:20.00]另一行\n");
    CHECK_EQ(l.lines.size(), static_cast<size_t>(2));
    CHECK_EQ(l.lines[0].time_ms, 10000);
    CHECK_EQ(l.lines[0].text, std::string("同样的"));
    CHECK(l.lines[0].tr.empty());
    CHECK(l.lines[0].roma.empty());
    CHECK_EQ(l.lines[1].text, std::string("另一行"));

    // 一行多个相同时间戳（[t][t]）也不会被填成翻译
    const Lyric multi = parse_ok("[00:10.00][00:10.00]重复\n");
    CHECK_EQ(multi.lines.size(), static_cast<size_t>(1));
    CHECK_EQ(multi.lines[0].text, std::string("重复"));
    CHECK(multi.lines[0].tr.empty());
}

TEST_CASE(test_lyrics_offset_tag_and_apply_offset) {
    Lyric l = parse_ok("[offset:+500]\n[00:10.00]a\n");
    CHECK_EQ(l.offset_ms, 500);
    CHECK(l.has_offset_tag);

    CHECK_EQ(radmusic::apply_offset(l, 0), 500);  // 用户为 0 -> 用标签值
    CHECK_EQ(l.offset_ms, 500);
    CHECK_EQ(radmusic::apply_offset(l, -300), -300);  // 用户非 0 -> 覆盖标签
    CHECK_EQ(l.offset_ms, -300);
    CHECK_EQ(l.lines[0].time_ms, 10000);  // 不修改行时间戳
    CHECK(l.lines[0].has_time);

    Lyric no_tag = parse_ok("[00:01.00]x\n");
    CHECK_FALSE(no_tag.has_offset_tag);
    CHECK_EQ(radmusic::apply_offset(no_tag, 0), 0);
    CHECK_EQ(radmusic::apply_offset(no_tag, 1200), 1200);

    Lyric neg = parse_ok("[offset:-1500]\n[00:01.00]x\n");
    CHECK_EQ(radmusic::apply_offset(neg, 0), -1500);
}

TEST_CASE(test_lyrics_enhanced_words) {
    const Lyric l = parse_ok("[00:12.00]<00:12.00>Hel<00:12.30>lo <00:12.60>world\n");
    CHECK_EQ(l.lines.size(), static_cast<size_t>(1));
    const LyricLine& line = l.lines[0];
    CHECK_EQ(line.time_ms, 12000);
    CHECK_EQ(line.text, std::string("Hello world"));  // 逐字文本拼接
    CHECK_EQ(line.words.size(), static_cast<size_t>(3));
    CHECK_EQ(line.words[0].t_ms, 12000);
    CHECK_EQ(line.words[0].text, std::string("Hel"));
    CHECK_EQ(line.words[1].t_ms, 12300);
    CHECK_EQ(line.words[1].text, std::string("lo "));
    CHECK_EQ(line.words[2].t_ms, 12600);
    CHECK_EQ(line.words[2].text, std::string("world"));
    CHECK(line.has_time);

    // 无逐字标记时 words 为空
    const Lyric plain = parse_ok("[00:01.00]hello\n");
    CHECK(plain.lines[0].words.empty());
}

TEST_CASE(test_lyrics_plain_text_is_untimed) {
    const Lyric l = parse_ok("第一行\n\n第二行\n");
    CHECK_FALSE(l.timed);
    CHECK_EQ(l.format, std::string("txt"));
    CHECK_EQ(l.lines.size(), static_cast<size_t>(2));
    CHECK_EQ(l.lines[0].time_ms, 0);
    CHECK_EQ(l.lines[0].text, std::string("第一行"));
    CHECK_EQ(l.lines[1].text, std::string("第二行"));
    CHECK_FALSE(l.lines[0].has_time);
    CHECK_FALSE(l.lines[1].has_time);
}

TEST_CASE(test_lyrics_invalid_timestamp_line_skipped) {
    const Lyric l = parse_ok("[00:01.00]A\n[aa:bb]B\n[00:02.00]C\n[00:xx.yy]D\n[00:03.00]E\n");
    CHECK_EQ(l.lines.size(), static_cast<size_t>(3));
    CHECK_EQ(l.lines[0].text, std::string("A"));
    CHECK_EQ(l.lines[1].text, std::string("C"));
    CHECK_EQ(l.lines[2].text, std::string("E"));
    CHECK(l.timed);

    // 非法行不会让整体失败，也不影响后续行
    const Lyric only_bad = parse_ok("[qq:ww]没有合法行\n");
    CHECK(only_bad.empty());

    // 形如 [00:12:34]（冒号毫秒）与未闭合的 [00:12.3 都视为坏时间戳行：跳过，而不是降级为纯文本
    const Lyric weird = parse_ok("[00:01.00]A\n[00:02:34]B\n[00:03.30C\n[00:04.00]D\n");
    CHECK(weird.timed);
    CHECK_EQ(weird.lines.size(), static_cast<size_t>(2));
    CHECK_EQ(weird.lines[0].text, std::string("A"));
    CHECK_EQ(weird.lines[1].text, std::string("D"));

    // 真正不含时间戳的方括号内容仍是普通文本（如 [Chorus]）
    const Lyric chorus = parse_ok("[00:01.00][Chorus]唱\n");
    CHECK_EQ(chorus.lines.size(), static_cast<size_t>(1));
    CHECK_EQ(chorus.lines[0].time_ms, 1000);
    CHECK_EQ(chorus.lines[0].text, std::string("[Chorus]唱"));
}

// ---------------------------------------------------------------- 合并 / 定位

TEST_CASE(test_lyrics_merge_track_translation_and_romaji) {
    Lyric main = parse_ok("[00:10.00]Hello\n[00:20.00]World\n");
    const Lyric tr = parse_ok("[00:10.03]你好\n[00:20.20]世界\n");  // 差 30ms 命中；差 200ms 不命中
    CHECK_EQ(radmusic::merge_track(main, tr, 50, false), 1);
    CHECK_EQ(main.lines[0].tr, std::string("你好"));
    CHECK(main.lines[1].tr.empty());

    // 容差放大到 200ms 则命中
    Lyric main2 = parse_ok("[00:10.00]Hello\n[00:20.00]World\n");
    CHECK_EQ(radmusic::merge_track(main2, tr, 200, false), 2);
    CHECK_EQ(main2.lines[1].tr, std::string("世界"));

    // 罗马音轨道
    Lyric main3 = parse_ok("[00:10.00]Hello\n[00:20.00]World\n");
    const Lyric roma = parse_ok("[00:20.02]Wa-rudo\n");
    CHECK_EQ(radmusic::merge_track(main3, roma, 50, true), 1);
    CHECK_EQ(main3.lines[1].roma, std::string("Wa-rudo"));
    CHECK(main3.lines[0].roma.empty());

    // 完全对不上 -> 0
    Lyric main4 = parse_ok("[00:10.00]Hello\n");
    const Lyric far = parse_ok("[00:59.00]很远\n");
    CHECK_EQ(radmusic::merge_track(main4, far, 50, false), 0);
    CHECK(main4.lines[0].tr.empty());

    // 纯文本 extra 不参与合并
    Lyric main5 = parse_ok("[00:10.00]Hello\n");
    const Lyric plain = parse_ok("没有时间戳\n");
    CHECK_EQ(radmusic::merge_track(main5, plain, 50, false), 0);
}

TEST_CASE(test_lyrics_find_active_line_boundaries) {
    Lyric l = parse_ok("[00:10.00]A\n[00:20.00]B\n[00:30.00]C\n");

    CHECK_EQ(radmusic::find_active_line(l, 0), -1);      // 第一行之前
    CHECK_EQ(radmusic::find_active_line(l, 9999), -1);
    CHECK_EQ(radmusic::find_active_line(l, 10000), 0);   // 正好等于时间戳
    CHECK_EQ(radmusic::find_active_line(l, 15000), 0);
    CHECK_EQ(radmusic::find_active_line(l, 20000), 1);
    CHECK_EQ(radmusic::find_active_line(l, 29999), 1);
    CHECK_EQ(radmusic::find_active_line(l, 30000), 2);
    CHECK_EQ(radmusic::find_active_line(l, 999999), 2);  // 最后一行之后
    CHECK_EQ(radmusic::find_active_line(l, -5000), -1);

    // 正偏移：歌词提前 -> 比较时间 = position + offset
    Lyric early = parse_ok("[00:10.00]A\n[00:20.00]B\n");
    CHECK_EQ(radmusic::apply_offset(early, 2000), 2000);
    CHECK_EQ(radmusic::find_active_line(early, 7999), -1);
    CHECK_EQ(radmusic::find_active_line(early, 8000), 0);
    CHECK_EQ(radmusic::find_active_line(early, 18000), 1);

    // 负偏移：歌词延后
    Lyric late = parse_ok("[00:10.00]A\n[00:20.00]B\n");
    CHECK_EQ(radmusic::apply_offset(late, -2000), -2000);
    CHECK_EQ(radmusic::find_active_line(late, 11999), -1);
    CHECK_EQ(radmusic::find_active_line(late, 12000), 0);

    // 纯文本歌词不做高亮
    const Lyric plain = parse_ok("纯文本\n第二行\n");
    CHECK_EQ(radmusic::find_active_line(plain, 100000), -1);

    // 空歌词
    const Lyric empty = parse_ok("");
    CHECK_EQ(radmusic::find_active_line(empty, 1000), -1);
}

TEST_CASE(test_lyrics_normalize_sorts_and_dedupes) {
    Lyric l;
    LyricLine a;
    a.time_ms = 20000;
    a.text = "b";
    LyricLine b;
    b.time_ms = 10000;
    b.text = "a";
    LyricLine dup = b;
    dup.tr = "译文";
    LyricLine c;
    c.time_ms = 10000;
    c.text = "c";
    LyricLine plain;
    plain.time_ms = 0;
    plain.text = "占位";
    plain.has_time = false;
    l.lines = {a, b, dup, c, plain};
    l.timed = false;

    radmusic::normalize_lyric(l);

    CHECK_EQ(l.lines.size(), static_cast<size_t>(4));  // 相同 time_ms + text 去重
    CHECK_EQ(l.lines[0].text, std::string("占位"));
    CHECK_FALSE(l.lines[0].has_time);
    CHECK_EQ(l.lines[1].time_ms, 10000);
    CHECK_EQ(l.lines[1].text, std::string("a"));
    CHECK_EQ(l.lines[1].tr, std::string("译文"));  // 去重时保留可选字段
    CHECK_EQ(l.lines[2].text, std::string("c"));
    CHECK_EQ(l.lines[3].time_ms, 20000);
    CHECK(l.timed);  // 存在 has_time 行

    Lyric all_plain;
    LyricLine p1;
    p1.text = "一";
    p1.has_time = false;
    LyricLine p2;
    p2.text = "二";
    p2.has_time = false;
    all_plain.lines = {p1, p2};
    radmusic::normalize_lyric(all_plain);
    CHECK_EQ(all_plain.lines.size(), static_cast<size_t>(2));
    CHECK_FALSE(all_plain.timed);
}

// ---------------------------------------------------------------- 回环

TEST_CASE(test_lyrics_to_lrc_round_trip) {
    const std::string src = "[00:10.00]Hello\n[00:20.50]World\n[01:05.12]第三行\n";
    const Lyric a = parse_ok(src);
    const std::string out = radmusic::to_lrc(a);
    CHECK(out.find("[00:10.00]Hello") != std::string::npos);
    CHECK(out.find("[00:20.50]World") != std::string::npos);
    CHECK(out.find("[01:05.12]第三行") != std::string::npos);

    const Lyric b = parse_ok(out);
    CHECK_EQ(b.lines.size(), a.lines.size());
    for (size_t i = 0; i < a.lines.size(); ++i) {
        CHECK_EQ(b.lines[i].time_ms, a.lines[i].time_ms);
        CHECK_EQ(b.lines[i].text, a.lines[i].text);
        CHECK_EQ(b.lines[i].has_time, a.lines[i].has_time);
    }

    // 偏移标签回环
    const Lyric off = parse_ok("[offset:-1500]\n[00:01.00]a\n");
    const std::string off_text = radmusic::to_lrc(off);
    CHECK_EQ(off_text.find("[offset:-1500]"), static_cast<size_t>(0));
    CHECK_EQ(radmusic::parse_lrc(off_text).value().offset_ms, -1500);
    CHECK(radmusic::parse_lrc(off_text).value().has_offset_tag);

    Lyric pos;
    pos.offset_ms = 750;
    pos.format = "lrc";
    pos.lines.push_back(parse_ok("[00:01.00]a\n").lines[0]);
    CHECK_EQ(radmusic::parse_lrc(radmusic::to_lrc(pos)).value().offset_ms, 750);

    // 增强逐字回环
    const Lyric w = parse_ok("[00:12.00]<00:12.00>Hel<00:12.30>lo\n");
    const std::string w_text = radmusic::to_lrc(w, true);
    CHECK(w_text.find("[00:12.00]<00:12.00>Hel<00:12.30>lo") != std::string::npos);
    const Lyric w2 = parse_ok(w_text);
    CHECK_EQ(w2.lines[0].time_ms, 12000);
    CHECK_EQ(w2.lines[0].text, std::string("Hello"));
    CHECK_EQ(w2.lines[0].words.size(), static_cast<size_t>(2));
    CHECK_EQ(w2.lines[0].words[0].t_ms, 12000);
    CHECK_EQ(w2.lines[0].words[1].t_ms, 12300);
    CHECK_EQ(w2.lines[0].words[1].text, std::string("lo"));

    // 纯文本导出：只输出文本行
    const Lyric plain = parse_ok("第一行\n第二行\n");
    CHECK_EQ(radmusic::to_lrc(plain), std::string("第一行\n第二行\n"));
}

TEST_CASE(test_lyrics_json_round_trip) {
    const std::string src = "[00:10.00]Hello\n[00:20.00]<00:20.00>Wo<00:20.50>rld\n";
    Lyric l = parse_ok(src);
    l.sha = "9f2cabcd";
    l.source = "radish";
    l.lines[0].tr = "你好";
    l.lines[0].roma = "ni hao";

    const std::string dumped = l.to_json(true, true, true).dump();
    const radmusic::json::Value v = radmusic::json::Value::parse(dumped);
    CHECK(v.is_object());

    const Lyric back = Lyric::from_json(v);
    CHECK_EQ(back.sha, std::string("9f2cabcd"));
    CHECK_EQ(back.source, std::string("radish"));
    CHECK_EQ(back.format, std::string("lrc"));
    CHECK(back.timed);
    CHECK_EQ(back.offset_ms, 0);
    CHECK_EQ(back.lines.size(), static_cast<size_t>(2));
    CHECK_EQ(back.lines[0].time_ms, 10000);
    CHECK_EQ(back.lines[0].text, std::string("Hello"));
    CHECK_EQ(back.lines[0].tr, std::string("你好"));
    CHECK_EQ(back.lines[0].roma, std::string("ni hao"));
    CHECK(back.lines[0].has_time);
    CHECK_EQ(back.lines[1].time_ms, 20000);
    CHECK_EQ(back.lines[1].text, std::string("World"));
    CHECK_EQ(back.lines[1].words.size(), static_cast<size_t>(2));
    CHECK_EQ(back.lines[1].words[0].t_ms, 20000);
    CHECK_EQ(back.lines[1].words[0].text, std::string("Wo"));
    CHECK_EQ(back.lines[1].words[1].t_ms, 20500);
    CHECK_EQ(back.lines[1].words[1].text, std::string("rld"));

    // include_* 开关
    const radmusic::json::Value thin = l.to_json(false, false, false);
    const radmusic::json::Value& first = thin["lines"][0];
    CHECK(first.contains("time_ms"));
    CHECK(first.contains("text"));
    CHECK_FALSE(first.contains("tr"));
    CHECK_FALSE(first.contains("roma"));
    CHECK_FALSE(thin["lines"][1].contains("words"));
    CHECK(l.lines[1].to_json(true).contains("words"));
    CHECK_FALSE(l.lines[1].to_json(false).contains("words"));

    // 纯文本歌词的 has_time 也要能回环
    const Lyric plain = parse_ok("第一行\n第二行\n");
    const Lyric plain_back = Lyric::from_json(plain.to_json(true, true, true));
    CHECK_FALSE(plain_back.timed);
    CHECK_EQ(plain_back.format, std::string("txt"));
    CHECK_EQ(plain_back.lines.size(), static_cast<size_t>(2));
    CHECK_FALSE(plain_back.lines[0].has_time);

    // 缺失字段宽容
    const radmusic::json::Value minimal =
        radmusic::json::Value::parse("{\"lines\":[{\"time_ms\":5,\"text\":\"x\"}]}");
    const Lyric m = Lyric::from_json(minimal);
    CHECK_EQ(m.format, std::string("lrc"));
    CHECK(m.timed);
    CHECK(m.sha.empty());
    CHECK_EQ(m.lines.size(), static_cast<size_t>(1));
    CHECK_EQ(m.lines[0].time_ms, 5);
    CHECK_EQ(m.lines[0].text, std::string("x"));
    CHECK(m.lines[0].has_time);
    CHECK(m.lines[0].tr.empty());

    // 非对象输入 -> 默认歌词
    const Lyric none = Lyric::from_json(radmusic::json::Value::parse("null"));
    CHECK(none.empty());
    const LyricLine none_line = LyricLine::from_json(radmusic::json::Value::parse("42"));
    CHECK(none_line.has_time);
    CHECK_EQ(none_line.time_ms, 0);
}

// ---------------------------------------------------------------- 空输入

TEST_CASE(test_lyrics_empty_inputs) {
    const Result<Lyric> a = radmusic::parse_lrc("");
    CHECK(a.ok());
    CHECK(a.value().empty());
    CHECK_EQ(a.value().lines.size(), static_cast<size_t>(0));

    const Result<Lyric> b = radmusic::parse_lrc("\n\n\r\n");
    CHECK(b.ok());
    CHECK(b.value().empty());

    const Result<Lyric> c = radmusic::parse_lrc("   \t  \n \t\n  ");
    CHECK(c.ok());
    CHECK(c.value().empty());

    // 只有标签、没有文本与时间戳
    const Result<Lyric> d = radmusic::parse_lrc("[ti:标题]\n[ar:歌手]\n[al:专辑]\n");
    CHECK(d.ok());
    CHECK(d.value().empty());
    CHECK_FALSE(d.value().has_offset_tag);

    // 空歌词导出为空字符串
    CHECK(radmusic::to_lrc(a.value()).empty());
}
