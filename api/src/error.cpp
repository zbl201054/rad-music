#include "radmusic/error.hpp"

#include <cstring>

namespace radmusic {
namespace {

struct Entry {
    ErrorCode code;
    const char* id;
    const char* description;
    bool non_fatal;
};

const Entry kEntries[] = {
    {ErrorCode::Ok, "ok", "成功", false},
    {ErrorCode::PlatformError, "platform error", "platform 未知或无可用插件", false},
    {ErrorCode::ShaError, "sha error", "version + 文件 sha 与元数据不一致", true},
    {ErrorCode::UsecError, "usec error", "密钥缺失或与 sec.json 校验值不符", false},
    {ErrorCode::FileNotFound, "file not found", "location 指向的文件不存在", false},
    {ErrorCode::NotLoaded, "not loaded", "未成功加载即调用播放接口", false},
    {ErrorCode::DecryptFailed, "decrypt failed", "解密或认证失败", false},
    {ErrorCode::InvalidMetadata, "invalid metadata", "元数据或插件清单非法", false},
    {ErrorCode::UnsupportedVersion, "unsupported version", "version 超出当前实现支持范围", false},
    {ErrorCode::SearchNoSource, "search no source", "没有可用的播放源，或全部源搜索失败", false},
    {ErrorCode::SourceTimeout, "source timeout", "某个源超时", true},
    {ErrorCode::RequestFailed, "request failed", "request 执行失败", false},
    {ErrorCode::UnsupportedCapability, "unsupported capability", "插件未声明该 op", false},
    {ErrorCode::PluginDisabled, "plugin disabled", "插件被禁用、未加载或已崩溃隔离", false},
    {ErrorCode::RateLimited, "rate limited", "触发源并发或 QPS 上限", false},
    {ErrorCode::LyricUnavailable, "lyric unavailable", "本地与在线均无可用歌词", true},
    {ErrorCode::SmtcUnavailable, "smtc unavailable", "系统媒体控件注册/更新失败", true},
    {ErrorCode::DspInitFailed, "dsp init failed", "调音处理链或音效节点初始化失败", true},
    {ErrorCode::DownloadFailed, "download failed", "下载任务失败", false},
    {ErrorCode::InsufficientSpace, "insufficient space", "磁盘空间不足", false},
    {ErrorCode::TaskNotFound, "task not found", "任务 id 不存在", false},
    {ErrorCode::QualityUnsupported, "quality unsupported", "该源不支持目标音质/格式", false},
    {ErrorCode::ChecksumMismatch, "checksum mismatch", "下载完成校验失败", false},
    {ErrorCode::BaseConflict, "base conflict", "base.json 唯一性冲突", false},
    {ErrorCode::TagParseFailed, "tag parse failed", "标签解析失败，已回退文件名模板", true},
    {ErrorCode::UnsupportedFormat, "unsupported format", "音频格式不被支持", false},
    {ErrorCode::LibraryLocked, "library locked", "曲库被其他进程或任务占用", false},
    {ErrorCode::QualityUndefined, "quality undefined", "播放源未声明该音质", false},
    {ErrorCode::BaseRebuildFailed, "base rebuild failed", "base.json 缺失且重建失败", false},
    {ErrorCode::MetaPathMismatch, "meta path mismatch", "缓存目录名与 base.json 内容不一致", false},
    {ErrorCode::PlaylistNotFound, "playlist not found", "歌单不存在", false},
    {ErrorCode::AlbumNotFound, "album not found", "专辑不存在", false},
    {ErrorCode::MvNotFound, "mv not found", "MV 不存在", false},
};

const Entry* lookup(ErrorCode code) {
    for (const auto& e : kEntries) {
        if (e.code == code) return &e;
    }
    return nullptr;
}

}  // namespace

const char* error_id(ErrorCode code) {
    const Entry* e = lookup(code);
    return e ? e->id : "request failed";
}

const char* error_description(ErrorCode code) {
    const Entry* e = lookup(code);
    return e ? e->description : "未知错误";
}

bool is_non_fatal(ErrorCode code) {
    const Entry* e = lookup(code);
    return e ? e->non_fatal : false;
}

ErrorCode error_from_id(std::string_view id) {
    for (const auto& e : kEntries) {
        if (id.size() == std::strlen(e.id) && id == e.id) return e.code;
    }
    return ErrorCode::RequestFailed;
}

bool operator==(const Status& a, const Status& b) {
    return a.code == b.code && a.message == b.message;
}

}  // namespace radmusic
