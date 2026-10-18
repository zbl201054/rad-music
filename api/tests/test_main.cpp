// Rad music API - 测试入口与断言实现（零依赖）
#include "test_framework.hpp"

#include <cstring>
#include <iostream>

namespace radtest {

std::vector<TestCase>& registry() {
    static std::vector<TestCase> reg;
    return reg;
}

namespace {
int g_failures_in_case = 0;
std::string g_current_case;
bool g_verbose = false;
}  // namespace

void fail(const char* file, int line, const std::string& message) {
    ++g_failures_in_case;
    std::cout << "  [FAIL] " << file << ":" << line << " " << message << "\n";
}

void note(const std::string& message) {
    std::cout << message << "\n";
}

bool verbose() { return g_verbose; }

int run_all(int argc, char** argv) {
    std::string filter;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-v") == 0) {
            g_verbose = true;
        } else if (filter.empty()) {
            filter = argv[i];
        }
    }

    int passed = 0;
    int failed = 0;
    for (const auto& tc : registry()) {
        if (!filter.empty() && tc.name.find(filter) == std::string::npos) continue;
        g_current_case = tc.name;
        g_failures_in_case = 0;
        if (g_verbose) std::cout << "[ RUN  ] " << tc.name << "\n";
        try {
            tc.fn();
        } catch (const std::exception& e) {
            ++g_failures_in_case;
            std::cout << "  [FAIL] 抛出异常: " << e.what() << "\n";
        } catch (...) {
            ++g_failures_in_case;
            std::cout << "  [FAIL] 抛出未知异常\n";
        }
        if (g_failures_in_case == 0) {
            ++passed;
            if (g_verbose) std::cout << "[  OK  ] " << tc.name << "\n";
        } else {
            ++failed;
            std::cout << "[ FAIL ] " << tc.name << "（" << g_failures_in_case << " 处失败）\n";
        }
    }

    std::cout << "\n合计: " << (passed + failed) << " 个用例, 通过 " << passed
              << ", 失败 " << failed << "\n";
    return failed == 0 ? 0 : 1;
}

}  // namespace radtest

int main(int argc, char** argv) {
    return radtest::run_all(argc, argv);
}
