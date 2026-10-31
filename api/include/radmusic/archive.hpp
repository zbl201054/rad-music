// Rad music API - 归档解压（检查更新用，零第三方依赖）
//
// 只实现发布包需要的两种形态：
//   * zip（Windows 发布包，stored + deflate）
//   * tar.gz（Linux/macOS 发布包，gzip + ustar/GNU/pax 普通文件）
//
// 安全约束（不可放宽）：
//   * 拒绝绝对路径、盘符、`..` 段（zip slip）；
//   * 拒绝符号链接与硬链接（发布包里不该有，解压出来有风险）；
//   * 只写 dest_dir 之下的文件，不允许越界。
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace radmusic {
namespace archive {

struct ExtractOptions {
    // 发布包顶层是一个 rad-music-vX-slug/ 目录：自动去掉这一层（所有条目共享同一顶层目录时）
    bool strip_single_root = true;
};

struct ExtractResult {
    std::string error;      // 空 = 成功
    size_t files = 0;
    size_t skipped = 0;

    bool ok() const { return error.empty(); }
};

// 原始 deflate 解压（RFC 1951，无 zlib 头）；测试直接用它。
bool inflate_raw(const unsigned char* in, size_t in_size, std::vector<unsigned char>& out, std::string* error);
// gzip 解压（跳过 10+ 字节头与 8 字节尾）
bool gunzip(const std::vector<unsigned char>& in, std::vector<unsigned char>& out, std::string* error);

// 相对路径是否安全（用于校验归档内条目名与最终落盘路径）
bool is_safe_relative_path(const std::string& path);

ExtractResult extract_zip(const std::filesystem::path& zip_path, const std::filesystem::path& dest_dir,
                          const ExtractOptions& options = ExtractOptions());
// 流式 zip 解压（大文件友好）：安全策略与 extract_zip 完全一致，
// 区别是 stored 条目按块拷贝、deflate 条目只把「单个条目」读进内存。
// 曲库音频包（.rdm，可能几百 MB）必须走这个入口，否则会一次性占满内存。
ExtractResult extract_zip_streamed(const std::filesystem::path& zip_path, const std::filesystem::path& dest_dir,
                                   const ExtractOptions& options = ExtractOptions());
ExtractResult extract_tar_gz(const std::filesystem::path& tar_gz_path, const std::filesystem::path& dest_dir,
                             const ExtractOptions& options = ExtractOptions());

// ---------------- zip 写入（曲库音频包 .rdm 用） ----------------
// 只写 stored（不压缩）条目：音频本身压不动，省掉 deflate 实现与 CPU。
struct ZipWriteItem {
    std::string name;                   // 归档内路径，强制 UTF-8 且用 / 分隔（如 "music/001 - a.flac"）
    std::string data;                   // 内存内容（与 path 二选一，data 非空优先）
    std::filesystem::path path;         // 磁盘文件：分两遍读（先算 CRC 再拷贝），峰值内存与文件大小无关
};

// 原子写：先写 <target>.part 再替换；任何失败都清理临时文件并返回 false。
// 条目名非法（绝对路径 / .. / 反斜杠）或单文件超过 4 GiB（zip32 上限）时直接失败。
bool write_zip(const std::filesystem::path& target, const std::vector<ZipWriteItem>& items, std::string* error);
// 按扩展名分派：.zip → extract_zip，.tar.gz/.tgz → extract_tar_gz
ExtractResult extract_archive(const std::filesystem::path& path, const std::filesystem::path& dest_dir,
                              const ExtractOptions& options = ExtractOptions());

}  // namespace archive
}  // namespace radmusic
