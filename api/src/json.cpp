// Rad music API - 极小 JSON 实现（零依赖，C++17）
//
// 严格遵循 RFC 8259：
//  * 空白仅限 space(0x20) / tab(0x09) / CR(0x0D) / LF(0x0A)；
//  * 对象与数组禁止尾随逗号；
//  * 字符串内禁止裸控制字符（< 0x20）；
//  * 转义仅允许 " \ / b f n r t 与 \uXXXX；
//  * \uXXXX 支持代理对，孤立代理按 U+FFFD 处理；
//  * 只允许一个顶层值（尾部非空白内容视为错误）。
//
// 另外：解析深度上限 200 层；整数优先，溢出/带小数点/带指数归为 Double；
// 任何失败都不抛异常，只返回 null 并填充 ParseError。

#include "radmusic/json.hpp"

#include <charconv>
#include <system_error>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace radmusic::json {
namespace {

constexpr int kMaxDepth = 200;  // 最大嵌套深度（防栈溢出）

// ---------------- 基础工具 ----------------

inline bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

inline bool is_digit(char c) { return c >= '0' && c <= '9'; }

inline bool is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

inline int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}

inline void report(ParseError* err, size_t offset, const char* message) {
    if (err != nullptr) {
        err->offset = offset;
        err->message = message;
    }
}

// 把 Unicode 码点编码为 UTF-8 追加到 out
void append_utf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

// ---------------- 解析器 ----------------

class Parser {
public:
    Parser(std::string_view text, ParseError* err) : s_(text), n_(text.size()), err_(err) {}

    bool run(Value& out) {
        skip_ws();
        if (i_ >= n_) {
            report(err_, i_, "输入为空或只包含空白");
            return false;
        }
        Value v;
        if (!parse_value(v)) return false;
        skip_ws();
        if (i_ != n_) {
            report(err_, i_, "顶层值之后还有多余内容（只允许一个顶层值）");
            return false;
        }
        out = std::move(v);
        return true;
    }

private:
    std::string_view s_;
    size_t n_ = 0;
    size_t i_ = 0;
    ParseError* err_ = nullptr;

    void skip_ws() {
        while (i_ < n_ && is_ws(s_[i_])) ++i_;
    }

    bool fail(size_t offset, const char* msg) {
        report(err_, offset, msg);
        return false;
    }

    // 字面量：期望从 i_ 开始出现 word（调用者保证首字符已匹配）
    bool parse_word(const char* word, Value literal, Value& out) {
        const size_t len = std::strlen(word);
        if (n_ - i_ < len || std::memcmp(s_.data() + i_, word, len) != 0) {
            return fail(i_, "非法字面量（仅支持 true / false / null）");
        }
        i_ += len;
        out = std::move(literal);
        return true;
    }

    bool parse_value(Value& out) {
        const char c = s_[i_];
        switch (c) {
            case 'n': return parse_word("null", Value(nullptr), out);
            case 't': return parse_word("true", Value(true), out);
            case 'f': return parse_word("false", Value(false), out);
            case '"': {
                std::string str;
                if (!parse_string(str)) return false;
                out = Value(std::move(str));
                return true;
            }
            case '[': return parse_array(out);
            case '{': return parse_object(out);
            default:
                if (c == '-' || is_digit(c)) return parse_number(out);
                return fail(i_, "无法识别的值（期望对象/数组/字符串/数字/true/false/null）");
        }
    }

    bool parse_string(std::string& out) {
        const size_t open = i_;  // 指向起始引号
        ++i_;                    // 跳过 "
        out.clear();
        while (true) {
            if (i_ >= n_) return fail(open, "字符串未闭合（缺少结束引号）");
            const unsigned char c = static_cast<unsigned char>(s_[i_]);
            if (c == '"') {
                ++i_;
                return true;
            }
            if (c == '\\') {
                if (!parse_escape(out)) return false;
                continue;
            }
            if (c < 0x20u) {
                return fail(i_, "字符串中出现未转义的控制字符（< 0x20）");
            }
            out.push_back(static_cast<char>(c));
            ++i_;
        }
    }

    // i_ 指向 '\'，返回时 i_ 已越过整个转义序列
    bool parse_escape(std::string& out) {
        const size_t esc = i_;
        ++i_;  // 跳过 '\'
        if (i_ >= n_) return fail(esc, "字符串以未完成的转义序列结尾");
        const char e = s_[i_];
        switch (e) {
            case '"': out.push_back('"'); ++i_; return true;
            case '\\': out.push_back('\\'); ++i_; return true;
            case '/': out.push_back('/'); ++i_; return true;
            case 'b': out.push_back('\b'); ++i_; return true;
            case 'f': out.push_back('\f'); ++i_; return true;
            case 'n': out.push_back('\n'); ++i_; return true;
            case 'r': out.push_back('\r'); ++i_; return true;
            case 't': out.push_back('\t'); ++i_; return true;
            case 'u': return parse_unicode_escape(esc, out);
            default:
                return fail(esc, "非法转义序列（仅允许 \\\" \\\\ \\/ \\b \\f \\n \\r \\t \\uXXXX）");
        }
    }

    bool read_hex4(size_t esc, uint32_t& cp) {
        if (n_ - i_ < 4) return fail(esc, "\\u 转义后不足 4 位十六进制数字");
        uint32_t v = 0;
        for (int k = 0; k < 4; ++k) {
            const char h = s_[i_ + static_cast<size_t>(k)];
            if (!is_hex_digit(h)) {
                return fail(i_ + static_cast<size_t>(k),
                            "\\u 转义后需要 4 位十六进制数字");
            }
            v = (v << 4) | static_cast<uint32_t>(hex_value(h));
        }
        i_ += 4;
        cp = v;
        return true;
    }

    // i_ 指向 'u'
    bool parse_unicode_escape(size_t esc, std::string& out) {
        ++i_;  // 跳过 'u'
        uint32_t cp = 0;
        if (!read_hex4(esc, cp)) return false;

        if (cp >= 0xD800u && cp <= 0xDBFFu) {  // 高代理：尝试配对低代理
            if (n_ - i_ >= 6 && s_[i_] == '\\' && s_[i_ + 1] == 'u' &&
                is_hex_digit(s_[i_ + 2]) && is_hex_digit(s_[i_ + 3]) &&
                is_hex_digit(s_[i_ + 4]) && is_hex_digit(s_[i_ + 5])) {
                // 先“窥探”第二个转义，确认是合法低代理后才消费它，
                // 否则保留给下一次转义处理（避免吞掉后续转义）
                const uint32_t low = (static_cast<uint32_t>(hex_value(s_[i_ + 2])) << 12) |
                                     (static_cast<uint32_t>(hex_value(s_[i_ + 3])) << 8) |
                                     (static_cast<uint32_t>(hex_value(s_[i_ + 4])) << 4) |
                                     static_cast<uint32_t>(hex_value(s_[i_ + 5]));
                if (low >= 0xDC00u && low <= 0xDFFFu) {
                    i_ += 6;
                    cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
                } else {
                    cp = 0xFFFDu;  // 高代理后不是低代理：本码点按 U+FFFD
                }
            } else {
                cp = 0xFFFDu;  // 孤立高代理
            }
        } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
            cp = 0xFFFDu;  // 孤立低代理
        }
        append_utf8(out, cp);
        return true;
    }

    bool parse_number(Value& out) {
        const size_t start = i_;
        if (s_[i_] == '-') {
            ++i_;
            if (i_ >= n_) return fail(start, "数字不完整：'-' 后缺少数字");
        }
        if (i_ >= n_ || !is_digit(s_[i_])) return fail(start, "数字不完整：缺少整数部分");
        if (s_[i_] == '0') {
            ++i_;
            if (i_ < n_ && is_digit(s_[i_])) {
                return fail(start, "数字格式错误：前导零不被允许");
            }
        } else {
            while (i_ < n_ && is_digit(s_[i_])) ++i_;
        }

        bool is_float = false;
        if (i_ < n_ && s_[i_] == '.') {
            is_float = true;
            ++i_;
            if (i_ >= n_ || !is_digit(s_[i_])) {
                return fail(start, "数字格式错误：小数点后缺少数字");
            }
            while (i_ < n_ && is_digit(s_[i_])) ++i_;
        }
        if (i_ < n_ && (s_[i_] == 'e' || s_[i_] == 'E')) {
            is_float = true;
            ++i_;
            if (i_ < n_ && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
            if (i_ >= n_ || !is_digit(s_[i_])) {
                return fail(start, "数字格式错误：指数部分缺少数字");
            }
            while (i_ < n_ && is_digit(s_[i_])) ++i_;
        }

        const size_t len = i_ - start;
        if (!is_float) {
            const bool negative = s_[start] == '-';
            const size_t dstart = negative ? start + 1 : start;
            const size_t ndigits = start + len - dstart;
            if (ndigits <= 19) {  // 19 位十进制可能超出 int64 范围，需逐个累加判断
                uint64_t acc = 0;
                bool overflow = false;
                const uint64_t limit = negative ? 9223372036854775808ull : 9223372036854775807ull;
                for (size_t k = 0; k < ndigits; ++k) {
                    const uint64_t digit =
                        static_cast<uint64_t>(s_[dstart + k] - '0');  // 已校验为数字
                    if (acc > (limit - digit) / 10ull) {
                        overflow = true;
                        break;
                    }
                    acc = acc * 10ull + digit;
                }
                if (!overflow) {
                    if (negative) {
                        // acc == 2^63 时 -(int64_t)acc 溢出，用补码写法避免 UB
                        const int64_t v = (acc == 9223372036854775808ull)
                                              ? (-9223372036854775807ll - 1ll)
                                              : -static_cast<int64_t>(acc);
                        out = Value(v);
                        return true;
                    }
                    out = Value(static_cast<int64_t>(acc));
                    return true;
                }
            }
            // 超出 int64 → Double（按需求回退）
        }

        // 截取 token 到独立缓冲，避免 strtod 越界读取
        std::string token(s_.substr(start, len));
        out = Value(std::strtod(token.c_str(), nullptr));
        return true;
    }

    bool parse_array(Value& out) {
        if (depth_ >= kMaxDepth) return fail(i_, "嵌套层数超过上限（200 层）");
        ++depth_;
        ++i_;  // 跳过 '['
        Value arr = Value::array();
        Array& items = arr.as_array();

        skip_ws();
        if (i_ < n_ && s_[i_] == ']') {
            ++i_;
            --depth_;
            out = std::move(arr);
            return true;
        }
        while (true) {
            skip_ws();
            if (i_ >= n_) {
                --depth_;
                return fail(i_, "数组未闭合（缺少 ']'）");
            }
            Value item;
            if (!parse_value(item)) {
                --depth_;
                return false;
            }
            items.push_back(std::move(item));
            skip_ws();
            if (i_ >= n_) {
                --depth_;
                return fail(i_, "数组未闭合（缺少 ']'）");
            }
            if (s_[i_] == ',') {
                ++i_;
                skip_ws();
                if (i_ < n_ && s_[i_] == ']') {
                    --depth_;
                    return fail(i_, "数组不允许尾随逗号");
                }
                continue;
            }
            if (s_[i_] == ']') {
                ++i_;
                --depth_;
                out = std::move(arr);
                return true;
            }
            --depth_;
            return fail(i_, "数组元素之间缺少 ',' 或 ']'");
        }
    }

    bool parse_object(Value& out) {
        if (depth_ >= kMaxDepth) return fail(i_, "嵌套层数超过上限（200 层）");
        ++depth_;
        ++i_;  // 跳过 '{'
        Value obj = Value::object();
        Object& members = obj.as_object();

        skip_ws();
        if (i_ < n_ && s_[i_] == '}') {
            ++i_;
            --depth_;
            out = std::move(obj);
            return true;
        }
        while (true) {
            skip_ws();
            if (i_ >= n_) {
                --depth_;
                return fail(i_, "对象未闭合（缺少 '}'）");
            }
            if (s_[i_] != '"') {
                --depth_;
                return fail(i_, "对象的键必须是字符串（缺少 '\"'）");
            }
            std::string key;
            if (!parse_string(key)) {
                --depth_;
                return false;
            }
            skip_ws();
            if (i_ >= n_) {
                --depth_;
                return fail(i_, "对象未闭合：键之后缺少 ':'");
            }
            if (s_[i_] != ':') {
                --depth_;
                return fail(i_, "对象的键之后缺少 ':'");
            }
            ++i_;  // 跳过 ':'
            skip_ws();
            if (i_ >= n_) {
                --depth_;
                return fail(i_, "对象未闭合：':' 之后缺少值");
            }
            Value val;
            if (!parse_value(val)) {
                --depth_;
                return false;
            }
            // 重复键：覆盖已有键的值，保留键第一次出现的位置
            bool replaced = false;
            for (auto& kv : members) {
                if (kv.first == key) {
                    kv.second = std::move(val);
                    replaced = true;
                    break;
                }
            }
            if (!replaced) members.emplace_back(std::move(key), std::move(val));

            skip_ws();
            if (i_ >= n_) {
                --depth_;
                return fail(i_, "对象未闭合（缺少 '}'）");
            }
            if (s_[i_] == ',') {
                ++i_;
                skip_ws();
                if (i_ < n_ && s_[i_] == '}') {
                    --depth_;
                    return fail(i_, "对象不允许尾随逗号");
                }
                continue;
            }
            if (s_[i_] == '}') {
                ++i_;
                --depth_;
                out = std::move(obj);
                return true;
            }
            --depth_;
            return fail(i_, "对象成员之间缺少 ',' 或 '}'");
        }
    }

    int depth_ = 0;
};

// ---------------- 输出工具 ----------------

void dump_string(const std::string& s, std::string& out) {
    static const char kHex[] = "0123456789abcdef";
    out.push_back('"');
    for (const char raw : s) {
        const unsigned char c = static_cast<unsigned char>(raw);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20u) {
                    out += "\\u00";
                    out.push_back(kHex[(c >> 4) & 0x0Fu]);
                    out.push_back(kHex[c & 0x0Fu]);
                } else {
                    out.push_back(raw);  // 非 ASCII 字节按 UTF-8 原样输出
                }
        }
    }
    out.push_back('"');
}

// 浮点：优先用 std::to_chars 得到"最短且可往返"的表示（0.1 → "0.1"，而不是
// %.17g 的 "0.10000000000000001"）；失败时退回 %.17g。
// 结果总是保留小数形式或指数形式带 ".0"，避免再次解析成 Int。
std::string format_double(double d) {
    char buf[64];
    std::string s;
#if defined(__cpp_lib_to_chars) || (defined(__GNUC__) && __GNUC__ >= 11)
    const std::to_chars_result r = std::to_chars(buf, buf + sizeof(buf), d);
    if (r.ec == std::errc()) {
        s.assign(buf, r.ptr);
    } else {
        std::snprintf(buf, sizeof(buf), "%.17g", d);
        s.assign(buf);
    }
#else
    std::snprintf(buf, sizeof(buf), "%.17g", d);
    s.assign(buf);
#endif

    const size_t epos = s.find_first_of("eE");
    if (epos != std::string::npos) {
        // 指数记数法：保证形如 d.ddd e±xx，避免 "1e+300" 少了小数点
        if (s.find('.') == std::string::npos) s.insert(epos, ".0");
        return s;
    }

    const size_t dot = s.find('.');
    if (dot == std::string::npos) {
        s += ".0";  // "1" → "1.0"，避免再次解析成 Int
        return s;
    }
    return s;
}

void indent_to(std::string& out, int indent, int depth) {
    if (indent > 0) out.append(static_cast<size_t>(indent) * static_cast<size_t>(depth), ' ');
}

}  // namespace

// ---------------- Value：静态构造 / 解析 ----------------

Value Value::parse(std::string_view text, ParseError* err) {
    Value out;
    if (!try_parse(text, out, err)) return Value();
    return out;
}

bool Value::try_parse(std::string_view text, Value& out, ParseError* err) {
    Parser parser(text, err);
    return parser.run(out);
}

// ---------------- Value：对象访问 ----------------

Value& Value::operator[](std::string_view key) {
    if (!is_object()) v_ = Object{};
    Object& members = std::get<Object>(v_);
    for (auto& kv : members) {
        if (kv.first == key) return kv.second;
    }
    members.emplace_back(std::string(key), Value());
    return members.back().second;
}

const Value& Value::operator[](std::string_view key) const {
    const Value* found = find(key);
    if (found != nullptr) return *found;
    static const Value kNull;
    return kNull;
}

Value& Value::operator[](size_t index) {
    if (!is_array()) v_ = Array{};
    Array& items = std::get<Array>(v_);
    if (index >= items.size()) items.resize(index + 1);
    return items[index];
}

const Value& Value::operator[](size_t index) const {
    static const Value kNull;
    if (!is_array()) return kNull;
    const Array& items = std::get<Array>(v_);
    if (index >= items.size()) return kNull;
    return items[index];
}

const Value* Value::find(std::string_view key) const {
    if (!is_object()) return nullptr;
    for (const auto& kv : std::get<Object>(v_)) {
        if (kv.first == key) return &kv.second;
    }
    return nullptr;
}

Value* Value::find(std::string_view key) {
    if (!is_object()) return nullptr;
    for (auto& kv : std::get<Object>(v_)) {
        if (kv.first == key) return &kv.second;
    }
    return nullptr;
}

bool Value::erase(std::string_view key) {
    if (!is_object()) return false;
    Object& members = std::get<Object>(v_);
    for (auto it = members.begin(); it != members.end(); ++it) {
        if (it->first == key) {
            members.erase(it);
            return true;
        }
    }
    return false;
}

void Value::set(std::string key, Value value) {
    if (!is_object()) v_ = Object{};
    Object& members = std::get<Object>(v_);
    for (auto& kv : members) {
        if (kv.first == key) {  // 覆盖值，保留首次出现的位置
            kv.second = std::move(value);
            return;
        }
    }
    members.emplace_back(std::move(key), std::move(value));
}

void Value::push_back(Value value) {
    if (!is_array()) v_ = Array{};
    std::get<Array>(v_).push_back(std::move(value));
}

// ---------------- Value：便捷取值 ----------------

bool Value::get_bool(std::string_view key, bool def) const {
    const Value* found = find(key);
    return (found != nullptr && found->is_bool()) ? std::get<bool>(found->v_) : def;
}

int64_t Value::get_int(std::string_view key, int64_t def) const {
    const Value* found = find(key);
    if (found == nullptr) return def;
    switch (found->type()) {
        case Type::Int: return std::get<int64_t>(found->v_);
        case Type::Double: return static_cast<int64_t>(std::get<double>(found->v_));
        case Type::Bool: return std::get<bool>(found->v_) ? 1 : 0;
        default: return def;
    }
}

double Value::get_double(std::string_view key, double def) const {
    const Value* found = find(key);
    if (found == nullptr) return def;
    switch (found->type()) {
        case Type::Double: return std::get<double>(found->v_);
        case Type::Int: return static_cast<double>(std::get<int64_t>(found->v_));
        default: return def;
    }
}

std::string Value::get_string(std::string_view key, std::string def) const {
    const Value* found = find(key);
    if (found != nullptr && found->is_string()) return std::get<std::string>(found->v_);
    return def;
}

const Value& Value::get(std::string_view key) const {
    const Value* found = find(key);
    if (found != nullptr) return *found;
    static const Value kNull;
    return kNull;
}

// ---------------- Value：输出 ----------------

std::string Value::dump(int indent) const {
    std::string out;
    dump_to(out, indent, 0);
    return out;
}

void Value::dump_to(std::string& out, int indent, int depth) const {
    const bool pretty = indent >= 0;
    switch (type()) {
        case Type::Null:
            out += "null";
            return;
        case Type::Bool:
            out += std::get<bool>(v_) ? "true" : "false";
            return;
        case Type::Int:
            out += std::to_string(std::get<int64_t>(v_));
            return;
        case Type::Double: {
            const double d = std::get<double>(v_);
            if (!std::isfinite(d)) {
                out += "null";  // NaN / Inf 无法用 JSON 表示
            } else {
                out += format_double(d);
            }
            return;
        }
        case Type::String:
            dump_string(std::get<std::string>(v_), out);
            return;
        case Type::Array: {
            const Array& items = std::get<Array>(v_);
            if (items.empty()) {
                out += "[]";
                return;
            }
            out.push_back('[');
            bool first = true;
            for (const Value& item : items) {
                if (!first) out.push_back(',');
                first = false;
                if (pretty) {
                    out.push_back('\n');
                    indent_to(out, indent, depth + 1);
                }
                item.dump_to(out, indent, depth + 1);
            }
            if (pretty) {
                out.push_back('\n');
                indent_to(out, indent, depth);
            }
            out.push_back(']');
            return;
        }
        case Type::Object:
        default: {
            const Object& members = std::get<Object>(v_);
            if (members.empty()) {
                out += "{}";
                return;
            }
            out.push_back('{');
            bool first = true;
            for (const auto& kv : members) {
                if (!first) out.push_back(',');
                first = false;
                if (pretty) {
                    out.push_back('\n');
                    indent_to(out, indent, depth + 1);
                }
                dump_string(kv.first, out);
                out.push_back(':');
                if (pretty) out.push_back(' ');
                kv.second.dump_to(out, indent, depth + 1);
            }
            if (pretty) {
                out.push_back('\n');
                indent_to(out, indent, depth);
            }
            out.push_back('}');
            return;
        }
    }
}

std::string dump(const Value& v, int indent) { return v.dump(indent); }

// ---------------- Value：比较 ----------------

bool Value::operator==(const Value& other) const {
    const Type a = type();
    const Type b = other.type();
    if (a != b) return false;  // 注意：Int 与 Double 视为不同类型（便于测试）
    switch (a) {
        case Type::Null: return true;
        case Type::Bool: return std::get<bool>(v_) == std::get<bool>(other.v_);
        case Type::Int: return std::get<int64_t>(v_) == std::get<int64_t>(other.v_);
        case Type::Double: return std::get<double>(v_) == std::get<double>(other.v_);
        case Type::String: return std::get<std::string>(v_) == std::get<std::string>(other.v_);
        case Type::Array: return std::get<Array>(v_) == std::get<Array>(other.v_);
        case Type::Object:
        default: return std::get<Object>(v_) == std::get<Object>(other.v_);
    }
}

}  // namespace radmusic::json
