// Rad music API - 本地音乐标签解析与音频流探测实现（零第三方依赖）
//
// 对应 Readme.md 1.10：优先读内嵌标签（MP3 ID3v2 / FLAC Vorbis Comment /
// M4A-MP4 atoms / WAV INFO / APE），缺失或损坏时由调用方回退文件名模板；
// 同时提供音频流参数（编码/码率/采样率/位深/声道/时长）用于 base.json.quality。
//
// 实现约束：
//  * 只读必要片段：先读头部若干 KiB 嗅探容器，再按需 seek；绝不整文件读入内存；
//  * 内嵌封面只读一次且限制 8 MiB，超限则跳过并仍然成功；
//  * 所有偏移/长度都做边界检查，所有循环都有步进保证（不会越界读、不会死循环）；
//  * 解析失败一律返回 Result 错误，绝不抛异常。
#include "radmusic/tags.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace radmusic {
namespace {

// ---------------------------------------------------------------- 常量上限

constexpr size_t kHeadBytes = 64u * 1024u;           // 头部嗅探缓冲
constexpr size_t kMaxCover = 8u * 1024u * 1024u;     // 封面大小上限（超出跳过）
constexpr size_t kMaxId3Body = 16u * 1024u * 1024u;  // ID3v2 标签体读取上限
constexpr size_t kMaxMoov = 64u * 1024u * 1024u;     // moov box 读取上限
constexpr size_t kMaxBlock = 16u * 1024u * 1024u;    // 单个元数据块上限
constexpr size_t kMaxChunk = 8u * 1024u * 1024u;     // WAV / LIST 子块上限
constexpr int kMaxBlocks = 1 << 16;                   // 元数据块数量上限（防死循环）
constexpr int kMaxItems = 1 << 20;                    // 条目数量上限（防死循环）

// ---------------------------------------------------------------- 基础工具

std::string to_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}

bool is_space_char(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

std::string trim(std::string s) {
    size_t b = 0;
    while (b < s.size() && is_space_char(s[b])) ++b;
    size_t e = s.size();
    while (e > b && is_space_char(s[e - 1])) --e;
    return s.substr(b, e - b);
}

// 严格 UTF-8 校验（用于区分 UTF-8 与本地代码页文本）
bool is_valid_utf8(const uint8_t* p, size_t n) {
    size_t i = 0;
    while (i < n) {
        const uint8_t c = p[i];
        if (c < 0x80) {
            ++i;
            continue;
        }
        size_t need = 0;
        uint32_t cp = 0;
        if ((c & 0xE0u) == 0xC0u) {
            need = 1;
            cp = c & 0x1Fu;
            if (cp < 2) return false;  // 过长编码
        } else if ((c & 0xF0u) == 0xE0u) {
            need = 2;
            cp = c & 0x0Fu;
        } else if ((c & 0xF8u) == 0xF0u) {
            need = 3;
            cp = c & 0x07u;
        } else {
            return false;
        }
        if (need > n - i - 1) return false;
        for (size_t k = 1; k <= need; ++k) {
            const uint8_t cc = p[i + k];
            if ((cc & 0xC0u) != 0x80u) return false;
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (need == 2 && cp < 0x800u) return false;
        if (need == 3 && (cp < 0x10000u || cp > 0x10FFFFu)) return false;
        if (cp >= 0xD800u && cp <= 0xDFFFu) return false;  // 代理区非法
        i += need + 1;
    }
    return true;
}

std::string latin1_to_utf8(const uint8_t* p, size_t n) {
    std::string out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const uint8_t c = p[i];
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back(static_cast<char>(0xC0u | (c >> 6)));
            out.push_back(static_cast<char>(0x80u | (c & 0x3Fu)));
        }
    }
    return out;
}

void append_cp(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        append_cp(out, 0xFFFDu);
    }
}

// UTF-16 → UTF-8；allow_bom=false 时忽略 BOM 检测并强制按 big_endian 解释
std::string utf16_to_utf8(const uint8_t* p, size_t n, bool big_endian, bool allow_bom) {
    std::string out;
    size_t i = 0;
    if (allow_bom && n >= 2) {
        if (p[0] == 0xFF && p[1] == 0xFE) {
            big_endian = false;
            i = 2;
        } else if (p[0] == 0xFE && p[1] == 0xFF) {
            big_endian = true;
            i = 2;
        }
    }
    while (i + 1 < n) {
        const uint32_t u = big_endian ? (uint32_t)((p[i] << 8) | p[i + 1])
                                      : (uint32_t)((p[i + 1] << 8) | p[i]);
        i += 2;
        if (u == 0) break;  // 字符串结束
        if (u >= 0xD800u && u <= 0xDBFFu) {
            if (i + 1 < n) {
                const uint32_t lo = big_endian ? (uint32_t)((p[i] << 8) | p[i + 1])
                                               : (uint32_t)((p[i + 1] << 8) | p[i]);
                if (lo >= 0xDC00u && lo <= 0xDFFFu) {
                    i += 2;
                    append_cp(out, 0x10000u + ((u - 0xD800u) << 10) + (lo - 0xDC00u));
                    continue;
                }
            }
            append_cp(out, 0xFFFDu);
            continue;
        }
        if (u >= 0xDC00u && u <= 0xDFFFu) {
            append_cp(out, 0xFFFDu);
            continue;
        }
        append_cp(out, u);
    }
    return out;
}

// 按“UTF-8 优先、否则 Latin-1”解码裸字节（WAV/APE 等无编码标记的文本）
std::string decode_bytes(const uint8_t* p, size_t n) {
    size_t len = n;
    for (size_t i = 0; i < n; ++i) {
        if (p[i] == 0) {
            len = i;
            break;
        }
    }
    if (len == 0) return std::string();
    if (is_valid_utf8(p, len)) return std::string(reinterpret_cast<const char*>(p), len);
    return latin1_to_utf8(p, len);
}

// ID3v2 文本编码：0=ISO-8859-1，1=UTF-16+BOM，2=UTF-16BE，3=UTF-8
std::string decode_id3_text(const uint8_t* p, size_t n, uint8_t encoding) {
    if (n == 0) return std::string();
    switch (encoding) {
        case 0: {
            size_t len = n;
            for (size_t i = 0; i < n; ++i) {
                if (p[i] == 0) {
                    len = i;
                    break;
                }
            }
            return latin1_to_utf8(p, len);
        }
        case 1:
            return utf16_to_utf8(p, n, /*big_endian=*/true, /*allow_bom=*/true);
        case 2:
            return utf16_to_utf8(p, n, /*big_endian=*/true, /*allow_bom=*/false);
        case 3: {
            size_t len = n;
            for (size_t i = 0; i < n; ++i) {
                if (p[i] == 0) {
                    len = i;
                    break;
                }
            }
            if (is_valid_utf8(p, len)) return std::string(reinterpret_cast<const char*>(p), len);
            return latin1_to_utf8(p, len);  // 宽容：非法 UTF-8 按 Latin-1 处理
        }
        default: {
            size_t len = n;
            for (size_t i = 0; i < n; ++i) {
                if (p[i] == 0) {
                    len = i;
                    break;
                }
            }
            return latin1_to_utf8(p, len);
        }
    }
}

// 有界缓冲区访问器：调用方必须先 has() 再用 uXX()（避免越界读）
class Buf {
public:
    Buf() = default;
    Buf(const uint8_t* d, size_t n) : d_(d), n_(n) {}

    size_t size() const { return n_; }
    bool empty() const { return n_ == 0; }
    bool has(size_t off, size_t len) const { return off <= n_ && len <= n_ - off; }
    const uint8_t* at(size_t off) const { return d_ + off; }

    uint8_t u8(size_t o) const { return d_[o]; }
    uint16_t u16be(size_t o) const { return (uint16_t)((d_[o] << 8) | d_[o + 1]); }
    uint16_t u16le(size_t o) const { return (uint16_t)((d_[o + 1] << 8) | d_[o]); }
    uint32_t u24be(size_t o) const {
        return ((uint32_t)d_[o] << 16) | ((uint32_t)d_[o + 1] << 8) | (uint32_t)d_[o + 2];
    }
    uint32_t u24le(size_t o) const {
        return ((uint32_t)d_[o + 2] << 16) | ((uint32_t)d_[o + 1] << 8) | (uint32_t)d_[o];
    }
    uint32_t u32be(size_t o) const {
        return ((uint32_t)d_[o] << 24) | ((uint32_t)d_[o + 1] << 16) | ((uint32_t)d_[o + 2] << 8) |
               (uint32_t)d_[o + 3];
    }
    uint32_t u32le(size_t o) const {
        return ((uint32_t)d_[o + 3] << 24) | ((uint32_t)d_[o + 2] << 16) | ((uint32_t)d_[o + 1] << 8) |
               (uint32_t)d_[o];
    }
    uint64_t u64be(size_t o) const {
        return ((uint64_t)u32be(o) << 32) | (uint64_t)u32be(o + 4);
    }
    uint64_t u64le(size_t o) const {
        return ((uint64_t)u32le(o + 4) << 32) | (uint64_t)u32le(o);
    }
    std::string str(size_t o, size_t len) const {
        return std::string(reinterpret_cast<const char*>(d_ + o), len);
    }

private:
    const uint8_t* d_ = nullptr;
    size_t n_ = 0;
};

bool match4(const Buf& b, size_t off, const char* four) {
    return b.has(off, 4) && std::memcmp(b.at(off), four, 4) == 0;
}

// 文件读取器：只做 seek + 局部读，绝不整文件读入
class FileReader {
public:
    bool open(const std::string& path) {
        // path 为 UTF-8 字节：必须经 std::filesystem::path 打开，Windows 下才会走宽字符 API，
        // 否则窄编码 fopen 会因系统 ANSI 代码页无法表示中文等字符而失败。
        stream_.open(std::filesystem::path(path), std::ios::binary);
        if (!stream_.is_open()) return false;
        stream_.seekg(0, std::ios::end);
        const std::streamoff end = stream_.tellg();
        stream_.clear();
        size_ = end > 0 ? static_cast<long long>(end) : 0;
        return true;
    }

    long long size() const { return size_; }

    // 返回实际读到的字节数（文件尾部不足时截断）
    size_t read_some(long long off, void* buf, size_t n) {
        if (n == 0 || off < 0 || off >= size_) return 0;
        size_t want = n;
        if (static_cast<long long>(want) > size_ - off) want = static_cast<size_t>(size_ - off);
        stream_.clear();
        stream_.seekg(static_cast<std::streamoff>(off), std::ios::beg);
        if (!stream_.good()) return 0;
        stream_.read(reinterpret_cast<char*>(buf), static_cast<std::streamsize>(want));
        const std::streamsize got = stream_.gcount();
        return got > 0 ? static_cast<size_t>(got) : 0;
    }

    bool read_exact(long long off, void* buf, size_t n) { return read_some(off, buf, n) == n; }

    std::vector<uint8_t> read_vec(long long off, size_t n) {
        std::vector<uint8_t> out(n);
        if (n == 0) return out;
        out.resize(read_some(off, out.data(), n));
        return out;
    }

private:
    std::ifstream stream_;
    long long size_ = 0;
};

// ---------------------------------------------------------------- 容器判定

AudioContainer sniff(const Buf& head, AudioContainer by_ext) {
    if (head.has(0, 3) && head.str(0, 3) == "ID3") return AudioContainer::Mp3;
    if (head.has(0, 4) && head.str(0, 4) == "fLaC") return AudioContainer::Flac;
    if (head.has(0, 12) && head.str(0, 4) == "RIFF" && head.str(8, 4) == "WAVE")
        return AudioContainer::Wav;
    if (head.has(0, 4) && head.str(0, 4) == "OggS") {
        const size_t lim = std::min<size_t>(head.size(), 512);
        for (size_t i = 0; i + 8 <= lim; ++i) {
            if (head.str(i, 8) == "OpusHead") return AudioContainer::Opus;
        }
        return AudioContainer::Ogg;
    }
    if (head.has(0, 12) && head.str(4, 4) == "ftyp") return AudioContainer::M4a;
    if (head.has(0, 4) && head.str(0, 4) == "MAC ") return AudioContainer::Ape;
    if (head.has(0, 7) && head.u8(0) == 0xFF && (head.u8(1) & 0xF6u) == 0xF0u) {
        // ADTS：layer 必须为 00，且帧长字段合理
        const size_t frame_len = ((size_t)(head.u8(3) & 0x03u) << 11) | ((size_t)head.u8(4) << 3) |
                                 ((size_t)head.u8(5) >> 5);
        if (frame_len >= 7) return AudioContainer::Aac;
    }
    if (head.has(0, 2) && head.u8(0) == 0xFF && (head.u8(1) & 0xE0u) == 0xE0u)
        return AudioContainer::Mp3;  // MPEG 帧同步（Layer I/II/III）
    return by_ext;
}

// ---------------------------------------------------------------- MP3 / MPEG

struct Mp3Frame {
    bool valid = false;
    int version = 1;       // 1 / 2 / 25（2.5）
    int layer = 3;         // 1 / 2 / 3
    int bitrate = 0;       // kbps
    int sample_rate = 0;   // Hz
    int channels = 0;      // 1 / 2
    int samples_per_frame = 0;
    int frame_length = 0;  // 字节
    int padding = 0;
    bool crc = false;
};

bool parse_mpeg_header(uint32_t h, Mp3Frame& out) {
    if ((h & 0xFFE00000u) != 0xFFE00000u) return false;
    const int ver_bits = (h >> 19) & 0x3;    // 0=2.5, 1=保留, 2=2, 3=1
    const int layer_bits = (h >> 17) & 0x3;  // 0=保留, 1=III, 2=II, 3=I
    const int br_idx = (h >> 12) & 0xF;
    const int sr_idx = (h >> 10) & 0x3;
    const int padding = (h >> 9) & 0x1;
    const int chan_mode = (h >> 6) & 0x3;
    if (ver_bits == 1 || layer_bits == 0) return false;
    if (br_idx == 0 || br_idx == 15 || sr_idx == 3) return false;

    static const int kBrV1L1[16] = {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0};
    static const int kBrV1L2[16] = {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0};
    static const int kBrV1L3[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    static const int kBrV2L1[16] = {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0};
    static const int kBrV2L23[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
    static const int kSrV1[3] = {44100, 48000, 32000};
    static const int kSrV2[3] = {22050, 24000, 16000};
    static const int kSrV25[3] = {11025, 12000, 8000};

    Mp3Frame f;
    f.version = (ver_bits == 3) ? 1 : (ver_bits == 2 ? 2 : 25);
    f.layer = (layer_bits == 3) ? 1 : (layer_bits == 2 ? 2 : 3);
    f.padding = padding;
    f.crc = ((h >> 16) & 0x1) == 0;
    f.channels = (chan_mode == 3) ? 1 : 2;

    if (f.version == 1) {
        f.bitrate = (f.layer == 1) ? kBrV1L1[br_idx] : (f.layer == 2 ? kBrV1L2[br_idx] : kBrV1L3[br_idx]);
        f.sample_rate = kSrV1[sr_idx];
    } else {
        f.bitrate = (f.layer == 1) ? kBrV2L1[br_idx] : kBrV2L23[br_idx];
        f.sample_rate = (f.version == 2) ? kSrV2[sr_idx] : kSrV25[sr_idx];
    }
    if (f.bitrate <= 0 || f.sample_rate <= 0) return false;

    if (f.layer == 1) {
        f.samples_per_frame = 384;
    } else if (f.layer == 2) {
        f.samples_per_frame = 1152;
    } else {
        f.samples_per_frame = (f.version == 1) ? 1152 : 576;
    }
    const int slot = (f.layer == 1) ? 4 : 1;
    f.frame_length = (f.samples_per_frame / 8) * f.bitrate * 1000 / f.sample_rate + padding * slot;
    if (f.frame_length < 4) return false;
    f.valid = true;
    out = f;
    return true;
}

// 在缓冲区内寻找第一个有效帧；用“下一帧头部一致”降低误判
bool find_mpeg_frame(const Buf& b, size_t start, Mp3Frame& out, size_t& at) {
    for (size_t i = start; i + 4 <= b.size(); ++i) {
        Mp3Frame f;
        if (!parse_mpeg_header(b.u32be(i), f)) continue;
        const size_t next = i + (size_t)f.frame_length;
        if (next + 4 <= b.size()) {
            Mp3Frame g;
            if (!parse_mpeg_header(b.u32be(next), g)) continue;
            if (g.layer != f.layer || g.sample_rate != f.sample_rate || g.version != f.version) continue;
        }
        out = f;
        at = i;
        return true;
    }
    return false;
}

struct Mp3Stream {
    bool ok = false;
    Mp3Frame frame;
    long long audio_offset = 0;
    long long duration_ms = 0;
    int bitrate = 0;  // kbps（有 VBR 头时给平均值）
};

Mp3Stream probe_mp3_stream(FileReader& f, long long audio_offset) {
    Mp3Stream out;
    if (audio_offset < 0 || audio_offset >= f.size()) return out;
    const size_t want = static_cast<size_t>(
        std::min<long long>(f.size() - audio_offset, static_cast<long long>(512 * 1024)));
    std::vector<uint8_t> buf = f.read_vec(audio_offset, want);
    const Buf b(buf.data(), buf.size());
    Mp3Frame frame;
    size_t at = 0;
    if (!find_mpeg_frame(b, 0, frame, at)) return out;

    out.ok = true;
    out.frame = frame;
    out.audio_offset = audio_offset + static_cast<long long>(at);
    out.bitrate = frame.bitrate;

    long long frames = 0;
    bool has_frames = false;

    // Xing / Info（VBR 头）：帧头之后、side info 之后
    const size_t side = (frame.version == 1) ? (frame.channels == 1 ? 17u : 32u)
                                             : (frame.channels == 1 ? 9u : 17u);
    const size_t xing_off = at + 4 + side;
    if (b.has(xing_off, 8) && (b.str(xing_off, 4) == "Xing" || b.str(xing_off, 4) == "Info")) {
        const uint32_t flags = b.u32be(xing_off + 4);
        if ((flags & 0x1u) && b.has(xing_off + 8, 4)) {
            frames = static_cast<long long>(b.u32be(xing_off + 8));
            has_frames = frames > 0;
        }
    }
    // VBRI（Fraunhofer）：帧头后 32 字节
    const size_t vbri_off = at + 4 + 32;
    if (!has_frames && b.has(vbri_off, 26) && b.str(vbri_off, 4) == "VBRI") {
        frames = static_cast<long long>(b.u32be(vbri_off + 14));
        has_frames = frames > 0;
    }

    if (has_frames) {
        out.duration_ms =
            frames * static_cast<long long>(frame.samples_per_frame) * 1000 / frame.sample_rate;
        const long long bytes = f.size() - out.audio_offset;
        if (out.duration_ms > 0) {
            const long long kbps = bytes * 8 / out.duration_ms;
            if (kbps > 0 && kbps < 100000) out.bitrate = static_cast<int>(kbps);
        }
    } else {
        // CBR 估算：bytes * 8 / kbps = ms
        const long long bytes = f.size() - out.audio_offset;
        if (bytes > 0) out.duration_ms = bytes * 8 / frame.bitrate;
    }
    return out;
}

// ---------------------------------------------------------------- ID3v2

size_t syncsafe(const Buf& b, size_t off) {
    return ((size_t)(b.u8(off) & 0x7Fu) << 21) | ((size_t)(b.u8(off + 1) & 0x7Fu) << 14) |
           ((size_t)(b.u8(off + 2) & 0x7Fu) << 7) | (size_t)(b.u8(off + 3) & 0x7Fu);
}

std::vector<uint8_t> deunsynchronise(const uint8_t* p, size_t n) {
    std::vector<uint8_t> out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        out.push_back(p[i]);
        if (p[i] == 0xFF && i + 1 < n && p[i + 1] == 0x00) ++i;  // 丢弃插入的 0x00
    }
    return out;
}

int parse_int_prefix(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && is_space_char(s[i])) ++i;
    long long v = 0;
    bool any = false;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        v = v * 10 + (s[i] - '0');
        if (v > 100000) {
            v = 100000;
            break;
        }
        any = true;
        ++i;
    }
    return any ? static_cast<int>(v) : 0;
}

// ID3v1/ID3v2 标准流派表（TCON 里 "(17)" 这类数字引用）
const char* genre_by_index(int idx) {
    static const char* kGenres[] = {
        "Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge", "Hip-Hop", "Jazz",
        "Metal", "New Age", "Oldies", "Other", "Pop", "R&B", "Rap", "Reggae", "Rock", "Techno",
        "Industrial", "Alternative", "Ska", "Death Metal", "Pranks", "Soundtrack", "Euro-Techno",
        "Ambient", "Trip-Hop", "Vocal", "Jazz+Funk", "Fusion", "Trance", "Classical", "Instrumental",
        "Acid", "House", "Game", "Sound Clip", "Gospel", "Noise", "AlternRock", "Bass", "Soul", "Punk",
        "Space", "Meditative", "Instrumental Pop", "Instrumental Rock", "Ethnic", "Gothic", "Darkwave",
        "Techno-Industrial", "Electronic", "Pop-Folk", "Eurodance", "Dream", "Southern Rock", "Comedy",
        "Cult", "Gangsta", "Top 40", "Christian Rap", "Pop/Funk", "Jungle", "Native American",
        "Cabaret", "New Wave", "Psychadelic", "Rave", "Showtunes", "Trailer", "Lo-Fi", "Tribal",
        "Acid Punk", "Acid Jazz", "Polka", "Retro", "Musical", "Rock & Roll", "Hard Rock", "Folk",
        "Folk-Rock", "National Folk", "Swing", "Fast Fusion", "Bebob", "Latin", "Revival", "Celtic",
        "Bluegrass", "Avantgarde", "Gothic Rock", "Progressive Rock", "Psychedelic Rock",
        "Symphonic Rock", "Slow Rock", "Big Band", "Chorus", "Easy Listening", "Acoustic", "Humour",
        "Speech", "Chanson", "Opera", "Chamber Music", "Sonata", "Symphony", "Booty Bass", "Primus",
        "Porn Groove", "Satire", "Slow Jam", "Club", "Tango", "Samba", "Folklore", "Ballad",
        "Power Ballad", "Rhythmic Soul", "Freestyle", "Duet", "Punk Rock", "Drum Solo", "A capella",
        "Euro-House", "Dance Hall"};
    const int count = static_cast<int>(sizeof(kGenres) / sizeof(kGenres[0]));
    if (idx < 0 || idx >= count) return nullptr;
    return kGenres[idx];
}

std::string normalise_genre(std::string value) {
    value = trim(value);
    // "(17)" / "(17)Rock" / "17" 三种常见形态
    size_t i = 0;
    if (i < value.size() && value[i] == '(') {
        const size_t close = value.find(')', i + 1);
        if (close != std::string::npos) {
            const std::string num = value.substr(i + 1, close - i - 1);
            if (!num.empty() && num.find_first_not_of("0123456789") == std::string::npos) {
                const char* name = genre_by_index(std::atoi(num.c_str()));
                std::string rest = trim(value.substr(close + 1));
                if (name && !rest.empty()) return std::string(name) + "/" + rest;
                if (name) return name;
                return rest;
            }
        }
    }
    if (!value.empty() && value.find_first_not_of("0123456789") == std::string::npos) {
        const char* name = genre_by_index(std::atoi(value.c_str()));
        if (name) return name;
    }
    return value;
}

std::string sniff_image_mime(const uint8_t* p, size_t n) {
    if (n >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) return "image/jpeg";
    if (n >= 8 && p[0] == 0x89 && p[1] == 'P' && p[2] == 'N' && p[3] == 'G') return "image/png";
    if (n >= 6 && std::memcmp(p, "GIF8", 4) == 0) return "image/gif";
    if (n >= 2 && p[0] == 'B' && p[1] == 'M') return "image/bmp";
    if (n >= 12 && std::memcmp(p, "RIFF", 4) == 0 && std::memcmp(p + 8, "WEBP", 4) == 0)
        return "image/webp";
    return std::string();
}

// 跳过“按文本编码终止的空串”（描述/文件名），返回下一字节偏移；越界返回 n
size_t skip_encoded_string(const uint8_t* p, size_t n, size_t off, uint8_t encoding) {
    if (off >= n) return n;
    if (encoding == 1 || encoding == 2) {
        size_t i = off;
        while (i + 1 < n) {
            if (p[i] == 0 && p[i + 1] == 0) return i + 2;
            i += 2;
        }
        return n;
    }
    size_t i = off;
    while (i < n && p[i] != 0) ++i;
    return i + 1 <= n ? i + 1 : n;
}

struct Id3Tag {
    TagData tags;
    bool meaningful = false;
};

void id3_set_text(TagData& t, const std::string& id, const std::string& value, bool& meaningful) {
    const std::string v = trim(value);
    if (v.empty()) return;
    meaningful = true;
    if (id == "TIT2" || id == "TT2") {
        t.title = v;
    } else if (id == "TPE1" || id == "TP1") {
        t.artist = v;
    } else if (id == "TALB" || id == "TAL") {
        t.album = v;
    } else if (id == "TRCK" || id == "TRK") {
        const int n = parse_int_prefix(v);
        if (n > 0) t.track_no = n;
    } else if (id == "TCON" || id == "TCO") {
        t.genre = normalise_genre(v);
    }
}

// APIC（v2.3/2.4）与 PIC（v2.2）共用一个解析路径
bool id3_parse_picture(const uint8_t* p, size_t n, bool v22, TagData& t) {
    if (n < 4) return false;
    const uint8_t encoding = p[0];
    size_t i = 1;
    std::string mime;
    if (v22) {
        if (i + 3 > n) return false;
        const std::string fmt = std::string(reinterpret_cast<const char*>(p + i), 3);
        i += 3;
        mime = to_lower(fmt) == "png" ? "image/png" : (to_lower(fmt) == "jpg" ? "image/jpeg" : std::string());
    } else {
        const size_t start = i;
        while (i < n && p[i] != 0) ++i;
        if (i >= n) return false;
        mime = to_lower(decode_bytes(p + start, i - start));
        ++i;  // 跳过 NUL
    }
    if (i >= n) return false;
    ++i;  // 图片类型（1 字节）
    i = skip_encoded_string(p, n, i, encoding);
    if (i >= n) return false;
    const size_t data_len = n - i;
    if (data_len == 0) return false;
    if (data_len > kMaxCover) return false;  // 过大：跳过封面，仍算解析成功
    if (mime.empty() || mime.rfind("image/", 0) != 0) mime = sniff_image_mime(p + i, data_len);
    if (mime.empty()) mime = "image/jpeg";
    t.cover.assign(p + i, p + n);
    t.cover_mime = mime;
    return true;
}

bool id3_parse_lyrics(const uint8_t* p, size_t n, TagData& t) {
    if (n < 5) return false;
    const uint8_t encoding = p[0];
    size_t i = 4;  // 编码(1) + 语言(3)
    i = skip_encoded_string(p, n, i, encoding);
    if (i >= n) return false;
    std::string text = decode_id3_text(p + i, n - i, encoding);
    text = trim(text);
    if (text.empty()) return false;
    t.lyrics_text = text;
    return true;
}

struct Id3v2Result {
    bool ok = false;              // 是否有可用标签
    long long audio_offset = 0;   // 音频数据起点
    TagData tags;
};

Id3v2Result parse_id3v2(FileReader& f, const Buf& head) {
    Id3v2Result res;
    if (!head.has(0, 10)) return res;
    const uint8_t major = head.u8(3);
    const uint8_t flags = head.u8(5);
    if (major < 2 || major > 4) return res;  // 未知版本：不解析（回退文件名）

    const size_t declared = syncsafe(head, 6);
    const bool unsync = (flags & 0x80u) != 0;
    const bool has_ext = (flags & 0x40u) != 0;
    const bool has_footer = (major >= 4) && ((flags & 0x10u) != 0);
    res.audio_offset = 10 + static_cast<long long>(declared) + (has_footer ? 10 : 0);

    const size_t to_read = std::min<size_t>(declared, kMaxId3Body);
    std::vector<uint8_t> body = f.read_vec(10, to_read);

    std::vector<uint8_t> cooked;
    const uint8_t* data = body.data();
    size_t size = body.size();
    if (unsync && size > 0) {
        cooked = deunsynchronise(body.data(), body.size());
        data = cooked.data();
        size = cooked.size();
    }
    const Buf b(data, size);

    size_t pos = 0;
    if (has_ext) {
        if (major == 3) {
            if (!b.has(0, 4)) return res;
            pos = 4 + static_cast<size_t>(b.u32be(0));
        } else if (major == 4) {
            if (!b.has(0, 4)) return res;
            pos = syncsafe(b, 0);
        } else {
            return res;  // v2.2 的压缩标签无法处理
        }
        if (pos > size) pos = size;
    }

    TagData t;
    bool meaningful = false;
    const size_t fhdr = (major == 2) ? 6 : 10;

    for (int guard = 0; guard < kMaxItems; ++guard) {
        if (pos >= size) break;
        if (b.u8(pos) == 0) break;  // padding 开始
        if (!b.has(pos, fhdr)) break;

        const std::string id = b.str(pos, (major == 2) ? 3 : 4);
        bool id_ok = true;
        for (char c : id) {
            if (!(std::isupper(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)))) {
                id_ok = false;
                break;
            }
        }
        if (!id_ok) break;

        size_t fsize = 0;
        if (major == 2) {
            fsize = b.u24be(pos + 3);
        } else if (major >= 4) {
            fsize = syncsafe(b, pos + 4);
        } else {
            fsize = b.u32be(pos + 4);
        }
        size_t fmt_flags = 0;
        if (major >= 3) fmt_flags = b.u8(pos + 9);

        const size_t content = pos + fhdr;
        const size_t avail = size - content;
        const size_t take = std::min<size_t>(fsize, avail);

        bool skip = false;
        if (major == 3) {
            if (fmt_flags & (0x80u | 0x40u)) skip = true;  // 压缩 / 加密
        } else if (major >= 4) {
            if (fmt_flags & (0x08u | 0x04u)) skip = true;  // 压缩 / 加密
        }

        if (!skip && take > 0) {
            const uint8_t* fp = b.at(content);
            size_t fn = take;
            std::vector<uint8_t> frame_buf;
            if (major >= 4 && (fmt_flags & 0x02u)) {  // 帧级 unsynchronisation
                frame_buf = deunsynchronise(fp, fn);
                fp = frame_buf.data();
                fn = frame_buf.size();
            }
            if (major >= 4 && (fmt_flags & 0x01u)) {  // data length indicator
                if (fn >= 4) {
                    fp += 4;
                    fn -= 4;
                } else {
                    fn = 0;
                }
            }
            if (fn > 0) {
                const uint8_t enc = fp[0];
                if (id[0] == 'T' && id != "TXXX") {
                    id3_set_text(t, id, decode_id3_text(fp + 1, fn - 1, enc), meaningful);
                } else if (id == "APIC" || id == "PIC") {
                    if (id3_parse_picture(fp, fn, id == "PIC", t)) meaningful = true;
                } else if (id == "USLT" || id == "ULT") {
                    if (id3_parse_lyrics(fp, fn, t)) meaningful = true;
                }
            }
        }

        if (take < fsize) break;  // 内容被截断（文件小于声明长度）
        pos = content + take;
    }

    if (meaningful) {
        t.tag_source = "id3v2";
        res.tags = t;
        res.ok = true;
    }
    return res;
}

// ---------------------------------------------------------------- FLAC

struct FlacInfo {
    bool ok = false;
    int sample_rate = 0;
    int channels = 0;
    int bit_depth = 0;
    long long total_samples = 0;
};

// 遍历 FLAC 元数据块；对 STREAMINFO/VORBIS_COMMENT/PICTURE 读取块内容
struct FlacResult {
    FlacInfo info;
    TagData tags;
    bool meaningful = false;
};

FlacResult parse_flac(FileReader& f) {
    FlacResult res;
    long long pos = 4;  // 跳过 "fLaC"
    for (int guard = 0; guard < kMaxBlocks; ++guard) {
        uint8_t hdr[4];
        if (!f.read_exact(pos, hdr, 4)) break;
        const bool last = (hdr[0] & 0x80u) != 0;
        const uint8_t type = hdr[0] & 0x7Fu;
        const size_t len = ((size_t)hdr[1] << 16) | ((size_t)hdr[2] << 8) | (size_t)hdr[3];
        const long long content = pos + 4;
        if (content + (long long)len > f.size()) break;  // 越界：停止

        if (len > 0 && len <= kMaxBlock) {
            if (type == 0) {  // STREAMINFO
                std::vector<uint8_t> d = f.read_vec(content, len);
                const Buf b(d.data(), d.size());
                if (b.has(0, 18)) {
                    res.info.sample_rate = ((int)b.u8(10) << 12) | ((int)b.u8(11) << 4) | (int)(b.u8(12) >> 4);
                    res.info.channels = (int)((b.u8(12) >> 1) & 0x07u) + 1;
                    res.info.bit_depth = (int)((((uint32_t)b.u8(12) & 0x01u) << 4) | (uint32_t)(b.u8(13) >> 4)) + 1;
                    res.info.total_samples = ((long long)(b.u8(13) & 0x0Fu) << 32) |
                                             ((long long)b.u8(14) << 24) | ((long long)b.u8(15) << 16) |
                                             ((long long)b.u8(16) << 8) | (long long)b.u8(17);
                    res.info.ok = res.info.sample_rate > 0;
                }
            } else if (type == 4) {  // VORBIS_COMMENT
                std::vector<uint8_t> d = f.read_vec(content, len);
                const Buf b(d.data(), d.size());
                if (b.has(0, 4)) {
                    const size_t vendor_len = b.u32le(0);
                    size_t p = 4;
                    if (vendor_len <= b.size() - std::min(p, b.size())) {
                        p += vendor_len;
                    } else {
                        p = b.size();
                    }
                    if (b.has(p, 4)) {
                        const uint32_t count = b.u32le(p);
                        p += 4;
                        const uint32_t limit = std::min<uint32_t>(count, 4096u);
                        for (uint32_t k = 0; k < limit; ++k) {
                            if (!b.has(p, 4)) break;
                            const size_t clen = b.u32le(p);
                            p += 4;
                            if (clen > b.size() - std::min(p, b.size())) break;
                            const std::string entry = b.str(p, clen);
                            p += clen;
                            const size_t eq = entry.find('=');
                            res.meaningful = true;
                            if (eq == std::string::npos) continue;
                            const std::string key = to_lower(entry.substr(0, eq));
                            const std::string val = trim(decode_bytes(
                                reinterpret_cast<const uint8_t*>(entry.data()) + eq + 1, entry.size() - eq - 1));
                            if (val.empty()) continue;
                            if (key == "title") {
                                res.tags.title = val;
                            } else if (key == "artist") {
                                if (res.tags.artist.empty()) res.tags.artist = val;
                                else if (res.tags.artist.find(val) == std::string::npos) res.tags.artist += " / " + val;
                            } else if (key == "album") {
                                res.tags.album = val;
                            } else if (key == "tracknumber" || key == "track") {
                                const int n = parse_int_prefix(val);
                                if (n > 0) res.tags.track_no = n;
                            } else if (key == "lyrics" || key == "unsyncedlyrics" || key == "unsynced lyrics") {
                                if (res.tags.lyrics_text.empty()) res.tags.lyrics_text = val;
                            } else if (key == "genre") {
                                res.tags.genre = val;
                            }
                        }
                    }
                }
            } else if (type == 6) {  // PICTURE
                std::vector<uint8_t> d = f.read_vec(content, len);
                const Buf b(d.data(), d.size());
                size_t p = 4;  // picture type
                if (b.has(0, 8)) {
                    const size_t mime_len = b.u32be(4);
                    p = 8;
                    if (b.has(p, mime_len)) {
                        const std::string mime = to_lower(b.str(p, mime_len));
                        p += mime_len;
                        if (b.has(p, 4)) {
                            const size_t desc_len = b.u32be(p);
                            p += 4;
                            if (b.has(p, desc_len)) {
                                p += desc_len;
                                if (b.has(p, 20)) {
                                    const size_t data_len = b.u32be(p + 16);
                                    p += 20;
                                    if (data_len > 0 && data_len <= kMaxCover && b.has(p, data_len)) {
                                        res.tags.cover.assign(b.at(p), b.at(p) + data_len);
                                        res.tags.cover_mime = mime.rfind("image/", 0) == 0
                                                                  ? mime
                                                                  : sniff_image_mime(b.at(p), data_len);
                                        if (res.tags.cover_mime.empty()) res.tags.cover_mime = "image/jpeg";
                                        res.meaningful = true;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        if (last) break;
        if (len > static_cast<size_t>(f.size())) break;
        pos = content + static_cast<long long>(len);
    }
    if (res.meaningful) res.tags.tag_source = "vorbis";
    return res;
}

// ---------------------------------------------------------------- MP4 / M4A

struct Mp4Box {
    size_t content = 0;
    size_t size = 0;
};

bool mp4_next(const Buf& b, size_t pos, size_t limit, size_t& next, Mp4Box& box) {
    if (pos + 8 > limit || !b.has(pos, 8)) return false;
    uint64_t sz = b.u32be(pos);
    size_t hdr = 8;
    if (sz == 1) {
        if (pos + 16 > limit || !b.has(pos, 16)) return false;
        sz = b.u64be(pos + 8);
        hdr = 16;
    } else if (sz == 0) {
        sz = limit - pos;
    }
    if (sz < hdr || sz > (uint64_t)(limit - pos)) return false;
    box.content = pos + hdr;
    box.size = static_cast<size_t>(sz) - hdr;
    next = pos + static_cast<size_t>(sz);
    return next > pos;
}

bool mp4_find(const Buf& b, size_t start, size_t limit, const char* type, Mp4Box& out) {
    size_t pos = start;
    for (int guard = 0; guard < kMaxBlocks; ++guard) {
        if (pos + 8 > limit) return false;
        Mp4Box box;
        size_t next = 0;
        if (!mp4_next(b, pos, limit, next, box)) return false;
        if (match4(b, pos + 4, type)) {  // box 类型在 size 之后（pos+4）
            out = box;
            return true;
        }
        pos = next;
    }
    return false;
}

bool mp4_top_box(FileReader& f, const char* type, long long& content, long long& size) {
    long long pos = 0;
    for (int guard = 0; guard < kMaxBlocks; ++guard) {
        uint8_t h[16];
        if (pos + 8 > f.size() || !f.read_exact(pos, h, 8)) return false;
        uint64_t sz = ((uint64_t)h[0] << 24) | ((uint64_t)h[1] << 16) | ((uint64_t)h[2] << 8) | h[3];
        size_t hdr = 8;
        if (sz == 1) {
            if (!f.read_exact(pos + 8, h + 8, 8)) return false;
            sz = 0;
            for (int i = 0; i < 8; ++i) sz = (sz << 8) | h[8 + i];
            hdr = 16;
        } else if (sz == 0) {
            sz = static_cast<uint64_t>(f.size() - pos);
        }
        if (sz < hdr || sz > static_cast<uint64_t>(f.size() - pos)) return false;
        if (std::memcmp(h + 4, type, 4) == 0) {
            content = pos + static_cast<long long>(hdr);
            size = static_cast<long long>(std::min<uint64_t>(sz - hdr, static_cast<uint64_t>(f.size() - content)));
            return true;
        }
        pos += static_cast<long long>(sz);
    }
    return false;
}

struct Mp4Info {
    bool ok = false;
    long long duration_ms = 0;
    int sample_rate = 0;
    int channels = 0;
    int bit_depth = 0;
    std::string codec;
};

// data box 的 payload：4 字节类型指示 + 4 字节 locale 之后
bool mp4_data_payload(const Buf& b, const Mp4Box& data_box, const uint8_t*& p, size_t& n, uint32_t& type) {
    if (data_box.size < 8) return false;
    const size_t off = data_box.content;
    if (!b.has(off, 8)) return false;
    type = b.u32be(off) & 0x00FFFFFFu;
    p = b.at(off + 8);
    n = data_box.size - 8;
    return true;
}

std::string mp4_text_value(const Buf& b, const Mp4Box& data_box) {
    const uint8_t* p = nullptr;
    size_t n = 0;
    uint32_t type = 0;
    if (!mp4_data_payload(b, data_box, p, n, type)) return std::string();
    if (type == 2) return utf16_to_utf8(p, n, /*big_endian=*/true, /*allow_bom=*/true);
    return decode_bytes(p, n);
}

struct Mp4Result {
    Mp4Info info;
    TagData tags;
    bool meaningful = false;
};

Mp4Result parse_mp4(FileReader& f) {
    Mp4Result res;
    long long moov_off = 0;
    long long moov_size = 0;
    if (!mp4_top_box(f, "moov", moov_off, moov_size)) return res;
    if (moov_size <= 0) return res;
    const size_t to_read = static_cast<size_t>(std::min<long long>(moov_size, (long long)kMaxMoov));
    std::vector<uint8_t> moov = f.read_vec(moov_off, to_read);
    const Buf b(moov.data(), moov.size());

    // mvhd：时长（timescale 换算成 ms）
    Mp4Box mvhd;
    if (mp4_find(b, 0, b.size(), "mvhd", mvhd) && mvhd.size >= 20) {
        const size_t o = mvhd.content;
        const uint8_t version = b.u8(o);
        uint64_t timescale = 0;
        uint64_t duration = 0;
        if (version == 1) {
            if (b.has(o + 20, 12)) {
                timescale = b.u32be(o + 20);
                duration = b.u64be(o + 24);
            }
        } else if (b.has(o + 12, 8)) {
            timescale = b.u32be(o + 12);
            duration = b.u32be(o + 16);
        }
        if (timescale > 0 && duration > 0 && duration != 0xFFFFFFFFull) {
            res.info.duration_ms = static_cast<long long>(duration * 1000ull / timescale);
        }
    }

    // stsd：编码与采样率/声道/位深
    Mp4Box trak;
    if (mp4_find(b, 0, b.size(), "trak", trak)) {
        Mp4Box mdia;
        if (mp4_find(b, trak.content, trak.content + trak.size, "mdia", mdia)) {
            Mp4Box minf;
            if (mp4_find(b, mdia.content, mdia.content + mdia.size, "minf", minf)) {
                Mp4Box stbl;
                if (mp4_find(b, minf.content, minf.content + minf.size, "stbl", stbl)) {
                    Mp4Box stsd;
                    if (mp4_find(b, stbl.content, stbl.content + stbl.size, "stsd", stsd) && stsd.size >= 8) {
                        const size_t entry = stsd.content + 8;  // version+flags(4) + entry_count(4)
                        if (b.has(entry, 8)) {
                            const uint32_t entry_size = b.u32be(entry);
                            if (entry_size >= 8 && entry_size <= stsd.content + stsd.size - entry) {
                                const size_t fmt = entry + 4;
                                const bool alac = match4(b, fmt, "alac");
                                const bool mp4a = match4(b, fmt, "mp4a");
                                if (alac) {
                                    res.info.codec = "alac";
                                } else if (mp4a) {
                                    res.info.codec = "aac";
                                }
                                // AudioSampleEntry 固定字段（相对 entry 起点）：
                                //   +24 channelcount(2)、+26 samplesize(2)、+32 samplerate(4, 16.16)
                                if (b.has(entry + 24, 12)) {
                                    const int ch = (int)b.u16be(entry + 24);
                                    const int bits = (int)b.u16be(entry + 26);
                                    const int rate = (int)(b.u32be(entry + 32) >> 16);
                                    if (ch > 0) res.info.channels = ch;
                                    if (bits > 0) res.info.bit_depth = bits;
                                    if (rate > 0) res.info.sample_rate = rate;
                                }
                                // alac 子 box 里可能带更准确的位深（best-effort）
                                if (alac) {
                                    Mp4Box alac_box;
                                    if (mp4_find(b, entry + 8, entry + entry_size, "alac", alac_box) &&
                                        alac_box.size >= 24) {
                                        const int bd = (int)b.u8(alac_box.content + 5);
                                        if (bd > 0 && bd <= 32) res.info.bit_depth = bd;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // udta/meta/ilst：标签
    Mp4Box udta;
    Mp4Box ilst;
    if (mp4_find(b, 0, b.size(), "udta", udta) &&
        mp4_find(b, udta.content, udta.content + udta.size, "meta", ilst) && ilst.size >= 4) {
        Mp4Box list;
        if (mp4_find(b, ilst.content + 4, ilst.content + ilst.size, "ilst", list)) {
            static const char kNam[4] = {(char)0xA9, 'n', 'a', 'm'};
            static const char kArt[4] = {(char)0xA9, 'A', 'R', 'T'};
            static const char kAlb[4] = {(char)0xA9, 'a', 'l', 'b'};
            static const char kGen[4] = {(char)0xA9, 'g', 'e', 'n'};
            static const char kTrk[4] = {'t', 'r', 'k', 'n'};
            static const char kCov[4] = {'c', 'o', 'v', 'r'};

            size_t pos = list.content;
            const size_t limit = list.content + list.size;
            for (int guard = 0; guard < kMaxBlocks; ++guard) {
                if (pos + 8 > limit) break;
                Mp4Box item;
                size_t next = 0;
                if (!mp4_next(b, pos, limit, next, item)) break;
                const bool is_nam = match4(b, pos + 4, kNam);
                const bool is_art = match4(b, pos + 4, kArt);
                const bool is_alb = match4(b, pos + 4, kAlb);
                const bool is_gen = match4(b, pos + 4, kGen);
                const bool is_trk = match4(b, pos + 4, kTrk);
                const bool is_cov = match4(b, pos + 4, kCov);
                if (is_nam || is_art || is_alb || is_gen || is_trk || is_cov) {
                    Mp4Box data_box;
                    if (mp4_find(b, item.content, item.content + item.size, "data", data_box)) {
                        if (is_cov) {
                            const uint8_t* p = nullptr;
                            size_t n = 0;
                            uint32_t dtype = 0;
                            if (mp4_data_payload(b, data_box, p, n, dtype) && n > 0 && n <= kMaxCover) {
                                res.tags.cover.assign(p, p + n);
                                if (dtype == 14 || sniff_image_mime(p, n) == "image/png") {
                                    res.tags.cover_mime = "image/png";
                                } else {
                                    res.tags.cover_mime = "image/jpeg";
                                }
                                res.meaningful = true;
                            }
                        } else if (is_trk) {
                            const uint8_t* p = nullptr;
                            size_t n = 0;
                            uint32_t dtype = 0;
                            if (mp4_data_payload(b, data_box, p, n, dtype) && n >= 4) {
                                const int track = (int)(((uint16_t)p[2] << 8) | p[3]);
                                if (track > 0) {
                                    res.tags.track_no = track;
                                    res.meaningful = true;
                                }
                            }
                        } else {
                            const std::string val = trim(mp4_text_value(b, data_box));
                            if (!val.empty()) {
                                res.meaningful = true;
                                if (is_nam) res.tags.title = val;
                                else if (is_art) res.tags.artist = val;
                                else if (is_alb) res.tags.album = val;
                                else if (is_gen) res.tags.genre = val;
                            }
                        }
                    }
                }
                pos = next;
            }
        }
    }

    if (res.info.codec.empty() && (res.info.sample_rate > 0 || res.info.duration_ms > 0)) {
        res.info.codec = "aac";  // 兜底：M4A 默认 AAC
    }
    res.info.ok = res.info.codec.empty() ? false : true;
    if (res.meaningful) res.tags.tag_source = "mp4";
    return res;
}

// ---------------------------------------------------------------- WAV

struct WavInfo {
    bool ok = false;
    int sample_rate = 0;
    int channels = 0;
    int bit_depth = 0;
    int byte_rate = 0;
    long long duration_ms = 0;
};

struct WavResult {
    WavInfo info;
    TagData tags;
    bool meaningful = false;
};

void wav_info_field(TagData& t, const std::string& id, const std::string& value, bool& meaningful) {
    const std::string v = trim(value);
    if (v.empty()) return;
    meaningful = true;
    if (id == "INAM") {
        t.title = v;
    } else if (id == "IART") {
        t.artist = v;
    } else if (id == "IPRD" || id == "IALB") {
        t.album = v;
    } else if (id == "IGNR") {
        t.genre = v;
    } else if (id == "ITRK" || id == "IPRT") {
        const int n = parse_int_prefix(v);
        if (n > 0) t.track_no = n;
    }
}

WavResult parse_wav(FileReader& f) {
    WavResult res;
    long long pos = 12;  // RIFF(4) + size(4) + WAVE(4)
    for (int guard = 0; guard < kMaxBlocks; ++guard) {
        uint8_t hdr[8];
        if (pos + 8 > f.size() || !f.read_exact(pos, hdr, 8)) break;
        const std::string id(reinterpret_cast<const char*>(hdr), 4);
        const uint32_t len = ((uint32_t)hdr[4]) | ((uint32_t)hdr[5] << 8) | ((uint32_t)hdr[6] << 16) |
                             ((uint32_t)hdr[7] << 24);
        const long long content = pos + 8;
        const uint64_t avail = static_cast<uint64_t>(std::max<long long>(f.size() - content, 0));
        if (len > avail) break;  // 越界/损坏

        if (id == "fmt " && len >= 16) {
            std::vector<uint8_t> d = f.read_vec(content, std::min<uint32_t>(len, 64));
            const Buf b(d.data(), d.size());
            if (b.has(0, 16)) {
                res.info.channels = (int)b.u16le(2);
                res.info.sample_rate = (int)b.u32le(4);
                res.info.byte_rate = (int)b.u32le(8);
                res.info.bit_depth = (int)b.u16le(14);
                res.info.ok = res.info.sample_rate > 0;
            }
        } else if (id == "data") {
            if (res.info.byte_rate > 0) {
                res.info.duration_ms = static_cast<long long>(len) * 1000 / res.info.byte_rate;
            }
        } else if (id == "LIST" && len >= 4) {
            std::vector<uint8_t> d = f.read_vec(content, std::min<uint32_t>(len, (uint32_t)kMaxChunk));
            const Buf b(d.data(), d.size());
            if (b.has(0, 4) && b.str(0, 4) == "INFO") {
                size_t p = 4;
                for (int g2 = 0; g2 < kMaxBlocks; ++g2) {
                    if (!b.has(p, 8)) break;
                    const std::string sid = b.str(p, 4);
                    const uint32_t slen = b.u32le(p + 4);
                    p += 8;
                    if (slen > b.size() - std::min(p, b.size())) break;
                    wav_info_field(res.tags, sid, decode_bytes(b.at(p), slen), res.meaningful);
                    p += slen + (slen & 1u);
                }
            }
        }
        pos = content + static_cast<long long>(len) + (len & 1u);
    }
    if (res.meaningful) res.tags.tag_source = "wav";
    return res;
}

// ---------------------------------------------------------------- APE

struct ApeResult {
    TagData tags;
    bool meaningful = false;
    bool parsed = false;
};

ApeResult parse_ape_tags(FileReader& f) {
    ApeResult res;
    const long long size = f.size();
    if (size < 32) return res;
    uint8_t footer[32];
    if (!f.read_exact(size - 32, footer, 32)) return res;
    if (std::memcmp(footer, "APETAGEX", 8) != 0) return res;
    const Buf fb(footer, 32);
    const uint32_t tag_size = fb.u32le(12);
    const uint32_t item_count = fb.u32le(16);
    const uint32_t flags = fb.u32le(20);
    if (tag_size < 32 || tag_size > static_cast<uint32_t>(size)) return res;
    if (item_count > 4096) return res;

    const long long items_start = size - static_cast<long long>(tag_size);
    const long long items_end = size - 32;
    if (items_start < 0 || items_end < items_start) return res;
    const size_t to_read = static_cast<size_t>(items_end - items_start);
    if (to_read > kMaxChunk) return res;
    std::vector<uint8_t> data = f.read_vec(items_start, to_read);
    const Buf b(data.data(), data.size());
    res.parsed = true;

    (void)flags;  // 头部存在与否不影响条目解析（条目紧跟头部之后）
    size_t p = 0;
    for (uint32_t i = 0; i < item_count; ++i) {
        if (!b.has(p, 8)) break;
        const uint32_t value_size = b.u32le(p);
        const uint32_t item_flags = b.u32le(p + 4);
        p += 8;
        const size_t key_start = p;
        while (p < b.size() && b.u8(p) != 0) ++p;
        if (p >= b.size()) break;
        const std::string key = b.str(key_start, p - key_start);
        ++p;  // 跳过 NUL
        if (value_size > b.size() - std::min(p, b.size())) break;
        const uint8_t* value = b.at(p);
        const bool binary = (item_flags & 0x2u) != 0;
        const std::string lkey = to_lower(key);
        if (binary) {
            if (lkey.rfind("cover art", 0) == 0) {
                size_t name_len = 0;
                while (name_len < value_size && value[name_len] != 0) ++name_len;
                const size_t img_off = (name_len < value_size) ? name_len + 1 : 0;
                const size_t img_len = value_size - img_off;
                if (img_len > 0 && img_len <= kMaxCover) {
                    res.tags.cover.assign(value + img_off, value + value_size);
                    std::string mime = sniff_image_mime(value + img_off, img_len);
                    if (mime.empty()) mime = "image/jpeg";
                    res.tags.cover_mime = mime;
                    res.meaningful = true;
                }
            }
        } else {
            const std::string val = trim(decode_bytes(value, value_size));
            if (!val.empty()) {
                if (lkey == "title") {
                    res.tags.title = val;
                    res.meaningful = true;
                } else if (lkey == "artist") {
                    res.tags.artist = val;
                    res.meaningful = true;
                } else if (lkey == "album") {
                    res.tags.album = val;
                    res.meaningful = true;
                } else if (lkey == "track") {
                    const int n = parse_int_prefix(val);
                    if (n > 0) {
                        res.tags.track_no = n;
                        res.meaningful = true;
                    }
                } else if (lkey == "genre") {
                    res.tags.genre = val;
                    res.meaningful = true;
                } else if (lkey == "lyrics") {
                    res.tags.lyrics_text = val;
                    res.meaningful = true;
                }
            }
        }
        p += value_size;
    }
    if (res.meaningful) res.tags.tag_source = "ape";
    return res;
}

// Monkey's Audio（.ape）流头：仅支持 3.98+ 的 descriptor 布局
struct ApeStream {
    bool ok = false;
    int sample_rate = 0;
    int channels = 0;
    int bit_depth = 0;
    long long total_samples = 0;
};

ApeStream probe_ape(FileReader& f) {
    ApeStream out;
    uint8_t h[76];
    if (!f.read_exact(0, h, 76)) return out;
    if (std::memcmp(h, "MAC ", 4) != 0) return out;
    const Buf b(h, sizeof(h));
    const uint16_t version = b.u16le(4);
    if (version < 3980) return out;  // 老版本布局不同：交给调用方回退
    out.channels = (int)b.u16le(70);
    out.bit_depth = (int)b.u16le(68);
    out.sample_rate = (int)b.u32le(72);
    const uint32_t blocks_per_frame = b.u32le(56);
    const uint32_t final_frame_blocks = b.u32le(60);
    const uint32_t total_frames = b.u32le(64);
    if (total_frames > 0 && blocks_per_frame > 0) {
        out.total_samples = (long long)(total_frames - 1) * blocks_per_frame + final_frame_blocks;
    }
    out.ok = out.sample_rate > 0 && out.channels > 0;
    return out;
}

// ---------------------------------------------------------------- Ogg / Opus

struct OggInfo {
    bool ok = false;
    bool opus = false;
    std::string codec;
    int sample_rate = 0;
    int channels = 0;
    int bitrate = 0;
    long long duration_ms = 0;
};

OggInfo probe_ogg(FileReader& f, const Buf& head) {
    OggInfo out;
    if (!head.has(0, 27) || head.str(0, 4) != "OggS") return out;
    const size_t segments = head.u8(26);
    const size_t payload = 27 + segments;
    if (!head.has(payload, 8)) return out;
    const size_t payload_len = head.size() - payload;

    if (head.str(payload, 8) == "OpusHead" && head.has(payload, 19)) {
        out.opus = true;
        out.codec = "opus";
        out.channels = (int)head.u8(payload + 9);
        const uint32_t input_rate = head.u32le(payload + 12);
        out.sample_rate = 48000;  // Opus 解码固定 48 kHz
        (void)input_rate;
    } else if (payload_len >= 30 && head.u8(payload) == 1 && head.str(payload + 1, 6) == "vorbis") {
        out.codec = "vorbis";
        const uint32_t rate = head.u32le(payload + 12);
        out.channels = (int)head.u8(payload + 11);
        out.sample_rate = (int)rate;
        const int32_t nominal = (int32_t)head.u32le(payload + 20);
        if (nominal > 0) out.bitrate = nominal / 1000;
    } else {
        // 可能是 OpusHead 未落在首页载荷起点（继续页/分片），做一次范围扫描
        const size_t lim = std::min<size_t>(head.size(), 4096);
        for (size_t i = 0; i + 19 <= lim; ++i) {
            if (head.str(i, 8) == "OpusHead") {
                out.opus = true;
                out.codec = "opus";
                out.channels = (int)head.u8(i + 9);
                out.sample_rate = 48000;
                break;
            }
        }
        if (!out.ok && out.codec.empty()) return out;
    }
    if (out.sample_rate <= 0) return out;
    if (out.channels <= 0) out.channels = 2;

    // 时长：从文件尾找最后一个 OggS 页的 granule position
    const size_t tail_want = static_cast<size_t>(std::min<long long>(f.size(), 64 * 1024));
    if (tail_want > 0) {
        std::vector<uint8_t> tail = f.read_vec(f.size() - static_cast<long long>(tail_want), tail_want);
        const Buf tb(tail.data(), tail.size());
        long long last_granule = -1;
        for (size_t i = 0; i + 14 <= tb.size(); ++i) {
            if (tb.str(i, 4) == "OggS") {
                const uint64_t g = tb.u64le(i + 6);
                if (g != 0xFFFFFFFFFFFFFFFFull) last_granule = (long long)g;
            }
        }
        if (last_granule > 0) {
            if (out.opus) {
                out.duration_ms = last_granule * 1000 / 48000;
            } else {
                out.duration_ms = last_granule * 1000 / out.sample_rate;
            }
        }
    }
    if (out.bitrate <= 0 && out.duration_ms > 0) {
        const long long kbps = f.size() * 8 / out.duration_ms;
        if (kbps > 0 && kbps < 100000) out.bitrate = (int)kbps;
    }
    out.ok = true;
    return out;
}

// ---------------------------------------------------------------- AAC (ADTS)

struct AdtsInfo {
    bool ok = false;
    int sample_rate = 0;
    int channels = 0;
    int bitrate = 0;
    long long duration_ms = 0;
};

AdtsInfo probe_adts(FileReader& f, const Buf& head) {
    AdtsInfo out;
    static const int kRates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                   22050, 16000, 12000, 11025, 8000, 7350};
    long long base = 0;
    if (head.has(0, 3) && head.str(0, 3) == "ID3") {
        if (!head.has(0, 10)) return out;
        const size_t tag = syncsafe(head, 6);
        const bool footer = (head.u8(3) >= 4) && ((head.u8(5) & 0x10u) != 0);
        base = 10 + static_cast<long long>(tag) + (footer ? 10 : 0);
    }
    uint8_t h[9];
    if (!f.read_exact(base, h, 9)) return out;
    const Buf b(h, sizeof(h));
    if (b.u8(0) != 0xFF || (b.u8(1) & 0xF6u) != 0xF0u) return out;
    const int sr_idx = (int)((b.u8(2) >> 2) & 0x0Fu);
    if (sr_idx >= 13) return out;
    out.sample_rate = kRates[sr_idx];
    out.channels = (int)(((b.u8(2) & 0x01u) << 2) | (b.u8(3) >> 6));
    const size_t frame_len = ((size_t)(b.u8(3) & 0x03u) << 11) | ((size_t)b.u8(4) << 3) | ((size_t)b.u8(5) >> 5);
    if (frame_len < 7) return out;
    const long long audio_bytes = f.size() - base;
    if (audio_bytes <= 0) return out;
    out.bitrate = (int)((long long)frame_len * 8 * out.sample_rate / 1024 / 1000);
    out.duration_ms = audio_bytes / (long long)frame_len * 1024 * 1000 / out.sample_rate;
    out.ok = out.sample_rate > 0;
    return out;
}

// ---------------------------------------------------------------- 公共小工具

bool meaningful_tags(const TagData& t) {
    return !t.title.empty() || !t.artist.empty() || !t.album.empty() || t.track_no > 0 ||
           !t.cover.empty() || !t.lyrics_text.empty() || !t.genre.empty();
}

std::string basename_no_ext(const std::string& path) {
    size_t sep = path.find_last_of("/\\");
    std::string name = (sep == std::string::npos) ? path : path.substr(sep + 1);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) name = name.substr(0, dot);
    return name;
}

}  // namespace

// ================================================================ 公共接口

const char* audio_container_id(AudioContainer c) {
    switch (c) {
        case AudioContainer::Mp3: return "mp3";
        case AudioContainer::Flac: return "flac";
        case AudioContainer::M4a: return "m4a";
        case AudioContainer::Wav: return "wav";
        case AudioContainer::Ape: return "ape";
        case AudioContainer::Ogg: return "ogg";
        case AudioContainer::Opus: return "opus";
        case AudioContainer::Aac: return "aac";
        default: return "unknown";
    }
}

AudioContainer audio_container_from_extension(std::string_view path) {
    const size_t sep = path.find_last_of("/\\");
    const size_t dot = path.find_last_of('.');
    if (dot == std::string_view::npos) return AudioContainer::Unknown;
    if (sep != std::string_view::npos && dot < sep) return AudioContainer::Unknown;
    const std::string ext = to_lower(path.substr(dot + 1));
    if (ext == "mp3" || ext == "mp2" || ext == "mpga") return AudioContainer::Mp3;
    if (ext == "flac") return AudioContainer::Flac;
    if (ext == "m4a" || ext == "mp4" || ext == "m4b" || ext == "m4r" || ext == "aac-lc")
        return AudioContainer::M4a;
    if (ext == "wav" || ext == "wave") return AudioContainer::Wav;
    if (ext == "ape") return AudioContainer::Ape;
    if (ext == "ogg" || ext == "oga") return AudioContainer::Ogg;
    if (ext == "opus") return AudioContainer::Opus;
    if (ext == "aac" || ext == "adts") return AudioContainer::Aac;
    return AudioContainer::Unknown;
}

bool audio_container_supported(AudioContainer c) { return c != AudioContainer::Unknown; }

Result<TagData> read_tags(const std::string& path) {
    FileReader f;
    if (!f.open(path)) {
        return Result<TagData>::failure(ErrorCode::FileNotFound, "无法打开文件: " + path);
    }
    const long long size = f.size();
    std::vector<uint8_t> head;
    if (size > 0) {
        head.resize(static_cast<size_t>(std::min<long long>(size, static_cast<long long>(kHeadBytes))));
        head.resize(f.read_some(0, head.data(), head.size()));
    }
    const Buf hb(head.data(), head.size());
    const AudioContainer container = sniff(hb, audio_container_from_extension(path));
    if (!audio_container_supported(container)) {
        return Result<TagData>::failure(ErrorCode::UnsupportedFormat,
                                        "不支持的音频格式: " + std::string(audio_container_id(container)));
    }

    TagData tags;
    bool ok = false;
    switch (container) {
        case AudioContainer::Mp3: {
            const Id3v2Result id3 = parse_id3v2(f, hb);
            if (id3.ok) {
                tags = id3.tags;
                ok = true;
            }
            const Mp3Stream stream = probe_mp3_stream(f, id3.audio_offset);
            if (stream.ok && stream.duration_ms > 0) {
                tags.duration_seconds = static_cast<int>((stream.duration_ms + 500) / 1000);
            }
            break;
        }
        case AudioContainer::Flac: {
            const FlacResult r = parse_flac(f);
            if (r.meaningful) {
                tags = r.tags;
                ok = true;
            }
            if (r.info.ok && r.info.sample_rate > 0 && r.info.total_samples > 0) {
                tags.duration_seconds = static_cast<int>(
                    (r.info.total_samples + r.info.sample_rate / 2) / r.info.sample_rate);
            }
            break;
        }
        case AudioContainer::M4a: {
            const Mp4Result r = parse_mp4(f);
            if (r.meaningful) {
                tags = r.tags;
                ok = true;
            }
            if (r.info.duration_ms > 0) {
                tags.duration_seconds = static_cast<int>((r.info.duration_ms + 500) / 1000);
            }
            break;
        }
        case AudioContainer::Wav: {
            const WavResult r = parse_wav(f);
            if (r.meaningful) {
                tags = r.tags;
                ok = true;
            }
            if (r.info.duration_ms > 0) {
                tags.duration_seconds = static_cast<int>((r.info.duration_ms + 500) / 1000);
            }
            break;
        }
        case AudioContainer::Ape: {
            const ApeResult r = parse_ape_tags(f);
            if (r.meaningful) {
                tags = r.tags;
                ok = true;
            }
            if (r.parsed) {
                const ApeStream s = probe_ape(f);
                if (s.ok && s.total_samples > 0) {
                    tags.duration_seconds =
                        static_cast<int>((s.total_samples + s.sample_rate / 2) / s.sample_rate);
                }
            }
            break;
        }
        default:
            // Ogg / Opus：按契约只做 probe 识别，标签解析返回 TagParseFailed
            break;
    }

    if (!ok || !meaningful_tags(tags)) {
        return Result<TagData>::failure(ErrorCode::TagParseFailed,
                                        "标签缺失或损坏（非致命，可回退文件名）: " + path);
    }
    return Result<TagData>::success(tags);
}

Result<AudioStreamInfo> probe_audio(const std::string& path) {
    FileReader f;
    if (!f.open(path)) {
        return Result<AudioStreamInfo>::failure(ErrorCode::FileNotFound, "无法打开文件: " + path);
    }
    const long long size = f.size();
    std::vector<uint8_t> head;
    if (size > 0) {
        head.resize(static_cast<size_t>(std::min<long long>(size, static_cast<long long>(kHeadBytes))));
        head.resize(f.read_some(0, head.data(), head.size()));
    }
    const Buf hb(head.data(), head.size());
    const AudioContainer container = sniff(hb, audio_container_from_extension(path));
    if (!audio_container_supported(container)) {
        return Result<AudioStreamInfo>::failure(ErrorCode::UnsupportedFormat,
                                                "不支持的音频格式: " + std::string(audio_container_id(container)));
    }

    AudioStreamInfo info;
    info.file_size = size;

    switch (container) {
        case AudioContainer::Mp3: {
            const Id3v2Result id3 = parse_id3v2(f, hb);
            const Mp3Stream s = probe_mp3_stream(f, id3.audio_offset);
            if (!s.ok) {
                return Result<AudioStreamInfo>::failure(ErrorCode::TagParseFailed,
                                                        "MP3 帧同步失败: " + path);
            }
            info.codec = "mp3";
            info.lossless = false;
            info.bitrate = s.bitrate;
            info.sample_rate = s.frame.sample_rate;
            info.channels = s.frame.channels;
            info.duration_ms = s.duration_ms;
            break;
        }
        case AudioContainer::Flac: {
            const FlacResult r = parse_flac(f);
            if (!r.info.ok) {
                return Result<AudioStreamInfo>::failure(ErrorCode::TagParseFailed,
                                                        "FLAC STREAMINFO 解析失败: " + path);
            }
            info.codec = "flac";
            info.lossless = true;
            info.sample_rate = r.info.sample_rate;
            info.channels = r.info.channels;
            info.bit_depth = r.info.bit_depth;
            if (r.info.total_samples > 0) {
                info.duration_ms = r.info.total_samples * 1000 / r.info.sample_rate;
            }
            if (info.duration_ms > 0) {
                const long long kbps = size * 8 / info.duration_ms;
                if (kbps > 0 && kbps < 100000) info.bitrate = static_cast<int>(kbps);
            }
            break;
        }
        case AudioContainer::M4a: {
            const Mp4Result r = parse_mp4(f);
            if (!r.info.ok) {
                return Result<AudioStreamInfo>::failure(ErrorCode::TagParseFailed,
                                                        "MP4/M4A 结构解析失败: " + path);
            }
            info.codec = r.info.codec;
            info.lossless = (r.info.codec == "alac");
            info.sample_rate = r.info.sample_rate > 0 ? std::optional<int>(r.info.sample_rate) : std::nullopt;
            info.channels = r.info.channels > 0 ? std::optional<int>(r.info.channels) : std::nullopt;
            if (info.lossless && r.info.bit_depth > 0) info.bit_depth = r.info.bit_depth;
            info.duration_ms = r.info.duration_ms;
            if (info.duration_ms > 0) {
                const long long kbps = size * 8 / info.duration_ms;
                if (kbps > 0 && kbps < 100000) info.bitrate = static_cast<int>(kbps);
            }
            break;
        }
        case AudioContainer::Wav: {
            const WavResult r = parse_wav(f);
            if (!r.info.ok) {
                return Result<AudioStreamInfo>::failure(ErrorCode::TagParseFailed,
                                                        "WAV fmt 块解析失败: " + path);
            }
            info.codec = "pcm";
            info.lossless = true;
            info.sample_rate = r.info.sample_rate;
            info.channels = r.info.channels;
            if (r.info.bit_depth > 0) info.bit_depth = r.info.bit_depth;
            info.duration_ms = r.info.duration_ms;
            if (r.info.byte_rate > 0) info.bitrate = r.info.byte_rate * 8 / 1000;
            break;
        }
        case AudioContainer::Ape: {
            const ApeStream s = probe_ape(f);
            if (!s.ok) {
                return Result<AudioStreamInfo>::failure(
                    ErrorCode::TagParseFailed, "APE 头部解析失败（仅支持 3.98+）: " + path);
            }
            info.codec = "ape";
            info.lossless = true;
            info.sample_rate = s.sample_rate;
            info.channels = s.channels;
            info.bit_depth = s.bit_depth;
            if (s.total_samples > 0) {
                info.duration_ms = s.total_samples * 1000 / s.sample_rate;
            }
            if (info.duration_ms > 0) {
                const long long kbps = size * 8 / info.duration_ms;
                if (kbps > 0 && kbps < 100000) info.bitrate = static_cast<int>(kbps);
            }
            break;
        }
        case AudioContainer::Ogg:
        case AudioContainer::Opus: {
            const OggInfo o = probe_ogg(f, hb);
            if (!o.ok) {
                return Result<AudioStreamInfo>::failure(ErrorCode::TagParseFailed,
                                                        "Ogg 流头解析失败: " + path);
            }
            info.codec = o.codec;
            info.lossless = false;
            info.sample_rate = o.sample_rate;
            info.channels = o.channels;
            if (o.bitrate > 0) info.bitrate = o.bitrate;
            info.duration_ms = o.duration_ms;
            break;
        }
        case AudioContainer::Aac: {
            const AdtsInfo a = probe_adts(f, hb);
            if (!a.ok) {
                return Result<AudioStreamInfo>::failure(ErrorCode::TagParseFailed,
                                                        "AAC/ADTS 头解析失败: " + path);
            }
            info.codec = "aac";
            info.lossless = false;
            info.sample_rate = a.sample_rate;
            info.channels = a.channels;
            info.bitrate = a.bitrate;
            info.duration_ms = a.duration_ms;
            break;
        }
        default:
            return Result<AudioStreamInfo>::failure(ErrorCode::UnsupportedFormat,
                                                    "不支持的音频格式: " + path);
    }
    return Result<AudioStreamInfo>::success(info);
}

TagData tags_from_filename(const std::string& path, std::string_view filename_template) {
    TagData tags;
    tags.tag_source = "filename";
    const std::string stem = trim(basename_no_ext(path));
    if (stem.empty()) return tags;

    const std::string tmpl(filename_template);
    const size_t pa = tmpl.find("{artist}");
    const size_t pt = tmpl.find("{title}");

    auto split_on = [&](const std::string& sep) -> bool {
        if (sep.empty()) return false;
        const size_t k = stem.find(sep);
        if (k == std::string::npos) return false;
        const std::string left = trim(stem.substr(0, k));
        const std::string right = trim(stem.substr(k + sep.size()));
        if (left.empty() || right.empty()) return false;
        tags.artist = left;
        tags.title = right;
        return true;
    };

    bool matched = false;
    if (pa != std::string::npos && pt != std::string::npos && pa != pt) {
        if (pa < pt) {
            const std::string prefix = tmpl.substr(0, pa);
            const std::string sep = tmpl.substr(pa + 8, pt - pa - 8);
            std::string body = stem;
            const std::string pfx = trim(prefix);
            if (!pfx.empty() && body.rfind(pfx, 0) == 0) body = trim(body.substr(pfx.size()));
            const size_t k = sep.empty() ? std::string::npos : body.find(sep);
            if (k != std::string::npos) {
                const std::string left = trim(body.substr(0, k));
                const std::string right = trim(body.substr(k + sep.size()));
                if (!left.empty() && !right.empty()) {
                    tags.artist = left;
                    tags.title = right;
                    matched = true;
                }
            }
            if (!matched && !sep.empty()) matched = split_on(sep);
        } else {
            // 模板写成 "{title} - {artist}"
            const std::string sep = tmpl.substr(pt + 7, pa - pt - 7);
            const size_t k = sep.empty() ? std::string::npos : stem.find(sep);
            if (k != std::string::npos) {
                const std::string left = trim(stem.substr(0, k));
                const std::string right = trim(stem.substr(k + sep.size()));
                if (!left.empty() && !right.empty()) {
                    tags.title = left;
                    tags.artist = right;
                    matched = true;
                }
            }
        }
    } else if (pa != std::string::npos) {
        const std::string sep = tmpl.substr(pa + 8);
        const size_t k = stem.find(sep);
        if (k != std::string::npos && !trim(sep).empty()) {
            const std::string left = trim(stem.substr(0, k));
            const std::string right = trim(stem.substr(k + sep.size()));
            if (!left.empty() && !right.empty()) {
                tags.artist = left;
                tags.title = right;
                matched = true;
            }
        }
    }

    if (!matched) {
        tags.title = stem;  // 找不到分隔符：整串当标题
        tags.artist.clear();
    }
    return tags;
}

Quality quality_from_stream(const AudioStreamInfo& info) {
    Quality q;
    q.codec = info.codec;
    q.lossless = info.lossless;
    q.bitrate = info.bitrate;
    q.sample_rate = info.sample_rate;
    q.bit_depth = info.bit_depth;
    q.channels = info.channels;
    q.level = infer_quality_level(info.lossless, info.sample_rate, info.bit_depth, info.bitrate);
    return q;
}

}  // namespace radmusic
