#include "radmusic/migrate.hpp"

#include <array>
#include <system_error>

namespace radmusic {

namespace {
constexpr std::size_t kChunkBytes = 16u << 20;  // 每个 step 最多拷贝 16MB
}  // namespace

const char* migrate_state_id(MigrateState state) {
    switch (state) {
        case MigrateState::Idle: return "idle";
        case MigrateState::Running: return "running";
        case MigrateState::Done: return "done";
        case MigrateState::Failed: return "failed";
        case MigrateState::Canceled: return "canceled";
    }
    return "idle";
}

json::Value MigrationStatus::to_json() const {
    json::Value v = json::Value::object();
    v.set("state", migrate_state_id(state));
    v.set("source", source);
    v.set("dest", dest);
    v.set("files_done", static_cast<long long>(index));
    v.set("files_total", static_cast<long long>(files.size()));
    v.set("bytes_done", static_cast<long long>(bytes_done));
    v.set("bytes_total", static_cast<long long>(bytes_total));
    v.set("current", current);
    v.set("failed", static_cast<long long>(failed_files.size()));
    json::Value failed = json::Value::array();
    for (const auto& f : failed_files) failed.push_back(f);
    v.set("failed_files", failed);
    v.set("error", error);
    return v;
}

DataMigrator::DataMigrator(const LibraryPaths& paths) : paths_(paths), chunk_(kChunkBytes, '\0') {}

// 判断 child 是否位于 base 之内（两者需已 lexically_normal）
static bool is_within(const std::filesystem::path& base, const std::filesystem::path& child) {
    auto b = base.begin();
    auto c = child.begin();
    for (; b != base.end(); ++b, ++c) {
        if (c == child.end()) return false;
        if (*b != *c) return false;
    }
    return true;
}

Status DataMigrator::start(const std::string& dest) {
    if (status_.state == MigrateState::Running) {
        return Status(ErrorCode::RequestFailed, "已有迁移任务正在进行");
    }
    if (dest.empty()) return Status(ErrorCode::InvalidMetadata, "缺少目标目录");

    std::error_code ec;
    const std::filesystem::path src = std::filesystem::path(paths_.root).lexically_normal();
    std::filesystem::path dst = std::filesystem::path(dest);
    if (dst.is_relative()) dst = std::filesystem::absolute(dst, ec);
    dst = dst.lexically_normal();

    if (!std::filesystem::exists(src, ec) || ec) {
        return Status(ErrorCode::FileNotFound, "源数据目录不存在: " + src.u8string());
    }
    // 同一目录 / 互相嵌套一律拒绝
    if (src == dst) return Status(ErrorCode::InvalidMetadata, "目标目录不能与当前数据目录相同");
    if (is_within(src, dst)) return Status(ErrorCode::InvalidMetadata, "目标目录不能位于当前数据目录内部");
    if (is_within(dst, src)) return Status(ErrorCode::InvalidMetadata, "目标目录不能包含当前数据目录");

    if (std::filesystem::exists(dst, ec) && !ec) {
        if (!std::filesystem::is_directory(dst, ec) || ec) {
            return Status(ErrorCode::InvalidMetadata, "目标已存在且不是目录: " + dst.u8string());
        }
        if (!std::filesystem::is_empty(dst, ec) || ec) {
            return Status(ErrorCode::InvalidMetadata, "目标目录不为空，请选择一个空目录或尚不存在的新目录");
        }
    }
    std::filesystem::create_directories(dst, ec);
    if (ec) return Status(ErrorCode::RequestFailed, "无法创建目标目录: " + dst.u8string() + " (" + ec.message() + ")");

    MigrationStatus s;
    s.source = src.u8string();
    s.dest = dst.u8string();
    s.state = MigrateState::Running;
    std::filesystem::recursive_directory_iterator it{
        src, std::filesystem::directory_options::skip_permission_denied, ec};
    const std::filesystem::recursive_directory_iterator end;
    while (!ec && it != end) {
        std::error_code fec;
        if (it->is_regular_file(fec) && !fec) {
            const std::uintmax_t sz = it->file_size(fec);
            if (!fec) {
                s.files.emplace_back(it->path().u8string(), sz);
                s.bytes_total += sz;
            }
        }
        it.increment(ec);
        if (ec) ec.clear();  // 个别目录无权限时跳过，不中断建表
    }
    status_ = std::move(s);
    return Status::success();
}

bool DataMigrator::open_current() {
    const auto& file = status_.files[status_.index];
    const std::filesystem::path src(file.first);
    const std::filesystem::path rel = src.lexically_relative(std::filesystem::path(status_.source));
    cur_rel_ = rel.generic_u8string();
    cur_dst_ = std::filesystem::path(status_.dest) / rel;
    cur_tmp_ = cur_dst_;
    cur_tmp_ += ".migrating";
    cur_size_ = file.second;
    cur_offset_ = 0;
    status_.current = cur_rel_;

    std::error_code ec;
    std::filesystem::create_directories(cur_dst_.parent_path(), ec);
    in_.open(src, std::ios::binary);
    out_.open(cur_tmp_, std::ios::binary | std::ios::trunc);
    if (!in_ || !out_) {
        close_current();
        return false;
    }
    return true;
}

void DataMigrator::close_current() {
    if (in_.is_open()) in_.close();
    if (out_.is_open()) out_.close();
    std::error_code ec;
    if (!cur_tmp_.empty()) std::filesystem::remove(cur_tmp_, ec);
    cur_tmp_.clear();
}

bool DataMigrator::finish_current() {
    out_.flush();
    const bool ok = out_.good();
    if (in_.is_open()) in_.close();
    out_.close();
    if (!ok) {
        std::error_code ec;
        std::filesystem::remove(cur_tmp_, ec);
        return false;
    }
    std::error_code ec;
    std::filesystem::rename(cur_tmp_, cur_dst_, ec);
    cur_tmp_.clear();
    return !ec;
}

void DataMigrator::finish(MigrateState state, const std::string& error) {
    close_current();
    status_.state = state;
    status_.current.clear();
    if (!error.empty()) status_.error = error;
    if (on_done) on_done(status_);
}

bool DataMigrator::step() {
    if (status_.state != MigrateState::Running) return false;

    // 没有打开的文件：先开下一个（或宣告完成）
    if (!in_.is_open()) {
        if (status_.index >= status_.files.size()) {
            finish(MigrateState::Done, std::string());
            return true;
        }
        if (!open_current()) {
            // 单文件打不开（如被占用的日志）：记录并跳过，不中断整体迁移
            status_.failed_files.push_back(cur_rel_.empty() ? status_.files[status_.index].first : cur_rel_);
            status_.bytes_done += status_.files[status_.index].second;  // 计入已处理，保证进度可达 100%
            ++status_.index;
            return true;  // 下一 tick 继续
        }
    }

    in_.read(chunk_.data(), static_cast<std::streamsize>(chunk_.size()));
    const std::streamsize got = in_.gcount();
    if (got > 0) {
        out_.write(chunk_.data(), got);
        if (!out_.good()) {
            const std::string rel = cur_rel_;
            finish(MigrateState::Failed, "写入目标失败: " + rel);
            return true;
        }
        cur_offset_ += static_cast<std::uintmax_t>(got);
        status_.bytes_done += static_cast<std::uintmax_t>(got);
    }

    const bool eof = got < static_cast<std::streamsize>(chunk_.size());
    if (eof) {
        const std::string rel = cur_rel_;
        if (!finish_current()) {
            finish(MigrateState::Failed, "收尾目标失败: " + rel);
            return true;
        }
        ++status_.index;
        status_.current.clear();
        cur_size_ = 0;
        cur_offset_ = 0;
        if (status_.index >= status_.files.size()) {
            finish(MigrateState::Done, std::string());
        }
    }
    return true;
}

void DataMigrator::cancel() {
    if (status_.state == MigrateState::Running) {
        finish(MigrateState::Canceled, "已被用户取消");
    }
}

}  // namespace radmusic
