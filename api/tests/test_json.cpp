// Rad music API - JSON 模块测试（零依赖）
//
// 覆盖：解析/dump 回环、键序保持、Int 与 Double 区分、\u 转义与代理对、
//       重复键覆盖、get_* 默认值、错误用例的 offset、集合操作与比较语义。
//
// 说明：CHECK 宏支持变参（条件是逗号表达式），因此 CHECK(a, b) 等价于
//       同时检查 a 与 b，用于断言“解析失败 且 offset 合理”。

#include "radmusic/json.hpp"
#include "test_framework.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace {

using radmusic::json::Array;
using radmusic::json::member;
using radmusic::json::Object;
using radmusic::json::ParseError;
using radmusic::json::Type;
using radmusic::json::Value;

// 构造 250 层嵌套数组：[[[…[]…]]]
std::string make_deep_arrays(int depth) {
    std::string s;
    s.reserve(static_cast<size_t>(depth) * 2);
    for (int i = 0; i < depth; ++i) s += '[';
    for (int i = 0; i < depth; ++i) s += ']';
    return s;
}

}  // namespace

// ---------------------------------------------------------------- 解析 / 回环

TEST_CASE(test_json_parse_round_trip) {
    const std::string text =
        "{\"a\":1,\"b\":[1,2,\"x\"],\"c\":{\"d\":true,\"e\":null},\"f\":[],\"g\":{}}";
    ParseError err;
    const Value v = Value::parse(text, &err);
    CHECK(v.is_object());
    CHECK_EQ(err.offset, static_cast<size_t>(0));
    CHECK_EQ(v.size(), static_cast<size_t>(5));
    CHECK(v["a"].is_int());
    CHECK_EQ(v["a"].as_int(), 1);
    CHECK(v["b"].is_array());
    CHECK_EQ(v["b"].size(), static_cast<size_t>(3));
    CHECK_EQ(v["b"][0].as_int(), 1);
    CHECK_EQ(v["b"][1].as_int(), 2);
    CHECK_EQ(v["b"][2].as_string(), std::string("x"));
    CHECK(v["c"].is_object());
    CHECK(v["c"]["d"].is_bool());
    CHECK(v["c"]["d"].as_bool());
    CHECK(v["c"]["e"].is_null());
    CHECK(v["f"].is_array());
    CHECK(v["f"].empty());
    CHECK(v["g"].is_object());
    CHECK(v["g"].empty());

    // 紧凑输出应与原文一致（键序保持）
    CHECK_EQ(v.dump(), text);

    // indent=2 输出应稳定：dump → parse → dump 完全一致
    const std::string indented = v.dump(2);
    CHECK(indented.find('\n') != std::string::npos);
    ParseError err2;
    const Value v2 = Value::parse(indented, &err2);
    CHECK(v2.is_object());
    CHECK_EQ(v2.dump(2), indented);
    CHECK_EQ(v2.dump(), text);

    // 两种输出都能被再次解析，且再 dump 后依然稳定
    ParseError err3;
    CHECK(Value::parse(v.dump(), &err3) == v);
    CHECK(Value::parse(indented, &err3) == v);
}

// ---------------------------------------------------------------- 结构 / 类型

TEST_CASE(test_json_nested_and_types) {
    ParseError err;
    const Value v = Value::parse(
        "{\"i\":1,\"d\":1.0,\"d2\":-2.5e2,\"s\":\"hi\",\"b\":false,\"n\":null,"
        "\"big\":12345678901234567890,\"e\":[{\"x\":[]}]}",
        &err);
    CHECK(v.is_object());
    CHECK_EQ(v.size(), static_cast<size_t>(8));

    CHECK(v["i"].is_int());
    CHECK_EQ(v["i"].as_int(), 1);
    CHECK_EQ(std::string(v["i"].type_name()), std::string("int"));

    CHECK(v["d"].is_double());
    CHECK_EQ(std::string(v["d"].type_name()), std::string("double"));
    CHECK_EQ(v["d"].as_double(), 1.0);

    CHECK(v["d2"].is_double());
    CHECK_EQ(v["d2"].as_double(), -250.0);

    CHECK(v["s"].is_string());
    CHECK_EQ(v["s"].as_string(), std::string("hi"));
    CHECK_EQ(v["s"].as_string("def"), std::string("hi"));

    CHECK(v["b"].is_bool());
    CHECK_FALSE(v["b"].as_bool(true));

    CHECK(v["n"].is_null());
    CHECK_FALSE(v["n"].is_number());

    // 超出 int64 → Double（精度损失可接受）
    CHECK(v["big"].is_double());
    CHECK(v["big"].as_double() > 1e19);

    CHECK(v["e"].is_array());
    CHECK(v["e"][0].is_object());
    CHECK(v["e"][0]["x"].is_array());
    CHECK(v["e"][0]["x"].empty());

    // Int 与 Double 是不同类型，即使数值相等
    CHECK_NE(Value(1), Value(1.0));
    CHECK(Value(1) == Value(1));
    CHECK(Value(1.0) == Value(1.0));
    CHECK_NE(Value("1"), Value(1));
    CHECK(Value() == Value(nullptr));
}

// ---------------------------------------------------------------- \u 转义

TEST_CASE(test_json_unicode_escapes) {
    ParseError err;

    const Value cn = Value::parse("\"\\u4e2d\\u6587\"", &err);
    CHECK(cn.is_string());
    CHECK_EQ(cn.as_string(), std::string("\xE4\xB8\xAD\xE6\x96\x87"));
    CHECK_EQ(cn.as_string(), std::string("中文"));

    // 代理对：U+1F600 😀 → F0 9F 98 80
    const Value emoji = Value::parse("\"\\ud83d\\ude00\"", &err);
    CHECK(emoji.is_string());
    CHECK_EQ(emoji.as_string(), std::string("\xF0\x9F\x98\x80"));
    CHECK_EQ(emoji.as_string().size(), static_cast<size_t>(4));

    // 孤立高代理 / 孤立低代理 → U+FFFD（EF BF BD）
    const Value lone_high = Value::parse("\"\\ud800\"", &err);
    CHECK_EQ(lone_high.as_string(), std::string("\xEF\xBF\xBD"));
    const Value lone_low = Value::parse("\"\\udc00\"", &err);
    CHECK_EQ(lone_low.as_string(), std::string("\xEF\xBF\xBD"));
    // 高代理后面不是低代理：高代理 → U+FFFD，后面的转义独立解析（不能被吞掉）
    const Value mixed = Value::parse("\"\\ud800\\u4e2d\"", &err);
    CHECK_EQ(mixed.as_string(), std::string("\xEF\xBF\xBD\xE4\xB8\xAD"));
    // 两个连续高代理：各自成为 U+FFFD
    const Value two_high = Value::parse("\"\\ud800\\ud800\"", &err);
    CHECK_EQ(two_high.as_string(), std::string("\xEF\xBF\xBD\xEF\xBF\xBD"));

    // 转义集合
    const Value esc = Value::parse("\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"", &err);
    CHECK(esc.is_string());
    CHECK_EQ(esc.as_string(), std::string("\"\\/\b\f\n\r\t"));

    // dump 时特殊字符必须被转义
    const std::string dumped = esc.dump();
    CHECK_EQ(dumped, std::string("\"\\\"\\\\/\\b\\f\\n\\r\\t\""));
    ParseError err2;
    CHECK(Value::parse(dumped, &err2) == esc);

    // 非 ASCII 与 U+0001 → \u0001
    CHECK_EQ(Value("\x01").dump(), std::string("\"\\u0001\""));
    CHECK_EQ(Value("中文").dump(), std::string("\"中文\""));

    // 大小写十六进制均可
    const Value upper = Value::parse("\"\\u4E2D\"", &err);
    CHECK_EQ(upper.as_string(), std::string("中"));
}

// ---------------------------------------------------------------- 重复键

TEST_CASE(test_json_duplicate_keys) {
    ParseError err;
    const Value v = Value::parse("{\"a\":1,\"a\":2}", &err);
    CHECK(v.is_object());
    CHECK_EQ(v.size(), static_cast<size_t>(1));
    CHECK_EQ(v["a"].as_int(), 2);

    // 位置保留：键第一次出现的位置不变；set 语义相同
    const Value v2 = Value::parse("{\"x\":0,\"a\":1,\"y\":0,\"a\":2}", &err);
    CHECK_EQ(v2.size(), static_cast<size_t>(3));
    CHECK_EQ(v2.as_object()[1].first, std::string("a"));
    CHECK_EQ(v2.as_object()[1].second.as_int(), 2);
    CHECK_EQ(v2.dump(), std::string("{\"x\":0,\"a\":2,\"y\":0}"));

    Value v3 = Value::object();
    v3.set("k", Value(1));
    v3.set("z", Value(9));
    v3.set("k", Value(5));
    CHECK_EQ(v3.size(), static_cast<size_t>(2));
    CHECK_EQ(v3.as_object()[0].first, std::string("k"));
    CHECK_EQ(v3["k"].as_int(), 5);
    CHECK_EQ(v3.dump(), std::string("{\"k\":5,\"z\":9}"));
}

// ---------------------------------------------------------------- get_* 默认值

TEST_CASE(test_json_get_helpers) {
    ParseError err;
    const Value v = Value::parse(
        "{\"b\":true,\"i\":7,\"d\":2.5,\"s\":\"q\",\"null\":null,\"arr\":[1]}", &err);
    CHECK(v.is_object());

    CHECK(v.get_bool("b", false));
    CHECK_FALSE(v.get_bool("missing", false));
    CHECK(v.get_bool("missing", true));     // 缺失 → 默认值
    CHECK_FALSE(v.get_bool("i", false));    // 类型不符 → 默认值（Int 不算 Bool）
    CHECK_FALSE(v.get_bool("null", false));

    CHECK_EQ(v.get_int("i", -1), 7);
    CHECK_EQ(v.get_int("d", -1), 2);        // Double → 截断
    CHECK_EQ(v.get_int("missing", -1), -1);
    CHECK_EQ(v.get_int("s", -1), -1);

    CHECK_EQ(v.get_double("d", -1.0), 2.5);
    CHECK_EQ(v.get_double("i", -1.0), 7.0);
    CHECK_EQ(v.get_double("missing", -1.0), -1.0);
    CHECK_EQ(v.get_double("arr", -1.0), -1.0);

    CHECK_EQ(v.get_string("s", "def"), std::string("q"));
    CHECK_EQ(v.get_string("missing", "def"), std::string("def"));
    CHECK_EQ(v.get_string("i", "def"), std::string("def"));
    CHECK(v.get_string("missing").empty());

    CHECK(v.get("i").is_int());
    CHECK(v.get("missing").is_null());
    CHECK_EQ(v.get("missing").size(), static_cast<size_t>(0));

    // 非对象上取键：全部返回默认值
    const Value num = Value(3);
    CHECK_FALSE(num.is_object());
    CHECK_EQ(num.get_int("i", -1), -1);
    CHECK(num.get("i").is_null());
    CHECK_EQ(num["i"].size(), static_cast<size_t>(0));
}

// ---------------------------------------------------------------- 错误用例

TEST_CASE(test_json_errors) {
    const std::string_view file = __FILE__;
    const int line = __LINE__;
    const auto check_fail = [&](const char* what, std::string_view text,
                                size_t lo, size_t hi) {
        ParseError e;
        Value out;
        const bool ok = Value::try_parse(text, out, &e);
        if (ok) {
            radtest::fail(file.data(), line, std::string("应解析失败: ") + what);
            return;
        }
        if (out.type() != Type::Null) {
            radtest::fail(file.data(), line, std::string("失败时应返回 null: ") + what);
        }
        if (e.message.empty()) {
            radtest::fail(file.data(), line, std::string("错误信息为空: ") + what);
        }
        if (e.offset < lo || e.offset > hi) {
            radtest::fail(file.data(), line,
                          std::string("offset 不合理: ") + what + " offset=" +
                              std::to_string(e.offset) + " 期望 [" + std::to_string(lo) +
                              "," + std::to_string(hi) + "]");
        }
    };

    // 尾随逗号
    check_fail("object trailing comma", "{\"a\":1,}", 7, 8);
    check_fail("array trailing comma", "[1,2,]", 5, 6);
    // 未闭合
    check_fail("unterminated string", "\"abc", 0, 4);
    check_fail("unterminated object", "{\"a\":1", 6, 7);
    check_fail("unterminated array", "[1,2", 4, 5);
    check_fail("missing colon", "{\"a\" 1}", 5, 5);
    // 非法转义
    check_fail("bad escape", "\"\\x\"", 1, 2);
    check_fail("incomplete escape", "\"abc\\", 4, 5);
    check_fail("short unicode escape", "\"\\u12\"", 1, 5);
    check_fail("bad unicode escape", "\"\\u12g4\"", 4, 5);
    // 裸控制字符
    check_fail("raw control char", "\"a\nb\"", 2, 2);
    check_fail("raw tab", "\"a\tb\"", 2, 2);
    // 多个顶层值
    check_fail("multiple top-level values", "1 2", 2, 2);
    check_fail("two objects", "{}{}", 2, 2);
    check_fail("trailing junk", "null x", 5, 5);
    // 数字
    check_fail("leading zero", "01", 0, 0);
    check_fail("leading plus", "+1", 0, 0);
    check_fail("dot without fraction", "1.", 0, 1);
    check_fail("dot without integer", ".5", 0, 0);
    check_fail("exponent without digits", "1e", 0, 1);
    check_fail("lone minus", "-", 0, 0);
    // 其他
    check_fail("empty input", "", 0, 0);
    check_fail("whitespace only", "   ", 3, 3);
    check_fail("bad literal", "tru", 0, 0);
    check_fail("single quote string", "'a'", 0, 0);
    check_fail("unquoted key", "{a:1}", 1, 1);
    // 深度超限：250 层
    const std::string deep = make_deep_arrays(250);
    check_fail("too deep", deep, 200, 250);
    // 对照：199 层应当能成功解析
    ParseError ok_err;
    const Value ok_deep = Value::parse(make_deep_arrays(199), &ok_err);
    CHECK(ok_deep.is_array());
    CHECK_EQ(ok_err.offset, static_cast<size_t>(0));
}

// ---------------------------------------------------------------- 数字 / dump 细节

TEST_CASE(test_json_number_edges) {
    ParseError err;

    // int64 边界
    const Value vmax = Value::parse("9223372036854775807", &err);
    CHECK(vmax.is_int());
    CHECK_EQ(vmax.as_int(), INT64_C(9223372036854775807));
    const Value vmin = Value::parse("-9223372036854775808", &err);
    CHECK(vmin.is_int());
    CHECK_EQ(vmin.as_int(), INT64_C(-9223372036854775807) - 1);
    // 越界一位 → Double
    CHECK(Value::parse("9223372036854775808", &err).is_double());
    CHECK(Value::parse("-9223372036854775809", &err).is_double());
    // 前导零后跟指数是合法的
    const Value ez = Value::parse("0e1", &err);
    CHECK(ez.is_double());
    CHECK_EQ(ez.as_double(), 0.0);

    // dump：整数用十进制，浮点可往返
    CHECK_EQ(Value(INT64_C(-42)).dump(), std::string("-42"));
    CHECK_EQ(Value(0.1).dump(), std::string("0.1"));
    CHECK_EQ(Value(1.0).dump(), std::string("1.0"));
    CHECK_EQ(Value(-0.5).dump(), std::string("-0.5"));
    CHECK_EQ(Value(1e300).dump(), std::string("1.0e+300"));
    CHECK_EQ(Value(1000.0).dump(), std::string("1000.0"));
    CHECK_EQ(Value(1.25e-7).dump(), std::string("1.25e-07"));
    CHECK_EQ(Value(-0.0).dump(), std::string("-0.0"));

    // 浮点 dump → parse 回环（同一 double 位模式）
    const double samples[] = {0.1, -2.5, 3.14159265358979, 1e-300, 1e300, 123456.789};
    for (const double d : samples) {
        const Value a(d);
        ParseError e2;
        const Value b = Value::parse(a.dump(), &e2);
        CHECK(b.is_double());
        CHECK(b == a);
    }

    // NaN / Inf → null（合法 JSON，可再次解析）
    const double zero = 0.0;
    const double nan_value = zero / zero;
    const double inf_value = 1.0 / zero;
    CHECK_EQ(Value(nan_value).dump(), std::string("null"));
    CHECK_EQ(Value(inf_value).dump(), std::string("null"));
    CHECK_EQ(Value(-inf_value).dump(), std::string("null"));
    ParseError e3;
    CHECK(Value::parse(Value(inf_value).dump(), &e3).is_null());
    CHECK(Value::parse(Value(nan_value).dump(), &e3).is_null());
}

// ---------------------------------------------------------------- dump 细节

TEST_CASE(test_json_dump_details) {
    ParseError err;
    const Value v = Value::parse("{\"a\":1,\"b\":[true,null,\"x\"]}", &err);
    CHECK(v.is_object());

    CHECK_EQ(v.dump(), std::string("{\"a\":1,\"b\":[true,null,\"x\"]}"));
    CHECK_EQ(radmusic::json::dump(v), std::string("{\"a\":1,\"b\":[true,null,\"x\"]}"));
    CHECK_EQ(radmusic::json::dump(v, 2), v.dump(2));

    const std::string p = v.dump(2);
    CHECK_EQ(p, std::string("{\n"
                            "  \"a\": 1,\n"
                            "  \"b\": [\n"
                            "    true,\n"
                            "    null,\n"
                            "    \"x\"\n"
                            "  ]\n"
                            "}"));

    // indent=0：换行但不缩进
    const std::string p0 = v.dump(0);
    CHECK_EQ(p0, std::string("{\n\"a\": 1,\n\"b\": [\ntrue,\nnull,\n\"x\"\n]\n}"));

    // 空容器不换行
    CHECK_EQ(Value::array().dump(2), std::string("[]"));
    CHECK_EQ(Value::object().dump(2), std::string("{}"));
    CHECK_EQ(Value().dump(4), std::string("null"));

    // 嵌套缩进按深度累加
    const Value nested = Value::parse("{\"o\":{\"a\":[1]}}", &err);
    CHECK_EQ(nested.dump(2), std::string("{\n"
                                         "  \"o\": {\n"
                                         "    \"a\": [\n"
                                         "      1\n"
                                         "    ]\n"
                                         "  }\n"
                                         "}"));

    // member() 辅助构造 + 非 ASCII 键
    Object obj;
    obj.push_back(member("名", Value(1)));
    obj.push_back(member("b", Value::array()));
    Value manual = Value::object();
    manual.as_object() = obj;
    CHECK_EQ(manual.dump(), std::string("{\"名\":1,\"b\":[]}"));
    CHECK(Value::parse(manual.dump(), &err) == manual);
}

// ---------------------------------------------------------------- 集合操作

TEST_CASE(test_json_containers) {
    ParseError err;
    Value v = Value::parse("{\"a\":1,\"b\":2}", &err);
    CHECK(v.contains("a"));
    CHECK_FALSE(v.contains("zz"));

    CHECK(v.find("a") != nullptr);
    CHECK_EQ(v.find("a")->as_int(), 1);
    CHECK(v.find("zz") == nullptr);
    const Value& cv = v;
    CHECK(cv.find("b") != nullptr);
    CHECK(cv.find("zz") == nullptr);

    CHECK(v.erase("a"));
    CHECK_FALSE(v.erase("a"));
    CHECK_FALSE(v.contains("a"));
    CHECK_EQ(v.size(), static_cast<size_t>(1));
    CHECK_EQ(v.dump(), std::string("{\"b\":2}"));

    // 非对象上的 erase/find 都不改变类型
    const Value num = Value(5);
    CHECK_FALSE(num.find("x") != nullptr);
    Value num2 = Value(5);
    CHECK_FALSE(num2.erase("x"));
    CHECK(num2.is_int());

    // operator[]：非对象先转成对象
    Value n = Value(1);
    CHECK(n.is_int());
    n["k"] = Value(2);
    CHECK(n.is_object());
    CHECK_EQ(n.size(), static_cast<size_t>(1));
    CHECK_EQ(n["k"].as_int(), 2);
    CHECK(n["k2"].is_null());          // 不存在则插入 null
    CHECK_EQ(n.size(), static_cast<size_t>(2));
    n["k"] = Value(9);                 // 已存在则覆盖，不新增
    CHECK_EQ(n.size(), static_cast<size_t>(2));
    CHECK_EQ(n["k"].as_int(), 9);

    // const operator[] 不插入
    const Value& cn = n;
    const size_t before = n.size();
    CHECK(cn["nope"].is_null());
    CHECK_EQ(n.size(), before);

    // push_back / as_array / as_object
    Value arr = Value::array();
    arr.push_back(Value(1));
    arr.push_back(Value("s"));
    CHECK_EQ(arr.size(), static_cast<size_t>(2));
    CHECK_EQ(arr.as_array()[1].as_string(), std::string("s"));
    arr.as_array().push_back(Value(true));
    CHECK_EQ(arr.size(), static_cast<size_t>(3));
    CHECK_EQ(arr.as_array()[2].as_bool(), true);

    // 非数组上 push_back 会先转换成数组
    Value not_arr = Value(7);
    not_arr.push_back(Value(1));
    CHECK(not_arr.is_array());
    CHECK_EQ(not_arr.size(), static_cast<size_t>(1));

    // 非对象上 as_object() 会先转换成对象
    Value not_obj = Value(7);
    not_obj.as_object().push_back(member("k", Value(1)));
    CHECK(not_obj.is_object());
    CHECK_EQ(not_obj["k"].as_int(), 1);

    // 非容器的 as_array() const / as_object() const
    const Value scalar = Value(1);
    CHECK(scalar.as_array().empty());
    CHECK(scalar.as_object().empty());
}

// ---------------------------------------------------------------- 数组顺序比较

TEST_CASE(test_json_equality) {
    ParseError err;
    CHECK(Value::parse("{\"a\":[1,2]}", &err) == Value::parse("{\"a\":[1,2]}", &err));
    CHECK(Value::parse("{\"a\":[1,2]}", &err) != Value::parse("{\"a\":[2,1]}", &err));
    CHECK(Value::parse("{\"a\":1}", &err) != Value::parse("{\"b\":1}", &err));
    CHECK(Value::parse("{\"a\":1,\"b\":2}", &err) != Value::parse("{\"b\":2,\"a\":1}", &err));
    CHECK(Value::parse("[]", &err) == Value::parse("[]", &err));
    CHECK(Value::parse("{}", &err) == Value::parse("{}", &err));
    CHECK(Value::parse("[\"a\"]", &err) != Value::parse("[\"b\"]", &err));
    CHECK(Value::parse("true", &err) != Value::parse("1", &err));
    CHECK(Value::parse("false", &err) == Value(false));
    CHECK(Value::parse("null", &err) == Value());
}
