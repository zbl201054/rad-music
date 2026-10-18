// Rad music API - 错误码与结果类型（对应 Readme.md 第 3 节）
#pragma once

#include <optional>
#include <string>
#include <utility>

namespace radmusic {

enum class ErrorCode : int {
    Ok = 0,
    PlatformError = 1001,          // platform 未知或无可用插件
    ShaError = 1002,               // version + 文件 sha 与元数据不一致（原文为“警告”）
    UsecError = 1003,              // 密钥缺失或与 sec.json 校验值不符
    FileNotFound = 1004,           // location 指向的文件不存在
    NotLoaded = 1005,              // 未成功加载即调用播放接口
    DecryptFailed = 1006,          // 解密或认证失败
    InvalidMetadata = 1007,        // base.json 字段缺失/JSON 非法/清单与类型矛盾
    UnsupportedVersion = 1008,     // version 超出支持范围
    SearchNoSource = 1009,         // 没有任何可用播放源，或全部源搜索失败
    SourceTimeout = 1010,          // 某个源超时
    RequestFailed = 1011,          // request API 执行失败
    UnsupportedCapability = 1012,  // 插件未声明该 op
    PluginDisabled = 1013,         // 插件被禁用/未加载/已崩溃隔离
    RateLimited = 1014,            // 触发源并发或 QPS 上限
    LyricUnavailable = 1015,       // 本地与在线均无可用歌词（非致命）
    SmtcUnavailable = 1016,        // 系统媒体控件注册/更新失败（非致命）
    DspInitFailed = 1017,          // 调音链或音效节点初始化失败（非致命）
    DownloadFailed = 1018,         // 下载任务失败
    InsufficientSpace = 1019,      // 磁盘空间不足
    TaskNotFound = 1020,           // 任务 id 不存在
    QualityUnsupported = 1021,     // 该源不支持目标音质
    ChecksumMismatch = 1022,       // 下载完成校验失败
    BaseConflict = 1023,           // base.json 唯一性冲突
    TagParseFailed = 1024,         // 标签解析失败，已回退文件名（非致命）
    UnsupportedFormat = 1025,      // 音频格式不支持
    LibraryLocked = 1026,          // 曲库被占用
    QualityUndefined = 1027,       // 插件未声明该音质
    BaseRebuildFailed = 1028,      // base.json 缺失且重建失败
    MetaPathMismatch = 1029,       // 缓存目录名与 base.json 内容不一致
    PlaylistNotFound = 1030,       // 歌单不存在（v0.10）
    AlbumNotFound = 1031,          // 专辑不存在（v0.10）
    MvNotFound = 1032,             // MV 不存在（v0.10）
};

// base.json / API 中的历史错误字符串（如 "platform error"）
const char* error_id(ErrorCode code);
// 中文描述
const char* error_description(ErrorCode code);
// 是否可继续播放（非致命）：1002/1015/1016/1017/1024
bool is_non_fatal(ErrorCode code);
// 由字符串（如 "platform error"）反查错误码，未知返回 ErrorCode::RequestFailed
ErrorCode error_from_id(std::string_view id);

struct Status {
    ErrorCode code = ErrorCode::Ok;
    std::string message;

    Status() = default;
    Status(ErrorCode c, std::string msg = std::string())
        : code(c), message(msg.empty() ? std::string(error_description(c)) : std::move(msg)) {}

    bool ok() const { return code == ErrorCode::Ok; }
    explicit operator bool() const { return ok(); }

    static Status success() { return Status{}; }
    static Status error(ErrorCode c, std::string msg = std::string()) { return Status(c, std::move(msg)); }
    // API 响应里的 message：优先自定义信息，其次标准 id
    std::string response_message() const {
        return message.empty() ? std::string(error_id(code)) : message;
    }
};

bool operator==(const Status& a, const Status& b);
inline bool operator!=(const Status& a, const Status& b) { return !(a == b); }

template <typename T>
class Result {
public:
    Result(T value) : status_(), value_(std::move(value)) {}
    Result(Status status) : status_(std::move(status)) {}

    bool ok() const { return status_.ok() && value_.has_value(); }
    explicit operator bool() const { return ok(); }
    const Status& status() const { return status_; }

    T& value() { return *value_; }
    const T& value() const { return *value_; }
    T value_or(T fallback) const { return value_.has_value() ? *value_ : std::move(fallback); }

    static Result success(T value) { return Result(std::move(value)); }
    static Result failure(ErrorCode code, std::string msg = std::string()) {
        return Result(Status(code, std::move(msg)));
    }

private:
    Status status_;
    std::optional<T> value_;
};

}  // namespace radmusic
