// 运行日志测试（Readme 1.14）
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "radmusic/log.hpp"
#include "test_framework.hpp"

using namespace radmusic;

namespace {

std::filesystem::path fresh_dir(const std::string& name) {
    const auto dir = std::filesystem::path("build") / "tmp_log" / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::string read_all(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

size_t count_lines(const std::string& text) {
    size_t lines = 0;
    for (char c : text) {
        if (c == '\n') ++lines;
    }
    return lines;
}

// 每个用例结束后恢复默认（避免影响其他用例）
struct LoggerGuard {
    ~LoggerGuard() {
        Logger::instance().clear_sinks();
        Logger::instance().set_level(LogLevel::Info);
    }
};

}  // namespace

TEST_CASE(test_log_levels_and_ids) {
    CHECK_EQ(std::string(log_level_id(LogLevel::Trace)), std::string("trace"));
    CHECK_EQ(std::string(log_level_id(LogLevel::Info)), std::string("info"));
    CHECK_EQ(std::string(log_level_id(LogLevel::Error)), std::string("error"));
    CHECK(log_level_from_id("debug") == LogLevel::Debug);
    CHECK(log_level_from_id("WARN") == LogLevel::Info);      // 大小写敏感：非小写按默认 info
    CHECK(log_level_from_id("warn") == LogLevel::Warn);
    CHECK(log_level_from_id("warning") == LogLevel::Warn);
    CHECK(log_level_from_id("off") == LogLevel::Off);
}

TEST_CASE(test_log_memory_sink_and_filtering) {
    LoggerGuard guard;
    auto memory = std::make_shared<MemoryLogSink>(10);
    Logger& logger = Logger::instance();
    logger.clear_sinks();
    logger.add_sink(memory);
    logger.set_level(LogLevel::Debug);

    CHECK(logger.enabled(LogLevel::Info));
    CHECK(logger.enabled(LogLevel::Debug));
    CHECK_FALSE(logger.enabled(LogLevel::Trace));

    logger.trace("t", "trace 应被过滤");
    logger.debug("d", "debug 应被记录");
    logger.info("i", "info 应被记录");
    logger.warn("w", "warn 应被记录");
    logger.error("e", "error 应被记录");
    CHECK_EQ(memory->records().size(), size_t{4});
    CHECK_EQ(memory->records()[0].message, std::string("debug 应被记录"));
    CHECK(memory->records()[3].level == LogLevel::Error);
    CHECK(memory->records()[0].ts_ms > 0);

    // 环形缓冲：容量 10，写 12 条只保留最后 10 条
    for (int i = 0; i < 12; ++i) logger.info("ring", "line " + std::to_string(i));
    CHECK_EQ(memory->records().size(), size_t{10});
    CHECK_EQ(memory->records().back().message, std::string("line 11"));

    // 级别调到 warn 后 info 不再记录（容量已满，用"最后一条"判断而不是 size）
    logger.set_level(LogLevel::Warn);
    logger.info("x", "不应记录");
    logger.warn("x", "应记录");
    CHECK_EQ(memory->records().size(), size_t{10});
    CHECK_EQ(memory->records().back().message, std::string("应记录"));
    CHECK_EQ(memory->records().back().category, std::string("x"));

    // off 关闭一切：最后一条保持不变
    logger.set_level(LogLevel::Off);
    CHECK_FALSE(logger.enabled(LogLevel::Error));
    logger.error("x", "不应记录");
    CHECK_EQ(memory->records().back().message, std::string("应记录"));
}

TEST_CASE(test_log_file_sink_and_rotation) {
    LoggerGuard guard;
    const auto dir = fresh_dir("rotate");
    const auto file = dir / "radmusic.log";

    Logger& logger = Logger::instance();
    logger.clear_sinks();
    logger.set_level(LogLevel::Info);

    // 上限 400 字节，写很多条触发轮转
    auto sink = std::make_shared<FileLogSink>(file.u8string(), 400);
    CHECK(sink->healthy());
    logger.add_sink(sink);
    for (int i = 0; i < 60; ++i) logger.info("file", "rotation line " + std::to_string(i));

    const auto backup = std::filesystem::path(file.u8string() + ".1");
    CHECK(std::filesystem::exists(file));
    CHECK(std::filesystem::exists(backup));      // 触发了轮转
    const std::string current = read_all(file);
    CHECK(!current.empty());
    CHECK(current.find("[info] [file]") != std::string::npos);
    CHECK(current.size() <= 1200);               // 轮转后不会继续无限增长
}

TEST_CASE(test_log_record_json_and_format) {
    LoggerGuard guard;
    LogRecord record;
    record.ts_ms = 1735689600123;
    record.level = LogLevel::Warn;
    record.category = "http";
    record.message = "GET /music/status -> code=0";
    record.file = "http.cpp";
    record.line = 512;

    const std::string line = log_format_record(record);
    CHECK(line.find("[warn] [http]") != std::string::npos);
    CHECK(line.find("GET /music/status -> code=0") != std::string::npos);
    CHECK(line.find("http.cpp:512") != std::string::npos);
    CHECK(line.find("2026-10-11T") == 0 || line.find("-") == 4);   // ISO 时间前缀

    const json::Value value = record.to_json();
    CHECK_EQ(value.get_string("level"), std::string("warn"));
    CHECK_EQ(value.get_string("category"), std::string("http"));
    CHECK_EQ(value.get_int("line"), 512);
    CHECK(value.get_int("ts_ms") == 1735689600123);
    CHECK(!value.get_string("time").empty());
}

TEST_CASE(test_log_configure_wires_sinks) {
    LoggerGuard guard;
    const auto dir = fresh_dir("configure");
    const auto file = dir / "app.log";

    LogConfig config;
    config.level = LogLevel::Debug;
    config.console = false;             // 测试里不污染 stderr
    config.memory = true;
    config.memory_capacity = 5;
    config.file_path = file.u8string();
    const Status status = log_configure(config);
    CHECK(status.ok());
    CHECK_EQ(Logger::instance().sinks().size(), size_t{2});   // memory + file

    RM_LOG_INFO("configure", "hello from macro");
    RM_LOG_DEBUG("configure", "debug from macro");
    RM_LOG_ERROR("configure", "error from macro");
    Logger::instance().flush();

    const std::string content = read_all(file);
    CHECK(content.find("hello from macro") != std::string::npos);
    CHECK(content.find("[debug]") != std::string::npos);
    CHECK(content.find("[error]") != std::string::npos);

    auto memory = log_memory_sink();
    CHECK(memory != nullptr);
    CHECK_EQ(memory->records().size(), size_t{3});
    CHECK_EQ(memory->records()[0].file.find("test_log.cpp") != std::string::npos, true);

    // 关闭内存 sink 后 log_memory_sink() 为空
    config.memory = false;
    config.file_path.clear();
    CHECK(log_configure(config).ok());
    CHECK(log_memory_sink() == nullptr);

    // 无法打开日志文件 → 失败状态（Windows 上 NUL 是保留设备名，不能作为目录）
    LogConfig bad;
    bad.console = false;
    bad.memory = false;
    bad.file_path = "NUL/forbidden/app.log";
    const Status failed = log_configure(bad);
    CHECK_FALSE(failed.ok());
}
