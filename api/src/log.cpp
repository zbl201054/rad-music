#include "radmusic/log.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace radmusic {
namespace {

long long now_ms() {
    return static_cast<long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

std::string base_name(const char* path) {
    if (path == nullptr) return std::string();
    const std::string text(path);
    const size_t slash = text.find_last_of("/\\");
    return slash == std::string::npos ? text : text.substr(slash + 1);
}

std::string format_timestamp(long long ts_ms) {
    const std::time_t seconds = static_cast<std::time_t>(ts_ms / 1000);
    const int millis = static_cast<int>(ts_ms % 1000);
    std::tm tm_value{};
#ifdef _WIN32
    gmtime_s(&tm_value, &seconds);
#else
    gmtime_r(&seconds, &tm_value);
#endif
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm_value.tm_year + 1900,
                  tm_value.tm_mon + 1, tm_value.tm_mday, tm_value.tm_hour, tm_value.tm_min, tm_value.tm_sec,
                  millis);
    return std::string(buffer);
}

std::shared_ptr<MemoryLogSink>& shared_memory_sink() {
    static std::shared_ptr<MemoryLogSink> sink;
    return sink;
}

}  // namespace

const char* log_level_id(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "trace";
        case LogLevel::Debug: return "debug";
        case LogLevel::Info: return "info";
        case LogLevel::Warn: return "warn";
        case LogLevel::Error: return "error";
        default: return "off";
    }
}

LogLevel log_level_from_id(std::string_view id) {
    if (id == "trace") return LogLevel::Trace;
    if (id == "debug") return LogLevel::Debug;
    if (id == "info") return LogLevel::Info;
    if (id == "warn" || id == "warning") return LogLevel::Warn;
    if (id == "error") return LogLevel::Error;
    if (id == "off" || id == "none") return LogLevel::Off;
    return LogLevel::Info;
}

json::Value LogRecord::to_json() const {
    json::Value value = json::Value::object();
    value.set("ts_ms", ts_ms);
    value.set("time", format_timestamp(ts_ms));
    value.set("level", std::string(log_level_id(level)));
    value.set("category", category);
    value.set("message", message);
    if (!file.empty()) value.set("file", file);
    if (line > 0) value.set("line", static_cast<long long>(line));
    return value;
}

std::string log_format_record(const LogRecord& record) {
    std::string out = format_timestamp(record.ts_ms);
    out += " [";
    out += log_level_id(record.level);
    out += "] [";
    out += record.category.empty() ? "general" : record.category;
    out += "] ";
    out += record.message;
    if (!record.file.empty()) {
        out += " (";
        out += record.file;
        if (record.line > 0) {
            out += ":";
            out += std::to_string(record.line);
        }
        out += ")";
    }
    return out;
}

// ---------------------------------------------------------------- ConsoleLogSink

void ConsoleLogSink::write(const LogRecord& record) {
    const std::string line = log_format_record(record) + "\n";
    std::FILE* stream = use_stdout_ ? stdout : stderr;
    std::fwrite(line.data(), 1, line.size(), stream);
}

void ConsoleLogSink::flush() { std::fflush(use_stdout_ ? stdout : stderr); }

// ---------------------------------------------------------------- FileLogSink

struct FileLogSink::Impl {
    std::ofstream stream;
};

FileLogSink::FileLogSink(std::string path, std::uintmax_t max_bytes)
    : path_(std::move(path)), max_bytes_(max_bytes == 0 ? 8ull * 1024 * 1024 : max_bytes), impl_(new Impl()) {
    std::error_code ec;
    const std::filesystem::path file(path_);
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    impl_->stream.open(path_, std::ios::binary | std::ios::app);
    healthy_ = impl_->stream.good();
    if (healthy_) {
        const auto size = std::filesystem::file_size(path_, ec);
        if (!ec) written_ = static_cast<std::uintmax_t>(size);
    }
}

FileLogSink::~FileLogSink() { flush(); }

void FileLogSink::rotate_if_needed() {
    if (!healthy_ || written_ < max_bytes_) return;
    impl_->stream.close();
    std::error_code ec;
    const std::filesystem::path file(path_);
    const std::filesystem::path backup(path_ + ".1");
    std::filesystem::remove(backup, ec);
    std::filesystem::rename(file, backup, ec);
    impl_->stream.open(path_, std::ios::binary | std::ios::trunc);
    healthy_ = impl_->stream.good();
    written_ = 0;
}

void FileLogSink::write(const LogRecord& record) {
    if (!healthy_) return;
    rotate_if_needed();
    const std::string line = log_format_record(record) + "\n";
    impl_->stream.write(line.data(), static_cast<std::streamsize>(line.size()));
    impl_->stream.flush();
    written_ += line.size();
    if (!impl_->stream.good()) healthy_ = false;
}

void FileLogSink::flush() {
    if (impl_ && impl_->stream.is_open()) impl_->stream.flush();
}

// ---------------------------------------------------------------- MemoryLogSink

void MemoryLogSink::write(const LogRecord& record) {
    if (records_.size() >= capacity_) records_.erase(records_.begin());
    records_.push_back(record);
}

// ---------------------------------------------------------------- Logger

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::set_level(LogLevel level) { level_ = level; }

void Logger::add_sink(std::shared_ptr<LogSink> sink) {
    if (!sink) return;
    sinks_.push_back(std::move(sink));
}

void Logger::clear_sinks() { sinks_.clear(); }

void Logger::log(LogLevel level, std::string_view category, std::string message, const char* file, int line) {
    if (!enabled(level)) return;
    LogRecord record;
    record.ts_ms = now_ms();
    record.level = level;
    record.category = std::string(category);
    record.message = std::move(message);
    record.file = base_name(file);
    record.line = line;
    for (auto& sink : sinks_) {
        if (sink) sink->write(record);
    }
}

void Logger::flush() {
    for (auto& sink : sinks_) {
        if (sink) sink->flush();
    }
}

// ---------------------------------------------------------------- 配置

namespace {

// 记住最近一次配置，供 log_resume_sinks 重建 sink（清空日志目录后恢复）
LogConfig g_last_config;
bool g_has_config = false;

}  // namespace

Status log_configure(const LogConfig& config) {
    g_last_config = config;
    g_has_config = true;

    Logger& logger = Logger::instance();
    logger.clear_sinks();
    logger.set_level(config.level);

    if (config.console) {
        logger.add_sink(std::make_shared<ConsoleLogSink>(config.console_to_stdout));
    }
    if (config.memory) {
        auto memory = std::make_shared<MemoryLogSink>(config.memory_capacity);
        logger.add_sink(memory);
        shared_memory_sink() = memory;
    } else {
        shared_memory_sink().reset();
    }
    if (!config.file_path.empty()) {
        auto file = std::make_shared<FileLogSink>(config.file_path, config.max_file_bytes);
        if (!file->healthy()) {
            return Status(ErrorCode::RequestFailed, "无法打开日志文件: " + config.file_path);
        }
        logger.add_sink(file);
    }
    return Status::success();
}

std::shared_ptr<MemoryLogSink> log_memory_sink() { return shared_memory_sink(); }

std::shared_ptr<MemoryLogSink> log_default_memory_sink() { return shared_memory_sink(); }

void log_suspend_sinks() {
    // shared_ptr 释放即触发 FileLogSink 析构，Windows 上文件句柄随之关闭
    Logger::instance().clear_sinks();
}

Status log_resume_sinks() {
    if (!g_has_config) return Status::success();
    return log_configure(g_last_config);
}

}  // namespace radmusic
