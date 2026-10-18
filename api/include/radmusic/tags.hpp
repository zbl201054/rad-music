// Rad music API - 本地音乐标签解析与音频流探测（对应 Readme.md 1.10）
//
// 设计要点：
//  * 只读文件的必要片段：先读头部若干 KiB 判断容器，再按需 seek，绝不整文件读入内存；
//  * 内嵌封面只读一次并限制大小（超过上限则跳过，仍视为解析成功）；
//  * 所有边界（长度/偏移/循环）都做防御性检查，解析失败返回 Result 错误而不抛异常；
//  * 本模块只依赖标准库（<fstream> 等），零第三方依赖。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "radmusic/error.hpp"
#include "radmusic/model.hpp"
#include "radmusic/quality.hpp"

namespace radmusic {

// 支持的音频容器（对应 Readme 1.10 的扫描候选）
enum class AudioContainer { Unknown, Mp3, Flac, M4a, Wav, Ape, Ogg, Opus, Aac };

// "mp3" / "flac" / "m4a" / "wav" / "ape" / "ogg" / "opus" / "aac"，未知返回 "unknown"
const char* audio_container_id(AudioContainer c);
// 由扩展名（忽略大小写与点号）判断容器；未知扩展名 → Unknown
AudioContainer audio_container_from_extension(std::string_view path);
// 支持返回 true；未知/不支持 → false（调用方返回 1025 unsupported format）
bool audio_container_supported(AudioContainer c);

struct TagData {
    std::string title;
    std::string artist;
    std::string album;
    int track_no = 0;
    int duration_seconds = 0;          // 0 = 未知
    std::vector<uint8_t> cover;        // 内嵌封面（可为空）
    std::string cover_mime;            // image/jpeg 等
    std::string tag_source;            // "id3v2" / "vorbis" / "mp4" / "ape" / "wav" / "filename"
    std::string lyrics_text;           // 内嵌歌词（USLT/LYRICS），可为空
    std::string genre;
};

struct AudioStreamInfo {
    std::string codec;                 // flac/mp3/aac/alac/opus/vorbis/pcm
    bool lossless = false;
    std::optional<int> bitrate;        // kbps
    std::optional<int> sample_rate;    // Hz
    std::optional<int> bit_depth;      // 无损才有
    std::optional<int> channels;
    long long duration_ms = 0;
    long long file_size = 0;
};

// 解析标签；文件不可读 → FileNotFound；容器不支持 → UnsupportedFormat；
// 容器支持但标签缺失/损坏 → TagParseFailed（非致命，调用方回退文件名）
Result<TagData> read_tags(const std::string& path);
// 探测音频流参数（用于 base.json.quality 与时长）
Result<AudioStreamInfo> probe_audio(const std::string& path);
// 由文件名按模板解析（默认 "{artist} - {title}"；找不到分隔符则整串作为 title，tag_source="filename"）
TagData tags_from_filename(const std::string& path, std::string_view filename_template = "{artist} - {title}");
// 由流信息生成 Quality（lossless/采样率/位深/码率 → level 用 infer_quality_level）
Quality quality_from_stream(const AudioStreamInfo& info);

}  // namespace radmusic
