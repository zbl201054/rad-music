// Rad music API - 运行日志（Readme 1.14）
//
// 设计：
//  * 分级：trace < debug < info < warn < error < off；默认 info。
//  * 分类（category）：http / api / engine / storage / plugin / import / download / cache / decrypt / eq。
//  * 多 sink：控制台（stderr）、文件（带大小轮转）、内存环形缓冲（供 GET /music/logs 与测试断言）。
//  * 单线程服务端：Logger 不做跨线程加锁；宿主若多线程使用，需在外部串行化（文档已注明）。
//  * 未显式初始化时不输出任何内容（避免库使用者被动产生日志），
//    调用 Logger::ensure_default_sinks() 或 configure() 后生效。
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "radmusic/error.hpp"
#include "radmusic/json.hpp"

namespace radmusic {

enum class LogLevel { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4, Off = 5 };

const char* log_level_id(LogLevel level);
LogLevel log_level_from_id(std::string_view id);

struct LogRecord {
    long long ts_ms = 0;          // Unix 毫秒
    LogLevel level = LogLevel::Info;
    std::string category;
    std::string message;
    std::string file;             // 产生日志的文件（基名）
    int line = 0;

    json::Value to_json() const;
};

class LogSink {
public:
    virtual ~LogSink() = default;
    virtual void write(const LogRecord& record) = 0;
    virtual void flush() {}
};

// 写 stderr（ISO 时间 + 级别 + 分类 + 消息）
class ConsoleLogSink : public LogSink {
public:
    explicit ConsoleLogSink(bool use_stdout = false) : use_stdout_(use_stdout) {}
    void write(const LogRecord& record) override;
    void flush() override;

private:
    bool use_stdout_ = false;
};

// 写文件，超过 max_bytes 时轮转成 <path>.1（只保留一份历史）
class FileLogSink : public LogSink {
public:
    FileLogSink(std::string path, std::uintmax_t max_bytes = 8ull * 1024 * 1024);
    ~FileLogSink() override;
    void write(const LogRecord& record) override;
    void flush() override;
    bool healthy() const { return healthy_; }
    const std::string& path() const { return path_; }

private:
    void rotate_if_needed();

    std::string path_;
    std::uintmax_t max_bytes_ = 8ull * 1024 * 1024;
    std::uintmax_t written_ = 0;
    bool healthy_ = false;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 内存环形缓冲（GET /music/logs 与测试用）
class MemoryLogSink : public LogSink {
public:
    explicit MemoryLogSink(size_t capacity = 500) : capacity_(capacity == 0 ? 500 : capacity) {}
    void write(const LogRecord& record) override;
    void clear() { records_.clear(); }
    const std::vector<LogRecord>& records() const { return records_; }
    size_t capacity() const { return capacity_; }

private:
    size_t capacity_;
    std::vector<LogRecord> records_;
};

class Logger {
public:
    static Logger& instance();

    void set_level(LogLevel level);
    LogLevel level() const { return level_; }
    bool enabled(LogLevel level) const { return level >= level_ && level_ != LogLevel::Off; }

    void add_sink(std::shared_ptr<LogSink> sink);
    void clear_sinks();
    const std::vector<std::shared_ptr<LogSink>>& sinks() const { return sinks_; }

    void log(LogLevel level, std::string_view category, std::string message, const char* file = nullptr,
             int line = 0);
    void info(std::string_view category, std::string message) {
        log(LogLevel::Info, category, std::move(message));
    }
    void warn(std::string_view category, std::string message) {
        log(LogLevel::Warn, category, std::move(message));
    }
    void error(std::string_view category, std::string message) {
        log(LogLevel::Error, category, std::move(message));
    }
    void debug(std::string_view category, std::string message) {
        log(LogLevel::Debug, category, std::move(message));
    }
    void trace(std::string_view category, std::string message) {
        log(LogLevel::Trace, category, std::move(message));
    }
    void flush();

private:
    Logger() = default;
    LogLevel level_ = LogLevel::Info;
    std::vector<std::shared_ptr<LogSink>> sinks_;
};

// 便捷配置：级别 + 控制台 + 可选文件（返回是否全部成功）
struct LogConfig {
    LogLevel level = LogLevel::Info;
    bool console = true;
    bool console_to_stdout = false;
    std::string file_path;                 // 空 = 不写文件
    std::uintmax_t max_file_bytes = 8ull * 1024 * 1024;
    bool memory = true;                    // 保留内存环形缓冲（GET /music/logs）
    size_t memory_capacity = 500;
};

Status log_configure(const LogConfig& config);

// 清空日志目录前调用：释放日志文件句柄（关闭并丢弃当前全部 sink）
void log_suspend_sinks();
// 按最近一次 log_configure 的配置重建 sink（日志文件随写入重新创建）
Status log_resume_sinks();

// 便捷取值：当前内存 sink（没有则返回 nullptr）
std::shared_ptr<MemoryLogSink> log_memory_sink();

// 全局内存缓冲（供 API 读取最近日志；log_configure 会替换它）
std::shared_ptr<MemoryLogSink> log_default_memory_sink();

std::string log_format_record(const LogRecord& record);

}  // namespace radmusic

// ---------------- 宏：带文件与行号 ----------------
#define RM_LOG(lvl, category, message) \
    ::radmusic::Logger::instance().log((lvl), (category), (message), __FILE__, __LINE__)
#define RM_LOG_TRACE(category, message) RM_LOG(::radmusic::LogLevel::Trace, category, message)
#define RM_LOG_DEBUG(category, message) RM_LOG(::radmusic::LogLevel::Debug, category, message)
#define RM_LOG_INFO(category, message) RM_LOG(::radmusic::LogLevel::Info, category, message)
#define RM_LOG_WARN(category, message) RM_LOG(::radmusic::LogLevel::Warn, category, message)
#define RM_LOG_ERROR(category, message) RM_LOG(::radmusic::LogLevel::Error, category, message)
