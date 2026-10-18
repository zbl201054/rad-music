#include "radmusic/groupkey.hpp"

#include <cctype>
#include <cstdint>
#include <vector>

#include "radmusic/crypto.hpp"

namespace radmusic {
namespace {

struct Codepoint {
    uint32_t cp;
};

// 宽松的 UTF-8 解码：非法字节返回 0xFFFD
std::vector<uint32_t> utf8_decode(std::string_view text) {
    std::vector<uint32_t> out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        uint32_t cp = 0;
        size_t extra = 0;
        if (c < 0x80) {
            cp = c;
            extra = 0;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            extra = 3;
        } else {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        if (i + extra >= text.size()) {
            out.push_back(0xFFFD);
            break;
        }
        bool ok = true;
        for (size_t k = 1; k <= extra; ++k) {
            const unsigned char cc = static_cast<unsigned char>(text[i + k]);
            if ((cc & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        out.push_back(cp);
        i += extra + 1;
    }
    return out;
}

void utf8_append(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

bool is_open_bracket(uint32_t cp) {
    return cp == '(' || cp == 0xFF08 || cp == '[' || cp == 0x3010 || cp == '{' ||
           cp == 0x3014 || cp == 0x300A || cp == 0x3008;
}

bool is_close_bracket(uint32_t cp) {
    return cp == ')' || cp == 0xFF09 || cp == ']' || cp == 0x3011 || cp == '}' ||
           cp == 0x3015 || cp == 0x300B || cp == 0x3009;
}

bool keep_codepoint(uint32_t cp) {
    if (cp >= '0' && cp <= '9') return true;
    if (cp >= 'a' && cp <= 'z') return true;
    if (cp >= 'A' && cp <= 'Z') return true;
    if (cp >= 0x3400 && cp <= 0x4DBF) return true;   // CJK 扩展 A
    if (cp >= 0x4E00 && cp <= 0x9FFF) return true;   // CJK 基本区
    if (cp >= 0xF900 && cp <= 0xFAFF) return true;   // CJK 兼容
    if (cp >= 0x3040 && cp <= 0x30FF) return true;   // 日文假名
    if (cp >= 0xAC00 && cp <= 0xD7AF) return true;   // 韩文音节
    return false;
}

uint32_t to_halfwidth(uint32_t cp) {
    if (cp == 0x3000) return ' ';
    if (cp >= 0xFF01 && cp <= 0xFF5E) return cp - 0xFEE0;
    return cp;
}

bool is_reserved_name(const std::string& upper) {
    static const char* kReserved[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3",
                                      "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
                                      "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6",
                                      "LPT7", "LPT8", "LPT9"};
    for (const char* r : kReserved) {
        if (upper == r) return true;
    }
    return false;
}

std::string to_upper_ascii(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) out.push_back(static_cast<char>(std::toupper(c)));
    return out;
}

// 按 UTF-8 码点边界截断到 max_bytes
std::string truncate_utf8(const std::string& text, size_t max_bytes) {
    if (text.size() <= max_bytes) return text;
    size_t cut = max_bytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
    return text.substr(0, cut);
}

}  // namespace

std::string normalize_text(std::string_view text) {
    const std::vector<uint32_t> cps = utf8_decode(text);
    std::string out;
    int depth = 0;
    bool pending_space = false;
    for (uint32_t raw : cps) {
        uint32_t cp = to_halfwidth(raw);
        if (cp == 0xFFFD) continue;

        if (is_open_bracket(cp)) {
            ++depth;
            continue;
        }
        if (is_close_bracket(cp)) {
            if (depth > 0) --depth;
            continue;
        }
        if (depth > 0) continue;

        if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '-' || cp == '_' ||
            cp == '.' || cp == ',' || cp == 0x3001 || cp == 0x3002 || cp == '!' || cp == '?' ||
            cp == 0x2018 || cp == 0x2019 || cp == 0x201C || cp == 0x201D) {
            if (!out.empty()) pending_space = true;
            continue;
        }

        if (cp >= 'A' && cp <= 'Z') cp = cp - 'A' + 'a';
        if (!keep_codepoint(cp)) continue;

        if (pending_space && !out.empty()) out.push_back(' ');
        pending_space = false;
        utf8_append(out, cp);
    }
    return out;
}

std::string group_key(std::string_view title, std::string_view primary_artist, int duration_seconds) {
    std::string out = normalize_text(title);
    out.push_back('|');
    out += normalize_text(primary_artist);
    out.push_back('|');
    out += std::to_string(duration_seconds < 0 ? 0 : duration_seconds);
    return out;
}

std::string group_key(const SongIdentity& identity) {
    return group_key(identity.title, identity.artist, identity.duration_seconds);
}

bool parse_group_key(std::string_view key, SongIdentity* out) {
    const size_t first = key.find('|');
    if (first == std::string_view::npos) return false;
    const size_t second = key.find('|', first + 1);
    if (second == std::string_view::npos) return false;
    if (!out) return true;
    out->title = std::string(key.substr(0, first));
    out->artist = std::string(key.substr(first + 1, second - first - 1));
    int duration = 0;
    for (size_t i = second + 1; i < key.size(); ++i) {
        const char c = key[i];
        if (c < '0' || c > '9') return false;
        duration = duration * 10 + (c - '0');
    }
    out->duration_seconds = duration;
    return true;
}

bool songs_fold_together(const SongIdentity& a, const SongIdentity& b) {
    if (normalize_text(a.title) != normalize_text(b.title)) return false;
    if (normalize_text(a.artist) != normalize_text(b.artist)) return false;
    int diff = a.duration_seconds - b.duration_seconds;
    if (diff < 0) diff = -diff;
    return diff <= kDurationToleranceSeconds;
}

bool group_keys_fold_together(std::string_view a, std::string_view b) {
    SongIdentity ia;
    SongIdentity ib;
    if (!parse_group_key(a, &ia) || !parse_group_key(b, &ib)) return false;
    return songs_fold_together(ia, ib);
}

std::string fs_sanitize(std::string_view text, size_t max_bytes) {
    const std::vector<uint32_t> cps = utf8_decode(text);
    std::string out;
    bool last_space = false;
    for (uint32_t cp : cps) {
        if (cp == 0xFFFD) continue;
        const bool illegal = cp < 0x20 || cp == '\\' || cp == '/' || cp == ':' || cp == '*' ||
                             cp == '?' || cp == '"' || cp == '<' || cp == '>' || cp == '|' ||
                             cp == 0x7F;
        if (illegal) {
            if (!out.empty() && !last_space) {
                out.push_back('_');
                last_space = true;
            }
            continue;
        }
        if (cp == ' ' || cp == '\t') {
            if (!out.empty() && !last_space) {
                out.push_back(' ');
                last_space = true;
            }
            continue;
        }
        last_space = false;
        utf8_append(out, cp);
    }
    // 去掉结尾的点与空格（Windows 不允许）
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    out = truncate_utf8(out, max_bytes);
    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (out.empty()) out = "unknown";
    if (is_reserved_name(to_upper_ascii(out))) out = "_" + out;
    return out;
}

std::string song_key(std::string_view title, std::string_view primary_artist, int duration_seconds) {
    const std::string key = group_key(title, primary_artist, duration_seconds);
    std::string slug = fs_sanitize(title, 40);
    const std::string artist = fs_sanitize(primary_artist, 24);
    if (!artist.empty() && artist != "unknown") slug += " - " + artist;
    slug = truncate_utf8(fs_sanitize(slug, kSongKeySlugMaxBytes), kSongKeySlugMaxBytes);
    const std::string hash = crypto::sha256_hex(key).substr(0, kSongKeyHashHex);
    return slug + "_" + hash;
}

std::string song_key(const SongIdentity& identity) {
    return song_key(identity.title, identity.artist, identity.duration_seconds);
}

std::string fallback_song_key(std::string_view platform, std::string_view item_id) {
    const std::string hash =
        crypto::sha256_hex(std::string(platform) + ":" + std::string(item_id)).substr(0, kSongKeyHashHex);
    return fs_sanitize(platform, 24) + "_" + hash;
}

bool song_key_consistent(std::string_view dir_name, std::string_view title,
                         std::string_view primary_artist, int duration_seconds) {
    if (dir_name.empty()) return false;
    if (dir_name.rfind("online_", 0) == 0) {
        // 兜底键无法由元数据反推，交由调用方用 platform/id 复核
        return true;
    }
    return song_key(title, primary_artist, duration_seconds) == dir_name;
}

}  // namespace radmusic
