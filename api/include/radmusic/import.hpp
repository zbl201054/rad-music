#pragma once
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "radmusic/api.hpp"      // ImportService
#include "radmusic/error.hpp"
#include "radmusic/json.hpp"
#include "radmusic/storage.hpp"
#include "radmusic/tags.hpp"

namespace radmusic {

struct ImportOptions {
    std::vector<std::string> paths;
    bool recursive = true;
    std::string mode = "copy";                 // copy / move / link
    ConflictPolicy conflict = ConflictPolicy::Skip;
    std::string tag_fallback = "filename";
    std::string filename_template = "{artist} - {title}";
    bool dry_run = false;

    static ImportOptions from_json(const json::Value& v);
    json::Value to_json() const;
};

struct ImportConflict {
    std::string type;        // duplicate_sha / same_song_same_quality / location / tag_parse ...
    std::string sha;
    std::string existing;    // 既有 base.json 路径
    std::string path;        // 本次导入的源文件
    std::string action;      // skip / merge / overwrite / failed
    ErrorCode code = ErrorCode::Ok;
    std::string message;

    json::Value to_json() const;
};

struct ImportItemResult {
    std::string path;
    std::string sha;
    std::string song_key;
    std::string quality_key;
    std::string action;      // created / merged / skipped / overwritten / failed
    ErrorCode code = ErrorCode::Ok;
    std::string message;

    json::Value to_json() const;
};

struct ImportReport {
    std::string import_id;
    int scanned = 0;
    int imported = 0;
    int skipped = 0;
    int failed = 0;
    bool dry_run = false;
    std::vector<ImportConflict> conflicts;
    std::vector<ImportItemResult> items;

    json::Value to_json() const;
};

// 实现 api.hpp 中的 ImportService
class ImportManager : public ImportService {
public:
    ImportManager(const LibraryPaths& paths, MetaCache& meta, LibraryIndex& index);

    // 依赖注入（默认走 tags.cpp 的真实实现；测试可替换）
    std::function<Result<TagData>(const std::string&)> read_tags_fn;
    std::function<Result<AudioStreamInfo>(const std::string&)> probe_audio_fn;
    std::function<std::string(const std::string& path)> sha_fn;   // 默认 crypto::sha256_file_hex

    // ImportService
    Result<json::Value> scan(const json::Value& request) override;
    Result<json::Value> run(const json::Value& request) override;
    Result<json::Value> status(std::string_view import_id) override;
    Result<json::Value> report(std::string_view import_id) override;

    const std::vector<ImportReport>& reports() const { return reports_; }

private:
    // 实际执行体（run() 取锁后调用；dry_run 时不取锁但仍复用同一实现）
    Result<json::Value> run_locked(const ImportOptions& opt);

    const LibraryPaths& paths_;
    MetaCache& meta_;
    LibraryIndex& index_;
    std::vector<ImportReport> reports_;
    int next_id_ = 1;
};
}
