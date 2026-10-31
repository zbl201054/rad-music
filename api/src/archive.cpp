// Rad music API - 归档解压实现（inflate / zip / tar.gz，零第三方依赖）
//
// inflate 按 RFC 1951 的 canonical Huffman 解码实现（stored / fixed / dynamic 三种块）。
// zip 走中央目录定位 local header；tar 只看普通文件与目录，链接类条目一律跳过。
#include "radmusic/archive.hpp"

#include <cstring>
#include <ctime>
#include <fstream>
#include <vector>

namespace radmusic {
namespace archive {
namespace {

constexpr size_t kMaxInflateBytes = 512ull * 1024 * 1024;   // 解压炸弹上限（发布包 < 20MB）

uint16_t rd16(const unsigned char* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t rd32(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

bool read_file(const std::filesystem::path& path, std::vector<unsigned char>& out, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error != nullptr) *error = "无法读取归档文件: " + std::string(path.u8string().c_str());
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(0, std::ios::beg);
    if (size < 0) {
        if (error != nullptr) *error = "无法读取归档文件长度";
        return false;
    }
    out.resize(static_cast<size_t>(size));
    if (size > 0) in.read(reinterpret_cast<char*>(out.data()), size);
    if (!in) {
        if (error != nullptr) *error = "归档文件读取不完整";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- inflate

struct BitReader {
    const unsigned char* data = nullptr;
    size_t size = 0;
    size_t pos = 0;
    unsigned long buf = 0;   // 尚未消费的位（LSB first）
    int cnt = 0;             // buf 中的有效位数

    // 取 need 位；输入耗尽返回 -1
    int bits(int need) {
        unsigned long value = buf;
        while (cnt < need) {
            if (pos >= size) return -1;
            value |= static_cast<unsigned long>(data[pos++]) << cnt;
            cnt += 8;
        }
        buf = value >> need;
        cnt -= need;
        return static_cast<int>(value & ((1UL << need) - 1));
    }
};

struct Huffman {
    short count[16] = {0};
    short symbol[288] = {0};
};

bool construct(Huffman& h, const short* length, int n) {
    for (int len = 0; len <= 15; ++len) h.count[len] = 0;
    for (int s = 0; s < n; ++s) h.count[length[s]]++;
    if (h.count[0] == n) return true;   // 无符号（空树）
    int left = 1;
    for (int len = 1; len <= 15; ++len) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) return false;     // 过度订阅
    }
    short offsets[16] = {0};
    for (int len = 1; len < 15; ++len) offsets[len + 1] = static_cast<short>(offsets[len] + h.count[len]);
    for (int s = 0; s < n; ++s) {
        if (length[s] != 0) h.symbol[offsets[length[s]]++] = static_cast<short>(s);
    }
    return true;
}

int decode_symbol(BitReader& br, const Huffman& h) {
    int code = 0;
    int first = 0;
    int index = 0;
    for (int len = 1; len <= 15; ++len) {
        const int bit = br.bits(1);
        if (bit < 0) return -1;
        code |= bit;
        const int count = h.count[len];
        if (code - first < count) return h.symbol[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

const short kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                            31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const short kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                             2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const short kDistBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,    25,
                             33,   49,   65,   97,   129,  193,   257,   385,   513,   769,
                             1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577};
const short kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                              6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool inflate_codes(BitReader& br, const Huffman& lencode, const Huffman& distcode,
                   std::vector<unsigned char>& out, size_t limit) {
    for (;;) {
        const int symbol = decode_symbol(br, lencode);
        if (symbol < 0) return false;
        if (symbol < 256) {
            if (out.size() >= limit) return false;
            out.push_back(static_cast<unsigned char>(symbol));
            continue;
        }
        if (symbol == 256) return true;   // end of block
        const int len_index = symbol - 257;
        if (len_index >= 29) return false;
        int extra = br.bits(kLenExtra[len_index]);
        if (extra < 0) return false;
        const int length = kLenBase[len_index] + extra;
        const int dist_symbol = decode_symbol(br, distcode);
        if (dist_symbol < 0 || dist_symbol >= 30) return false;
        extra = br.bits(kDistExtra[dist_symbol]);
        if (extra < 0) return false;
        const size_t distance = static_cast<size_t>(kDistBase[dist_symbol] + extra);
        if (distance == 0 || distance > out.size()) return false;
        if (out.size() + static_cast<size_t>(length) > limit) return false;
        const size_t from = out.size() - distance;
        for (int i = 0; i < length; ++i) out.push_back(out[from + static_cast<size_t>(i)]);
    }
}

bool inflate_stored(BitReader& br, std::vector<unsigned char>& out, size_t limit) {
    br.buf = 0;
    br.cnt = 0;
    if (br.pos + 4 > br.size) return false;
    const unsigned len = rd16(br.data + br.pos);
    const unsigned nlen = rd16(br.data + br.pos + 2);
    br.pos += 4;
    if (len != (~nlen & 0xFFFFu)) return false;
    if (br.pos + len > br.size) return false;
    if (out.size() + len > limit) return false;
    out.insert(out.end(), br.data + br.pos, br.data + br.pos + len);
    br.pos += len;
    return true;
}

bool inflate_fixed(BitReader& br, std::vector<unsigned char>& out, size_t limit) {
    short lengths[288];
    int i = 0;
    for (; i < 144; ++i) lengths[i] = 8;
    for (; i < 256; ++i) lengths[i] = 9;
    for (; i < 280; ++i) lengths[i] = 7;
    for (; i < 288; ++i) lengths[i] = 8;
    Huffman lencode;
    if (!construct(lencode, lengths, 288)) return false;
    short dist_lengths[30];
    for (i = 0; i < 30; ++i) dist_lengths[i] = 5;
    Huffman distcode;
    if (!construct(distcode, dist_lengths, 30)) return false;
    return inflate_codes(br, lencode, distcode, out, limit);
}

bool inflate_dynamic(BitReader& br, std::vector<unsigned char>& out, size_t limit) {
    static const short kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    int nlen = br.bits(5);
    if (nlen < 0) return false;
    nlen += 257;
    int ndist = br.bits(5);
    if (ndist < 0) return false;
    ndist += 1;
    int ncode = br.bits(4);
    if (ncode < 0) return false;
    ncode += 4;
    if (nlen > 286 || ndist > 30) return false;

    short lengths[320] = {0};
    for (int i = 0; i < ncode; ++i) {
        const int value = br.bits(3);
        if (value < 0) return false;
        lengths[kOrder[i]] = static_cast<short>(value);
    }
    Huffman lencode;
    if (!construct(lencode, lengths, 19)) return false;

    int index = 0;
    while (index < nlen + ndist) {
        const int symbol = decode_symbol(br, lencode);
        if (symbol < 0) return false;
        if (symbol < 16) {
            lengths[index++] = static_cast<short>(symbol);
            continue;
        }
        short value = 0;
        int repeat = 0;
        if (symbol == 16) {
            if (index == 0) return false;
            value = lengths[index - 1];
            const int extra = br.bits(2);
            if (extra < 0) return false;
            repeat = 3 + extra;
        } else if (symbol == 17) {
            const int extra = br.bits(3);
            if (extra < 0) return false;
            repeat = 3 + extra;
        } else {
            const int extra = br.bits(7);
            if (extra < 0) return false;
            repeat = 11 + extra;
        }
        if (index + repeat > nlen + ndist) return false;
        while (repeat-- > 0) lengths[index++] = value;
    }
    if (lengths[256] == 0) return false;

    Huffman distcode;
    if (!construct(lencode, lengths, nlen)) return false;
    if (!construct(distcode, lengths + nlen, ndist)) return false;
    return inflate_codes(br, lencode, distcode, out, limit);
}

bool inflate_stream(BitReader& br, std::vector<unsigned char>& out, size_t limit) {
    int last = 0;
    while (last == 0) {
        last = br.bits(1);
        if (last < 0) return false;
        const int type = br.bits(2);
        if (type < 0) return false;
        bool ok = false;
        if (type == 0) {
            ok = inflate_stored(br, out, limit);
        } else if (type == 1) {
            ok = inflate_fixed(br, out, limit);
        } else if (type == 2) {
            ok = inflate_dynamic(br, out, limit);
        } else {
            return false;   // 保留类型
        }
        if (!ok) return false;
    }
    return true;
}

// ---------------------------------------------------------------- 路径安全

// 分块 CRC（流式写 zip 时先扫一遍算校验值，避免把整个文件读进内存）
uint32_t crc32_update(uint32_t crc, const unsigned char* data, size_t length) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        ready = true;
    }
    for (size_t i = 0; i < length; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

uint32_t crc32_of(const unsigned char* data, size_t length) {
    return crc32_update(0xFFFFFFFFu, data, length) ^ 0xFFFFFFFFu;
}

std::filesystem::path utf8_path(const std::string& text) {
    // libstdc++ / libc++ 的 path 窄字符接口按 UTF-8 解释
    return std::filesystem::path(text);
}

std::string normalized_relative(std::string name) {
    for (char& c : name) {
        if (c == '\\') c = '/';
    }
    while (name.rfind("./", 0) == 0) name = name.substr(2);
    while (!name.empty() && name.front() == '/') name.erase(name.begin());
    return name;
}

std::string strip_root_prefix(const std::string& name, const std::string& root) {
    if (root.empty()) return name;
    const std::string prefix = root + "/";
    if (name == root) return std::string();
    if (name.rfind(prefix, 0) == 0) return name.substr(prefix.size());
    return name;
}

bool write_entry(const std::filesystem::path& dest_dir, const std::string& name,
                 const std::vector<unsigned char>& data) {
    const std::filesystem::path target = dest_dir / utf8_path(name);
    std::error_code code;
    std::filesystem::create_directories(target.parent_path(), code);
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    if (!data.empty()) out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    out.flush();
    return out.good();
}

// ---------------------------------------------------------------- tar

long long parse_octal(const unsigned char* p, size_t length) {
    long long value = 0;
    size_t i = 0;
    while (i < length && (p[i] == ' ' || p[i] == 0)) ++i;
    for (; i < length; ++i) {
        if (p[i] < '0' || p[i] > '7') break;
        value = value * 8 + (p[i] - '0');
    }
    return value;
}

std::string cstr(const unsigned char* p, size_t length) {
    size_t len = 0;
    while (len < length && p[len] != 0) ++len;
    return std::string(reinterpret_cast<const char*>(p), len);
}

}  // namespace

bool inflate_raw(const unsigned char* in, size_t in_size, std::vector<unsigned char>& out, std::string* error) {
    out.clear();
    BitReader reader;
    reader.data = in;
    reader.size = in_size;
    if (!inflate_stream(reader, out, kMaxInflateBytes)) {
        if (error != nullptr) *error = "deflate 数据解压失败（数据损坏或格式不支持）";
        return false;
    }
    return true;
}

bool gunzip(const std::vector<unsigned char>& in, std::vector<unsigned char>& out, std::string* error) {
    out.clear();
    if (in.size() < 18 || in[0] != 0x1F || in[1] != 0x8B) {
        if (error != nullptr) *error = "不是 gzip 数据";
        return false;
    }
    if (in[2] != 8) {
        if (error != nullptr) *error = "gzip 压缩方法不是 deflate";
        return false;
    }
    const unsigned flags = in[3];
    size_t pos = 10;
    if (flags & 0x04) {   // FEXTRA
        if (pos + 2 > in.size()) return false;
        const size_t extra = rd16(in.data() + pos);
        pos += 2 + extra;
    }
    if (flags & 0x08) {   // FNAME
        while (pos < in.size() && in[pos] != 0) ++pos;
        ++pos;
    }
    if (flags & 0x10) {   // FCOMMENT
        while (pos < in.size() && in[pos] != 0) ++pos;
        ++pos;
    }
    if (flags & 0x02) pos += 2;   // FHCRC
    if (pos >= in.size()) {
        if (error != nullptr) *error = "gzip 头部不完整";
        return false;
    }
    BitReader reader;
    reader.data = in.data() + pos;
    reader.size = in.size() - pos;
    if (!inflate_stream(reader, out, kMaxInflateBytes)) {
        if (error != nullptr) *error = "gzip 解压失败（数据损坏）";
        return false;
    }
    return true;
}

bool is_safe_relative_path(const std::string& path) {
    if (path.empty() || path.size() > 1024) return false;
    if (path.find('\0') != std::string::npos) return false;
    if (path.front() == '/' || path.front() == '\\') return false;
    if (path.size() >= 2 && path[1] == ':') return false;
    const std::string name = normalized_relative(path);
    if (name.empty()) return false;
    size_t i = 0;
    while (i <= name.size()) {
        size_t end = name.find('/', i);
        if (end == std::string::npos) end = name.size();
        const std::string segment = name.substr(i, end - i);
        if (segment == "..") return false;
        if (end == name.size()) break;
        i = end + 1;
    }
    return true;
}

ExtractResult extract_zip(const std::filesystem::path& zip_path, const std::filesystem::path& dest_dir,
                          const ExtractOptions& options) {
    ExtractResult result;
    std::vector<unsigned char> data;
    if (!read_file(zip_path, data, &result.error)) return result;
    if (data.size() < 22) {
        result.error = "zip 文件太小，不是合法归档";
        return result;
    }

    // 结尾记录（EOCD）：签名 0x06054b50，最多带 65535 字节注释
    size_t eocd = std::string::npos;
    const size_t limit = data.size() > 65557 ? data.size() - 65557 : 0;
    for (size_t i = data.size() - 22;; --i) {
        if (rd32(data.data() + i) == 0x06054b50u) {
            eocd = i;
            break;
        }
        if (i == limit) break;
    }
    if (eocd == std::string::npos) {
        result.error = "找不到 zip 中央目录（文件损坏或不支持的格式）";
        return result;
    }
    const unsigned count = rd16(data.data() + eocd + 10);
    size_t cursor = rd32(data.data() + eocd + 16);
    if (count == 0) {
        result.error = "zip 归档为空";
        return result;
    }

    struct Entry {
        std::string name;
        uint16_t method = 0;
        uint32_t crc = 0;
        uint32_t packed = 0;
        uint32_t unpacked = 0;
        uint32_t local = 0;
    };
    std::vector<Entry> entries;
    entries.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        if (cursor + 46 > data.size() || rd32(data.data() + cursor) != 0x02014b50u) {
            result.error = "zip 中央目录损坏";
            return result;
        }
        Entry entry;
        entry.method = rd16(data.data() + cursor + 10);
        entry.crc = rd32(data.data() + cursor + 16);
        entry.packed = rd32(data.data() + cursor + 20);
        entry.unpacked = rd32(data.data() + cursor + 24);
        entry.local = rd32(data.data() + cursor + 42);
        const size_t name_len = rd16(data.data() + cursor + 28);
        const size_t extra_len = rd16(data.data() + cursor + 30);
        const size_t comment_len = rd16(data.data() + cursor + 32);
        if (cursor + 46 + name_len > data.size()) {
            result.error = "zip 中央目录条目越界";
            return result;
        }
        entry.name.assign(reinterpret_cast<const char*>(data.data() + cursor + 46), name_len);
        entries.push_back(std::move(entry));
        cursor += 46 + name_len + extra_len + comment_len;
    }

    // 发布包顶层统一是一个目录：所有条目共享同一段时去掉它
    std::string root;
    if (options.strip_single_root) {
        for (const auto& entry : entries) {
            std::string name = normalized_relative(entry.name);
            const size_t slash = name.find('/');
            if (slash == std::string::npos || slash == 0) {
                root.clear();
                break;
            }
            const std::string segment = name.substr(0, slash);
            if (segment == "." || segment == "..") {
                root.clear();
                break;
            }
            if (root.empty()) root = segment;
            else if (root != segment) {
                root.clear();
                break;
            }
        }
    }

    std::error_code code;
    std::filesystem::create_directories(dest_dir, code);

    for (const auto& entry : entries) {
        const std::string name = strip_root_prefix(normalized_relative(entry.name), root);
        if (name.empty() || name.back() == '/') continue;   // 目录条目：由文件路径自动创建
        if (!is_safe_relative_path(name)) {
            result.skipped++;
            continue;
        }
        if (entry.local + 30 > data.size() || rd32(data.data() + entry.local) != 0x04034b50u) {
            result.error = "zip 条目头损坏: " + name;
            return result;
        }
        const size_t local_name_len = rd16(data.data() + entry.local + 26);
        const size_t local_extra_len = rd16(data.data() + entry.local + 28);
        const size_t start = static_cast<size_t>(entry.local) + 30 + local_name_len + local_extra_len;
        if (start + entry.packed > data.size()) {
            result.error = "zip 条目数据越界: " + name;
            return result;
        }

        std::vector<unsigned char> content;
        if (entry.method == 0) {
            content.assign(data.begin() + static_cast<std::ptrdiff_t>(start),
                           data.begin() + static_cast<std::ptrdiff_t>(start + entry.packed));
        } else if (entry.method == 8) {
            std::string error;
            if (!inflate_raw(data.data() + start, entry.packed, content, &error)) {
                result.error = name + ": " + error;
                return result;
            }
            if (entry.unpacked != 0 && content.size() != entry.unpacked) {
                result.error = name + ": 解压长度与声明不一致";
                return result;
            }
        } else {
            result.skipped++;   // 不支持的压缩方法（如 zip64/bzip2）
            continue;
        }
        // 完整性校验：发布包损坏时宁可安装失败，也不能把坏文件覆盖上去
        if (crc32_of(content.data(), content.size()) != (entry.crc & 0xFFFFFFFFu)) {
            result.error = name + ": CRC 校验失败（安装包已损坏），请重新下载";
            return result;
        }
        if (!write_entry(dest_dir, name, content)) {
            result.error = "写入失败: " + name;
            return result;
        }
        result.files++;
    }
    if (result.files == 0) result.error = "归档中没有任何可解压的文件";
    return result;
}

ExtractResult extract_tar_gz(const std::filesystem::path& tar_gz_path, const std::filesystem::path& dest_dir,
                             const ExtractOptions& options) {
    ExtractResult result;
    std::vector<unsigned char> packed;
    if (!read_file(tar_gz_path, packed, &result.error)) return result;
    std::vector<unsigned char> tar;
    std::string error;
    if (!gunzip(packed, tar, &error)) {
        result.error = error;
        return result;
    }

    struct Item {
        std::string name;
        std::vector<unsigned char> content;
    };
    std::vector<Item> items;
    std::string long_name;
    std::string pax_path;
    size_t pos = 0;
    while (pos + 512 <= tar.size()) {
        const unsigned char* header = tar.data() + pos;
        // 连续两个全零块表示结束
        bool zero = true;
        for (size_t i = 0; i < 512; ++i) {
            if (header[i] != 0) {
                zero = false;
                break;
            }
        }
        if (zero) break;

        const std::string name = cstr(header, 100);
        const std::string prefix = cstr(header + 345, 155);
        const long long size = parse_octal(header + 124, 12);
        const char type = static_cast<char>(header[156]);
        if (size < 0) {
            result.error = "tar 条目长度非法";
            return result;
        }
        const size_t data_start = pos + 512;
        const size_t data_end = data_start + static_cast<size_t>(size);
        if (data_end > tar.size()) {
            result.error = "tar 条目越界: " + name;
            return result;
        }
        const size_t next = data_start + ((static_cast<size_t>(size) + 511) / 512) * 512;

        if (type == 'L') {   // GNU 长文件名
            long_name.assign(reinterpret_cast<const char*>(tar.data() + data_start), static_cast<size_t>(size));
            while (!long_name.empty() && long_name.back() == 0) long_name.pop_back();
            pos = next;
            continue;
        }
        if (type == 'x') {   // pax 扩展头：抓 path=
            const std::string body(reinterpret_cast<const char*>(tar.data() + data_start), static_cast<size_t>(size));
            size_t at = 0;
            while (at < body.size()) {
                const size_t space = body.find(' ', at);
                if (space == std::string::npos) break;
                const size_t newline = body.find('\n', space);
                if (newline == std::string::npos) break;
                const std::string record = body.substr(space + 1, newline - space - 1);
                if (record.rfind("path=", 0) == 0) pax_path = record.substr(5);
                at = newline + 1;
            }
            pos = next;
            continue;
        }
        if (type == 'g') {   // 全局 pax：忽略
            pos = next;
            continue;
        }

        std::string full = name;
        if (!pax_path.empty()) full = pax_path;
        else if (!long_name.empty()) full = long_name;
        else if (!prefix.empty()) full = prefix + "/" + name;
        pax_path.clear();
        long_name.clear();

        if (type == '0' || type == 0 || type == '7') {
            Item item;
            item.name = full;
            item.content.assign(tar.begin() + static_cast<std::ptrdiff_t>(data_start),
                                tar.begin() + static_cast<std::ptrdiff_t>(data_end));
            items.push_back(std::move(item));
        } else {
            result.skipped++;   // 目录 / 链接 / 设备：不落盘
        }
        pos = next;
    }

    std::string root;
    if (options.strip_single_root) {
        for (const auto& item : items) {
            const std::string name = normalized_relative(item.name);
            const size_t slash = name.find('/');
            if (slash == std::string::npos || slash == 0) {
                root.clear();
                break;
            }
            const std::string segment = name.substr(0, slash);
            if (segment == "." || segment == "..") {
                root.clear();
                break;
            }
            if (root.empty()) root = segment;
            else if (root != segment) {
                root.clear();
                break;
            }
        }
    }

    std::error_code code;
    std::filesystem::create_directories(dest_dir, code);
    for (const auto& item : items) {
        const std::string name = strip_root_prefix(normalized_relative(item.name), root);
        if (name.empty()) continue;
        if (!is_safe_relative_path(name)) {
            result.skipped++;
            continue;
        }
        if (!write_entry(dest_dir, name, item.content)) {
            result.error = "写入失败: " + name;
            return result;
        }
        result.files++;
    }
    if (result.files == 0) result.error = "归档中没有任何可解压的文件";
    return result;
}

ExtractResult extract_archive(const std::filesystem::path& path, const std::filesystem::path& dest_dir,
                              const ExtractOptions& options) {
    const std::string name = std::string(path.filename().u8string().c_str());
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".zip") == 0) {
        return extract_zip(path, dest_dir, options);
    }
    if (name.size() > 7 && name.compare(name.size() - 7, 7, ".tar.gz") == 0) {
        return extract_tar_gz(path, dest_dir, options);
    }
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".tgz") == 0) {
        return extract_tar_gz(path, dest_dir, options);
    }
    ExtractResult result;
    result.error = "不支持的归档格式: " + name;
    return result;
}

// ---------------------------------------------------------------- 流式 zip 解压

ExtractResult extract_zip_streamed(const std::filesystem::path& zip_path, const std::filesystem::path& dest_dir,
                                   const ExtractOptions& options) {
    ExtractResult result;
    std::ifstream in(zip_path, std::ios::binary);
    if (!in) {
        result.error = "无法读取归档文件: " + std::string(zip_path.u8string().c_str());
        return result;
    }
    in.seekg(0, std::ios::end);
    const long long file_size = static_cast<long long>(in.tellg());
    if (file_size < 22) {
        result.error = "zip 文件太小，不是合法归档";
        return result;
    }

    // 结尾记录（EOCD）在文件末尾 ≤64KiB 的范围内，先只读这一段
    const size_t tail_len = static_cast<size_t>(file_size < 65557 ? file_size : 65557);
    std::vector<unsigned char> tail(tail_len);
    in.seekg(file_size - static_cast<long long>(tail_len), std::ios::beg);
    in.read(reinterpret_cast<char*>(tail.data()), static_cast<std::streamsize>(tail_len));
    if (!in) {
        result.error = "读取 zip 结尾记录失败";
        return result;
    }
    size_t eocd = std::string::npos;
    for (size_t i = tail_len - 22;; --i) {
        if (rd32(tail.data() + i) == 0x06054b50u) {
            eocd = i;
            break;
        }
        if (i == 0) break;
    }
    if (eocd == std::string::npos) {
        result.error = "找不到 zip 中央目录（文件损坏或不支持的格式）";
        return result;
    }
    const unsigned count = rd16(tail.data() + eocd + 10);
    const uint32_t cd_offset = rd32(tail.data() + eocd + 16);
    if (count == 0) {
        result.error = "zip 归档为空";
        return result;
    }
    if (static_cast<long long>(cd_offset) >= file_size) {
        result.error = "zip 中央目录偏移越界";
        return result;
    }

    std::vector<unsigned char> cd(static_cast<size_t>(file_size - static_cast<long long>(cd_offset)));
    in.clear();
    in.seekg(static_cast<std::streamoff>(cd_offset), std::ios::beg);
    in.read(reinterpret_cast<char*>(cd.data()), static_cast<std::streamsize>(cd.size()));
    if (static_cast<size_t>(in.gcount()) != cd.size()) {
        result.error = "读取 zip 中央目录失败";
        return result;
    }

    struct Entry {
        std::string name;
        uint16_t method = 0;
        uint32_t crc = 0;
        uint32_t packed = 0;
        uint32_t unpacked = 0;
        uint32_t local = 0;
    };
    std::vector<Entry> entries;
    entries.reserve(count);
    size_t cursor = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (cursor + 46 > cd.size() || rd32(cd.data() + cursor) != 0x02014b50u) {
            result.error = "zip 中央目录损坏";
            return result;
        }
        Entry entry;
        entry.method = rd16(cd.data() + cursor + 10);
        entry.crc = rd32(cd.data() + cursor + 16);
        entry.packed = rd32(cd.data() + cursor + 20);
        entry.unpacked = rd32(cd.data() + cursor + 24);
        entry.local = rd32(cd.data() + cursor + 42);
        const size_t name_len = rd16(cd.data() + cursor + 28);
        const size_t extra_len = rd16(cd.data() + cursor + 30);
        const size_t comment_len = rd16(cd.data() + cursor + 32);
        if (cursor + 46 + name_len > cd.size()) {
            result.error = "zip 中央目录条目越界";
            return result;
        }
        entry.name.assign(reinterpret_cast<const char*>(cd.data() + cursor + 46), name_len);
        entries.push_back(std::move(entry));
        cursor += 46 + name_len + extra_len + comment_len;
    }

    std::string root;
    if (options.strip_single_root) {
        for (const auto& entry : entries) {
            std::string name = normalized_relative(entry.name);
            const size_t slash = name.find('/');
            if (slash == std::string::npos || slash == 0) {
                root.clear();
                break;
            }
            const std::string segment = name.substr(0, slash);
            if (segment == "." || segment == "..") {
                root.clear();
                break;
            }
            if (root.empty()) root = segment;
            else if (root != segment) {
                root.clear();
                break;
            }
        }
    }

    std::error_code code;
    std::filesystem::create_directories(dest_dir, code);

    std::vector<char> chunk(256 * 1024);
    for (const auto& entry : entries) {
        const std::string name = strip_root_prefix(normalized_relative(entry.name), root);
        if (name.empty() || name.back() == '/') continue;   // 目录条目：由文件路径自动创建
        if (!is_safe_relative_path(name)) {
            result.skipped++;
            continue;
        }
        if (entry.method != 0 && entry.method != 8) {
            result.skipped++;   // unsupported method（zip64 / bzip2 …）
            continue;
        }

        in.clear();
        in.seekg(static_cast<std::streamoff>(entry.local), std::ios::beg);
        unsigned char lh[30];
        in.read(reinterpret_cast<char*>(lh), 30);
        if (!in || rd32(lh) != 0x04034b50u) {
            result.error = "zip 条目头损坏: " + name;
            return result;
        }
        const size_t local_name_len = rd16(lh + 26);
        const size_t local_extra_len = rd16(lh + 28);
        const long long start = static_cast<long long>(entry.local) + 30 +
                                static_cast<long long>(local_name_len) + static_cast<long long>(local_extra_len);
        if (start + static_cast<long long>(entry.packed) > file_size) {
            result.error = "zip 条目数据越界: " + name;
            return result;
        }

        const std::filesystem::path target = dest_dir / utf8_path(name);
        std::filesystem::create_directories(target.parent_path(), code);
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) {
            result.error = "写入失败: " + name;
            return result;
        }

        uint32_t crc = 0xFFFFFFFFu;
        in.seekg(static_cast<std::streamoff>(start), std::ios::beg);
        if (entry.method == 0) {
            uint32_t remaining = entry.packed;
            while (remaining > 0) {
                const size_t want = remaining < chunk.size() ? static_cast<size_t>(remaining) : chunk.size();
                in.read(chunk.data(), static_cast<std::streamsize>(want));
                const size_t got = static_cast<size_t>(in.gcount());
                if (got != want) {
                    out.close();
                    std::filesystem::remove(target, code);
                    result.error = "zip 条目读取不完整: " + name;
                    return result;
                }
                crc = crc32_update(crc, reinterpret_cast<const unsigned char*>(chunk.data()), got);
                out.write(chunk.data(), static_cast<std::streamsize>(got));
                remaining -= static_cast<uint32_t>(got);
            }
        } else {
            std::vector<unsigned char> packed(entry.packed);
            if (entry.packed > 0) in.read(reinterpret_cast<char*>(packed.data()), static_cast<std::streamsize>(packed.size()));
            if (static_cast<size_t>(in.gcount()) != packed.size()) {
                out.close();
                std::filesystem::remove(target, code);
                result.error = "zip 条目读取不完整: " + name;
                return result;
            }
            std::vector<unsigned char> content;
            std::string inflate_error;
            if (!inflate_raw(packed.data(), packed.size(), content, &inflate_error)) {
                out.close();
                std::filesystem::remove(target, code);
                result.error = name + ": " + inflate_error;
                return result;
            }
            if (entry.unpacked != 0 && content.size() != entry.unpacked) {
                out.close();
                std::filesystem::remove(target, code);
                result.error = name + ": 解压长度与声明不一致";
                return result;
            }
            crc = crc32_update(crc, content.data(), content.size());
            if (!content.empty()) out.write(reinterpret_cast<const char*>(content.data()),
                                            static_cast<std::streamsize>(content.size()));
        }
        out.flush();
        if (!out.good()) {
            out.close();
            std::filesystem::remove(target, code);
            result.error = "写入失败: " + name;
            return result;
        }
        out.close();
        if ((crc ^ 0xFFFFFFFFu) != (entry.crc & 0xFFFFFFFFu)) {
            std::filesystem::remove(target, code);
            result.error = name + ": CRC 校验失败（文件已损坏）";
            return result;
        }
        result.files++;
    }
    if (result.files == 0) result.error = "归档中没有任何可解压的文件";
    return result;
}

// ---------------------------------------------------------------- zip 写入

namespace {

void append16(std::string& out, uint16_t value) {
    out.push_back(static_cast<char>(value & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
}

void append32(std::string& out, uint32_t value) {
    out.push_back(static_cast<char>(value & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>((value >> 16) & 0xFF));
    out.push_back(static_cast<char>((value >> 24) & 0xFF));
}

// MS-DOS 时间戳（zip 头用）：本地时间，2 秒精度
void dos_datetime(uint16_t* out_time, uint16_t* out_date) {
    const std::time_t now = std::time(nullptr);
    std::tm parts{};
#ifdef _WIN32
    localtime_s(&parts, &now);
#else
    localtime_r(&now, &parts);
#endif
    if (parts.tm_year < 80) {
        *out_time = 0;
        *out_date = (1 << 5) | 1;   // 1980-01-01
        return;
    }
    const int year = parts.tm_year + 1900 < 1980 ? 1980 : parts.tm_year + 1900;
    *out_time = static_cast<uint16_t>((parts.tm_hour << 11) | (parts.tm_min << 5) | (parts.tm_sec / 2));
    *out_date = static_cast<uint16_t>(((year - 1980) << 9) | ((parts.tm_mon + 1) << 5) | parts.tm_mday);
}

}  // namespace

bool write_zip(const std::filesystem::path& target, const std::vector<ZipWriteItem>& items, std::string* error) {
    auto fail = [&](const std::string& message) {
        if (error != nullptr) *error = message;
        return false;
    };
    if (items.empty()) return fail("没有要打包的文件");

    std::error_code ec;
    const std::filesystem::path parent = target.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);

    const std::filesystem::path part(target.u8string() + ".part");
    std::ofstream out(part, std::ios::binary | std::ios::trunc);
    if (!out) return fail("无法写入归档: " + std::string(target.u8string().c_str()));

    // 失败统一清理临时文件
    auto abort_write = [&](const std::string& message) {
        out.close();
        std::error_code rm;
        std::filesystem::remove(part, rm);
        return fail(message);
    };

    uint16_t dos_time = 0;
    uint16_t dos_date = 0;
    dos_datetime(&dos_time, &dos_date);

    struct CentralEntry {
        std::string name;
        uint32_t crc = 0;
        uint32_t size = 0;
        uint32_t offset = 0;
    };
    std::vector<CentralEntry> central;
    central.reserve(items.size());
    std::vector<char> buffer(256 * 1024);
    uint64_t written = 0;

    for (const auto& item : items) {
        std::string name = item.name;
        for (char& c : name) {
            if (c == '\\') c = '/';
        }
        if (!is_safe_relative_path(name)) return abort_write("归档条目名非法: " + name);

        uint32_t crc = 0;
        uint32_t size = 0;
        std::ifstream in;
        if (item.data.empty()) {
            std::error_code size_ec;
            const auto bytes = std::filesystem::file_size(item.path, size_ec);
            if (size_ec) return abort_write("无法读取文件大小: " + std::string(item.path.u8string().c_str()));
            if (bytes > 0xFFFFFFFFull) return abort_write("单个文件超过 4 GiB，无法打包: " +
                                                          std::string(item.path.u8string().c_str()));
            size = static_cast<uint32_t>(bytes);
            in.open(item.path, std::ios::binary);
            if (!in) return abort_write("无法读取文件: " + std::string(item.path.u8string().c_str()));
            // 第一遍：只算 CRC（避免把整个音频读进内存）
            uint32_t running = 0xFFFFFFFFu;
            uint32_t read_total = 0;
            while (in) {
                in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const std::streamsize got = in.gcount();
                if (got <= 0) break;
                running = crc32_update(running, reinterpret_cast<const unsigned char*>(buffer.data()),
                                       static_cast<size_t>(got));
                read_total += static_cast<uint32_t>(got);
            }
            if (read_total != size) return abort_write("读取文件失败: " + std::string(item.path.u8string().c_str()));
            crc = running ^ 0xFFFFFFFFu;
        } else {
            if (item.data.size() > 0xFFFFFFFFull) return abort_write("条目超过 4 GiB: " + name);
            size = static_cast<uint32_t>(item.data.size());
            crc = crc32_of(reinterpret_cast<const unsigned char*>(item.data.data()), item.data.size());
        }

        std::string header;
        header.reserve(30 + name.size());
        append32(header, 0x04034b50u);
        append16(header, 20);        // version needed to extract (2.0)
        append16(header, 0x0800);    // bit 11：文件名是 UTF-8
        append16(header, 0);         // method 0 = stored
        append16(header, dos_time);
        append16(header, dos_date);
        append32(header, crc);
        append32(header, size);
        append32(header, size);
        append16(header, static_cast<uint16_t>(name.size()));
        append16(header, 0);         // extra len
        header += name;

        const uint32_t local_offset = static_cast<uint32_t>(written);
        out.write(header.data(), static_cast<std::streamsize>(header.size()));
        written += header.size();

        if (item.data.empty()) {
            in.clear();
            in.seekg(0, std::ios::beg);
            uint32_t copied = 0;
            while (copied < size) {
                const uint32_t want = (size - copied) < buffer.size() ? (size - copied)
                                                                     : static_cast<uint32_t>(buffer.size());
                in.read(buffer.data(), static_cast<std::streamsize>(want));
                const std::streamsize got = in.gcount();
                if (got <= 0) return abort_write("读取文件失败: " + std::string(item.path.u8string().c_str()));
                out.write(buffer.data(), got);
                copied += static_cast<uint32_t>(got);
            }
        } else if (!item.data.empty()) {
            out.write(item.data.data(), static_cast<std::streamsize>(item.data.size()));
        }
        written += size;

        CentralEntry entry;
        entry.name = name;
        entry.crc = crc;
        entry.size = size;
        entry.offset = local_offset;
        central.push_back(std::move(entry));
    }

    // 中央目录
    const uint64_t cd_start = written;
    std::string cd;
    for (const auto& entry : central) {
        append32(cd, 0x02014b50u);
        append16(cd, 20);            // version made by
        append16(cd, 20);            // version needed
        append16(cd, 0x0800);        // UTF-8 名字
        append16(cd, 0);             // stored
        append16(cd, dos_time);
        append16(cd, dos_date);
        append32(cd, entry.crc);
        append32(cd, entry.size);
        append32(cd, entry.size);
        append16(cd, static_cast<uint16_t>(entry.name.size()));
        append16(cd, 0);             // extra
        append16(cd, 0);             // comment
        append16(cd, 0);             // disk number
        append16(cd, 0);             // internal attrs
        append32(cd, 0);             // external attrs
        append32(cd, entry.offset);
        cd += entry.name;
    }
    out.write(cd.data(), static_cast<std::streamsize>(cd.size()));

    std::string eocd;
    append32(eocd, 0x06054b50u);
    append16(eocd, 0);
    append16(eocd, 0);
    append16(eocd, static_cast<uint16_t>(central.size()));
    append16(eocd, static_cast<uint16_t>(central.size()));
    append32(eocd, static_cast<uint32_t>(cd.size()));
    append32(eocd, static_cast<uint32_t>(cd_start));
    append16(eocd, 0);
    out.write(eocd.data(), static_cast<std::streamsize>(eocd.size()));
    out.flush();
    const bool ok = out.good();
    out.close();
    if (!ok) return abort_write("写入归档失败（磁盘空间不足？）");

    std::filesystem::remove(target, ec);
    ec.clear();
    std::filesystem::rename(part, target, ec);
    if (ec) {
        std::error_code rm;
        std::filesystem::remove(part, rm);
        return fail("归档落盘失败: " + ec.message());
    }
    return true;
}

}  // namespace archive
}  // namespace radmusic
