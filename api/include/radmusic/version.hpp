// Rad music API - 版本号（检查更新用）
//
// 版本字符串在构建时注入（CMake/Makefile 的 RADMUSIC_VERSION 宏），
// 发布流水线用 git tag 生成，未注入时回落到 dev 标记。
#pragma once

#ifndef RADMUSIC_VERSION
#define RADMUSIC_VERSION "0.12.1-canary"
#endif

// 检查更新默认查询的 GitHub 仓库（owner/name）。可用构建宏覆盖。
#ifndef RADMUSIC_UPDATE_REPO
#define RADMUSIC_UPDATE_REPO "zbl201054/rad-music"
#endif

namespace radmusic {

inline const char* version_string() {
    return RADMUSIC_VERSION;
}

inline const char* update_repo() {
    return RADMUSIC_UPDATE_REPO;
}

// 版本字符串转可比较的分段（主版本.次版本.修订，忽略前缀 v 与预发布后缀）。
// 返回 false 表示无法解析。
bool parse_version(const char* text, int out[3]);

// a > b 返回 1，相等 0，a < b 返回 -1；无法解析的一方按更旧处理。
int compare_version(const char* a, const char* b);

}  // namespace radmusic
