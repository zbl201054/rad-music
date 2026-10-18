// Rad music API - 极小测试框架（零依赖）
// 用法：
//   TEST_CASE(name) { CHECK(cond); CHECK_EQ(a, b); }
// 运行：radmusic_tests [过滤子串]
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace radtest {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

std::vector<TestCase>& registry();

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) {
        registry().push_back(TestCase{name, std::move(fn)});
    }
};

// 由 test_main.cpp 提供：当前用例的失败计数与上下文
void fail(const char* file, int line, const std::string& message);
void note(const std::string& message);

int run_all(int argc, char** argv);

}  // namespace radtest

#define TEST_CASE(name)                                                    \
    static void radtest_case_##name();                                     \
    static ::radtest::Registrar radtest_reg_##name(#name, radtest_case_##name); \
    static void radtest_case_##name()

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            ::radtest::fail(__FILE__, __LINE__, "CHECK 失败: " #cond);      \
        }                                                                  \
    } while (0)

#define CHECK_FALSE(cond) CHECK(!(cond))

#define CHECK_EQ(a, b)                                                     \
    do {                                                                   \
        auto&& radtest_a = (a);                                            \
        auto&& radtest_b = (b);                                            \
        if (!(radtest_a == radtest_b)) {                                    \
            ::radtest::note(std::string("  左侧: ") + ::radtest::show(radtest_a)); \
            ::radtest::note(std::string("  右侧: ") + ::radtest::show(radtest_b)); \
            ::radtest::fail(__FILE__, __LINE__, "CHECK_EQ 失败: " #a " == " #b); \
        }                                                                  \
    } while (0)

#define CHECK_NE(a, b)                                                     \
    do {                                                                   \
        auto&& radtest_a = (a);                                            \
        auto&& radtest_b = (b);                                            \
        if (radtest_a == radtest_b) {                                      \
            ::radtest::fail(__FILE__, __LINE__, "CHECK_NE 失败: " #a " != " #b); \
        }                                                                  \
    } while (0)

namespace radtest {

// show()：把常见类型转成字符串用于失败输出
inline std::string show(const std::string& v) { return v; }
inline std::string show(const char* v) { return v ? v : "(null)"; }
inline std::string show(bool v) { return v ? "true" : "false"; }
inline std::string show(int v) { return std::to_string(v); }
inline std::string show(long v) { return std::to_string(v); }
inline std::string show(long long v) { return std::to_string(v); }
inline std::string show(unsigned v) { return std::to_string(v); }
inline std::string show(unsigned long v) { return std::to_string(v); }
inline std::string show(unsigned long long v) { return std::to_string(v); }
inline std::string show(double v) { return std::to_string(v); }

template <typename T>
std::string show(const T&) {
    return "<value>";
}

}  // namespace radtest
