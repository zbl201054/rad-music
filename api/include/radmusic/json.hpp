// Rad music API - 极小 JSON 实现（零依赖，C++17）
// 设计要点：
//  * 对象使用保序 vector<pair<string, Value>>，重复键“后者覆盖前者”且保留首次出现的位置；
//  * 整数与浮点分开保存（Int / Double），整数溢出回退为 Double；
//  * 输出为 UTF-8（中文不转义），转义仅处理 JSON 必需字符；
//  * parse 不做异常抛出，失败时返回 null 并填充 ParseError。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace radmusic::json {

class Value;

using Array = std::vector<Value>;
using Object = std::vector<std::pair<std::string, Value>>;

enum class Type { Null, Bool, Int, Double, String, Array, Object };

struct ParseError {
    size_t offset = 0;      // 出错位置（字节偏移）
    std::string message;    // 人类可读描述
};

class Value {
public:
    Value() : v_(nullptr) {}
    Value(std::nullptr_t) : v_(nullptr) {}
    Value(bool b) : v_(b) {}
    Value(int i) : v_(static_cast<int64_t>(i)) {}
    Value(long i) : v_(static_cast<int64_t>(i)) {}
    Value(long long i) : v_(static_cast<int64_t>(i)) {}
    Value(unsigned u) : v_(static_cast<int64_t>(u)) {}
    Value(unsigned long u) : v_(static_cast<int64_t>(u)) {}
    Value(unsigned long long u) : v_(static_cast<int64_t>(u)) {}
    Value(double d) : v_(d) {}
    Value(const char* s) : v_(std::string(s ? s : "")) {}
    Value(std::string s) : v_(std::move(s)) {}
    Value(std::string_view s) : v_(std::string(s)) {}

    static Value array() {
        Value v;
        v.v_ = Array{};
        return v;
    }
    static Value object() {
        Value v;
        v.v_ = Object{};
        return v;
    }

    Type type() const {
        switch (v_.index()) {
            case 0: return Type::Null;
            case 1: return Type::Bool;
            case 2: return Type::Int;
            case 3: return Type::Double;
            case 4: return Type::String;
            case 5: return Type::Array;
            default: return Type::Object;
        }
    }

    const char* type_name() const {
        switch (type()) {
            case Type::Null: return "null";
            case Type::Bool: return "bool";
            case Type::Int: return "int";
            case Type::Double: return "double";
            case Type::String: return "string";
            case Type::Array: return "array";
            default: return "object";
        }
    }

    bool is_null() const { return type() == Type::Null; }
    bool is_bool() const { return type() == Type::Bool; }
    bool is_int() const { return type() == Type::Int; }
    bool is_double() const { return type() == Type::Double; }
    bool is_number() const { return is_int() || is_double(); }
    bool is_string() const { return type() == Type::String; }
    bool is_array() const { return type() == Type::Array; }
    bool is_object() const { return type() == Type::Object; }

    bool as_bool(bool def = false) const {
        if (is_bool()) return std::get<bool>(v_);
        if (is_int()) return std::get<int64_t>(v_) != 0;
        return def;
    }
    int64_t as_int(int64_t def = 0) const {
        if (is_int()) return std::get<int64_t>(v_);
        if (is_double()) return static_cast<int64_t>(std::get<double>(v_));
        if (is_bool()) return std::get<bool>(v_) ? 1 : 0;
        return def;
    }
    double as_double(double def = 0.0) const {
        if (is_double()) return std::get<double>(v_);
        if (is_int()) return static_cast<double>(std::get<int64_t>(v_));
        return def;
    }
    std::string as_string(std::string def = std::string()) const {
        if (is_string()) return std::get<std::string>(v_);
        return def;
    }

    size_t size() const {
        if (is_array()) return std::get<Array>(v_).size();
        if (is_object()) return std::get<Object>(v_).size();
        return 0;
    }
    bool empty() const { return size() == 0; }

    Array& as_array() {
        if (!is_array()) v_ = Array{};
        return std::get<Array>(v_);
    }
    const Array& as_array() const {
        static const Array kEmpty;
        return is_array() ? std::get<Array>(v_) : kEmpty;
    }
    Object& as_object() {
        if (!is_object()) v_ = Object{};
        return std::get<Object>(v_);
    }
    const Object& as_object() const {
        static const Object kEmpty;
        return is_object() ? std::get<Object>(v_) : kEmpty;
    }

    // 对象：不存在则插入；非对象会被转换为对象
    Value& operator[](std::string_view key);
    const Value& operator[](std::string_view key) const;

    // 数组下标：非数组时可变版本会转换为数组并扩容，const 版本越界返回 null
    Value& operator[](size_t index);
    const Value& operator[](size_t index) const;

    const Value* find(std::string_view key) const;
    Value* find(std::string_view key);
    bool contains(std::string_view key) const { return find(key) != nullptr; }
    bool erase(std::string_view key);

    void set(std::string key, Value value);
    void push_back(Value value);

    // 便捷取值（缺失/类型不符时返回默认值）
    bool get_bool(std::string_view key, bool def = false) const;
    int64_t get_int(std::string_view key, int64_t def = 0) const;
    double get_double(std::string_view key, double def = 0.0) const;
    std::string get_string(std::string_view key, std::string def = std::string()) const;
    const Value& get(std::string_view key) const;   // 缺失返回静态 null

    static Value parse(std::string_view text, ParseError* err = nullptr);
    static bool try_parse(std::string_view text, Value& out, ParseError* err = nullptr);

    // indent < 0：紧凑输出；indent >= 0：按该空格数换行缩进
    std::string dump(int indent = -1) const;
    void dump_to(std::string& out, int indent, int depth) const;

    bool operator==(const Value& other) const;
    bool operator!=(const Value& other) const { return !(*this == other); }

    const std::variant<std::nullptr_t, bool, int64_t, double, std::string, Array, Object>& raw() const {
        return v_;
    }

private:
    std::variant<std::nullptr_t, bool, int64_t, double, std::string, Array, Object> v_;
};

// 便于书写：json::Object 的辅助构造
inline std::pair<std::string, Value> member(std::string key, Value value) {
    return {std::move(key), std::move(value)};
}

std::string dump(const Value& v, int indent = -1);

}  // namespace radmusic::json
