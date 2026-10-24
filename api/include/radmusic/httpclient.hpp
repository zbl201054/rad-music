// Rad music API - 网络传输（仅用于下载落盘与更新检查，零第三方依赖）
//
// Windows 上走 WinHTTP（系统组件，需链接 winhttp.lib），支持 HTTPS 与 Range 断点续传；
// 其他平台用系统 curl 子进程（无 shell，argv 直传，避免路径转义问题）。
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace radmusic {

// 是否为 http(s) 地址
bool is_remote_url(const std::string& url);

// 把 url 下载到 out_path：
//   * received 传入已下载字节数（>0 时带 Range 续传），实际进度回写；
//   * total 回写总字节数（服务器未给 Content-Length 时为 -1）；
//   * max_bytes > 0 时本次最多下载该字节数（分片下载：进度平滑、不长时间占住主线程），
//     到量即返回 true，由调用方根据 received/total 判断是否还需要下一片；
//   * 失败返回 false，error 填原因（含 "insufficient space" 时上层判定为 1019）。
bool http_fetch_file(const std::string& url, const std::filesystem::path& out_path,
                     long long* received, long long* total, std::string* error,
                     long long max_bytes = 0);

// 取一个小的文本响应（GitHub API 等）。extra_headers 形如 {"Accept", "application/json"}。
// 响应超过 max_bytes 视为失败（防止把大文件读进内存）。
bool http_fetch_text(const std::string& url, const std::vector<std::pair<std::string, std::string>>& extra_headers,
                     std::string& out, std::string* error, long long max_bytes = 8 * 1024 * 1024);

// 下载 [start, end] 字节区间，写到 out_path 的同一偏移（多线程分段下载用，各段互不重叠）。
//   * 文件必须已存在且长度 >= end+1（调用方先预分配）；
//   * on_progress 回报「本段已写入字节数」（单调递增，从 0 起算），返回 false 可中止本段；
//   * 服务器忽略 Range 返回 200 时返回 false，error 说明不支持分段下载（调用方应退回单线程）。
// 本函数是线程安全的：每次调用自建连接与文件句柄，可在多个线程里并发调用同一个 url + path。
bool http_fetch_segment(const std::string& url, const std::filesystem::path& out_path, long long start, long long end,
                        const std::function<bool(long long)>& on_progress, std::string* error);

}  // namespace radmusic
