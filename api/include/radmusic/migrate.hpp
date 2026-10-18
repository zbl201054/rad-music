// Rad music API - 数据迁移（设置页「存储位置 / 迁移数据」）
//
// 把整个数据根目录（曲目 + 缓存 + 设置 + 索引…都由 --root 派生）复制到一个新位置。
// 设计要点：
//   * 单线程模型：由宿主 idle-hook 每次调 step() 推进，一次只拷一个分片（默认 16MB），
//     避免大文件（无损可达上百 MB）长时间阻塞 HTTP 事件循环；
//   * 边读边写、落临时文件再改名，中途取消/失败不会留下半成品覆盖目标；
//   * 单个文件拷贝失败（例如正在写的日志被占用）记入 failed_files 并继续，不中断整体迁移；
//   * 只「复制」，不删源（旧数据保留，用户确认新位置无误后可自行删除回收空间）。
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "radmusic/error.hpp"
#include "radmusic/json.hpp"
#include "radmusic/storage.hpp"

namespace radmusic {

enum class MigrateState { Idle, Running, Done, Failed, Canceled };
const char* migrate_state_id(MigrateState state);

struct MigrationStatus {
    MigrateState state = MigrateState::Idle;
    std::string source;                 // 源数据根（当前 root）
    std::string dest;                   // 目标数据根
    std::vector<std::pair<std::string, std::uintmax_t>> files;  // (绝对路径, 大小) 待拷贝队列
    std::size_t index = 0;              // 下一个待拷贝文件下标
    std::uintmax_t bytes_done = 0;
    std::uintmax_t bytes_total = 0;
    std::string current;                // 正在拷贝的文件（相对 root）
    std::vector<std::string> failed_files;  // 拷贝失败被跳过的文件（相对 root）
    std::string error;                  // 任务级错误（校验/启动失败）

    json::Value to_json() const;
};

class DataMigrator {
public:
    explicit DataMigrator(const LibraryPaths& paths);

    // 校验目标目录并建立拷贝队列；成功后 state=Running。dest 需为绝对路径、且不在源内/源不在其内。
    Status start(const std::string& dest);
    const MigrationStatus& status() const { return status_; }
    bool active() const { return status_.state == MigrateState::Running; }

    // 推进一个分片；返回是否有状态变化（供宿主决定是否保存/通知）
    bool step();
    void cancel();

    // 迁移结束（Done/Failed/Canceled）时回调，宿主据此写配置并重启切换
    std::function<void(const MigrationStatus&)> on_done;

private:
    const LibraryPaths& paths_;
    MigrationStatus status_;

    // 当前正在拷贝的文件（跨 step 保持打开，实现分片拷贝）
    std::ifstream in_;
    std::ofstream out_;
    std::filesystem::path cur_dst_;
    std::filesystem::path cur_tmp_;
    std::uintmax_t cur_size_ = 0;
    std::uintmax_t cur_offset_ = 0;
    std::string cur_rel_;
    std::vector<char> chunk_;   // 分片缓冲（堆上，避免大栈分配）

    bool open_current();                 // 打开 index 指向的文件，失败返回 false
    void close_current();                // 关闭当前流（不改名）
    bool finish_current();               // 收尾当前文件：flush + 改名，失败返回 false
    void finish(MigrateState state, const std::string& error);
};

}  // namespace radmusic
