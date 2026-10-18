// Rad music API - 系统文件管理器（「打开所在文件夹」）
//
// 下载 / 导入落盘后，UI 需要「在文件管理器里显示这个文件」。宿主服务端与浏览器
// 同机，因此直接调用系统 Shell 打开资源管理器即可；浏览器端拿不到、也不需要真实路径。
//
// 平台：Windows 已实现（ShellExecuteW + explorer）。其他平台返回 1011（未实现）。
#pragma once

#include <string>

#include "radmusic/error.hpp"

namespace radmusic {

// 在系统文件管理器中显示 path：
//   * path 是文件 → 打开所在目录并选中该文件（Windows: explorer /select,"<file>"）
//   * path 是目录 → 直接打开该目录
// path 不存在返回 1004；平台未实现返回 1011。
Status reveal_in_file_manager(const std::string& path);

// path 是文件时返回所在目录，是目录时返回自身；取不到返回空串。
std::string containing_folder(const std::string& path);

}  // namespace radmusic
